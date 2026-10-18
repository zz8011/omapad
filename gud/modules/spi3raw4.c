// SPDX-License-Identifier: GPL-2.0
/*
 * spi3raw: Apple Samsung SPI v1（spi-version 1）裸寄存器 PIO 测试工具，J207 触摸 SPI3 用
 *
 * 时序照 iOS 17 AppleSamsungSPIController（见 .scratch/touch-j207/FINDINGS.md）：
 *   RXCNT/TXCNT = 总长 → CLKDIV → WORD_DELAY → STATUS 清除 → CFG → CTRL=0xd
 *   → 片选拉低 → CFG|0x20 → 填 TXDATA → 循环（STATUS 读后写回，收 RX、补 TX）
 *
 * 不碰电源/时钟：PMGR ps_spi3 实际状态不是 0xf 时拒绝访问 SPI 寄存器（未上电访问可能 SError）。
 * 内核没开 MODULE_UNLOAD，所以参数都可运行时改：/sys/module/spi3raw/parameters/
 *
 * 接口：/sys/kernel/spi3raw/cmd（写）  /sys/kernel/spi3raw/out（读上次输出）
 *   regs                       读控制器寄存器
 *   xfer <b0> <b1> ... [: n]   全双工，总长 max(发送字节数, n)，不足补 0xff，结束后释放片选
 *   xferk ...                  同上但保持片选
 *   cs 0|1                     直接设 GPIO 电平（不受 cs_mode 影响）
 * /sys/kernel/spi3raw/tx（二进制写）：写入的原始字节保持片选发出，out 只记最后 16 字节 RX；结束后需 cs 1
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/hex.h>

#define SPI_CTRL	0x00
#define SPI_CFG		0x04
#define SPI_STATUS	0x08
#define SPI_PIN		0x0c
#define SPI_TXDATA	0x10
#define SPI_RXDATA	0x20
#define SPI_CLKDIV	0x30
#define SPI_RXCNT	0x34
#define SPI_WDELAY	0x38
#define SPI_TXCNT	0x4c

#define V1_CFG_BASE	0x4000
#define V1_STATUS_CLR	0x40000f
#define V1_FIFO		16
#define MAXLEN		16384

static ulong spi_base = 0x20a08c000;	/* arm-io spi3 */
static ulong ps_reg = 0x20e0801d8;	/* PMGR ps_spi3 */
static ulong cs_reg = 0x20f10032c;	/* GPIO203，低有效 */
static uint clkdiv = 0x7ff;		/* nclk 未知，默认最慢 */
static uint wdelay = 2;
static uint mode = 0;			/* bit0 CPHA，bit1 CPOL */
static uint bits_per_word = 8;		/* 8 或 16 bit 传输 */
static uint timeout_ms = 200;
static uint cs_setup_us = 0;		/* 片选拉低 → 首个时钟 */
static uint cs_hold_us = 0;		/* 末个时钟 → 片选拉高 */
static uint cs_mode = 0;		/* 0 低有效，1 高有效，2 传输时不动片选 */
module_param(spi_base, ulong, 0644);
module_param(ps_reg, ulong, 0644);
module_param(cs_reg, ulong, 0644);
module_param(clkdiv, uint, 0644);
module_param(wdelay, uint, 0644);
module_param(mode, uint, 0644);
module_param(bits_per_word, uint, 0644);
module_param(timeout_ms, uint, 0644);
module_param(cs_setup_us, uint, 0644);
module_param(cs_hold_us, uint, 0644);
module_param(cs_mode, uint, 0644);

#define OUT_SZ	PAGE_SIZE

static DEFINE_MUTEX(lock);
static char out_buf[OUT_SZ];
static int out_len;
static struct kobject *kobj;
static u8 txb[MAXLEN], rxb[MAXLEN];

static __printf(1, 2) void o(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (out_len >= OUT_SZ - 100)
		return;
	va_start(ap, fmt);
	n = vscnprintf(out_buf + out_len, OUT_SZ - out_len - 1, fmt, ap);
	va_end(ap);
	if (n > 0)
		out_len += n;
}

static int parse_bytes(const char *s, u8 *buf, int max)
{
	int n = 0;

	while (*s && n < max) {
		unsigned int v = 0;
		int digits = 0;

		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			s += 2;
		while (isxdigit(*s) && digits < 2) {
			v = v * 16 + hex_to_bin(*s);
			s++;
			digits++;
		}
		if (!digits)
			break;
		buf[n++] = v;
	}
	return n;
}

/* 每次按需映射，参数改了立即生效 */
static void __iomem *map(ulong pa)
{
	return ioremap(pa, 4);
}

static bool powered(void)
{
	void __iomem *p = map(ps_reg);
	u32 v;

	if (!p)
		return false;
	v = readl(p);
	iounmap(p);
	if (((v >> 4) & 0xf) != 0xf) {
		o("ps_reg %#lx = %08x，SPI 未上电，拒绝访问\n", ps_reg, v);
		return false;
	}
	return true;
}

static void set_cs_raw(int level);

static void set_cs(int level)
{
	if (cs_mode == 2)
		return;
	if (cs_mode == 1)
		level = !level;
	set_cs_raw(level);
}

/* 直接写 GPIO 电平，不受 cs_mode 影响 */
static void set_cs_raw(int level)
{
	void __iomem *p = map(cs_reg);
	u32 v;

	if (!p)
		return;
	v = readl(p);
	writel((v & ~1u) | (level & 1), p);
	iounmap(p);
}

static void dump(void __iomem *b, const char *tag)
{
	u32 st = readl(b + SPI_STATUS);

	o("%s: CTRL=%08x CFG=%08x STATUS=%08x(tx=%u rx=%u) PIN=%08x DIV=%08x RXCNT=%08x WD=%08x TXCNT=%08x\n",
	  tag, readl(b + SPI_CTRL), readl(b + SPI_CFG), st, (st >> 6) & 0x1f,
	  (st >> 11) & 0x1f, readl(b + SPI_PIN), readl(b + SPI_CLKDIV),
	  readl(b + SPI_RXCNT), readl(b + SPI_WDELAY), readl(b + SPI_TXCNT));
}

static void do_xfer(void __iomem *b, int txlen, int total, bool keep, bool quiet)
{
	u32 cfg = V1_CFG_BASE | 0x18 | (mode & 1 ? 0x2 : 0) | (mode & 2 ? 0x4 : 0);
	int tx = 0, rx = 0, i;
	ktime_t end;
	bool use_16bit = (bits_per_word == 16);

	/* 16bit 模式：设置 WORD_SIZE bit (bit 6, 0x40) */
	if (use_16bit)
		cfg |= 0x40;

	for (i = txlen; i < total; i++)
		txb[i] = 0xff;

	writel(total, b + SPI_RXCNT);
	writel(total, b + SPI_TXCNT);
	writel(clkdiv, b + SPI_CLKDIV);
	writel(wdelay, b + SPI_WDELAY);
	writel(V1_STATUS_CLR, b + SPI_STATUS);
	writel(cfg, b + SPI_CFG);
	writel(0xd, b + SPI_CTRL);
	/* CPOL 已在 CFG 中，先让 SCK 稳定在空闲电平再拉低片选 */
	udelay(2);
	set_cs(0);
	if (cs_setup_us)
		udelay(cs_setup_us);
	writel(cfg | 0x20, b + SPI_CFG);

	/* 16bit 模式下每次传输 2 字节 */
	if (use_16bit) {
		while (tx < total && tx < V1_FIFO) {
			u32 word = txb[tx];
			if (tx + 1 < total)
				word |= (txb[tx + 1] << 8);
			writel(word, b + SPI_TXDATA);
			tx += 2;
		}
	} else {
		while (tx < total && tx < V1_FIFO)
			writel(txb[tx++], b + SPI_TXDATA);
	}

	writel(cfg | 0x20 | (tx >= total ? 0x80 : 0x100), b + SPI_CFG);
	if (!quiet)
		dump(b, "start");

	end = ktime_add_ms(ktime_get(), timeout_ms);
	while (rx < total) {
		u32 st = readl(b + SPI_STATUS);
		int rxl = (st >> 11) & 0x1f, txfree = V1_FIFO - ((st >> 6) & 0x1f);

		writel(st, b + SPI_STATUS);

		/* 16bit 模式下每次接收 2 字节 */
		if (use_16bit) {
			while (rxl-- > 0 && rx < total) {
				u32 word = readl(b + SPI_RXDATA);
				rxb[rx++] = word & 0xff;
				if (rx < total)
					rxb[rx++] = (word >> 8) & 0xff;
			}
			while (txfree-- > 0 && tx < total) {
				u32 word = txb[tx];
				if (tx + 1 < total)
					word |= (txb[tx + 1] << 8);
				writel(word, b + SPI_TXDATA);
				tx += 2;
			}
		} else {
			while (rxl-- > 0 && rx < total)
				rxb[rx++] = readl(b + SPI_RXDATA);
			while (txfree-- > 0 && tx < total)
				writel(txb[tx++], b + SPI_TXDATA);
		}

		if (ktime_after(ktime_get(), end)) {
			o("超时：tx %d / rx %d / 总 %d\n", tx, rx, total);
			break;
		}
		udelay(5);
	}
	if (!quiet)
		dump(b, "end");
	writel(cfg, b + SPI_CFG);
	writel(V1_STATUS_CLR, b + SPI_STATUS);
	if (rx < total)
		writel(0, b + SPI_CTRL);
	if (!keep) {
		if (cs_hold_us)
			udelay(cs_hold_us);
		set_cs(1);
	}

	o("rx[%d]:", rx);
	for (i = quiet ? max(rx - 16, 0) : 0; i < rx; i++)
		o("%s%02x", i % 32 ? " " : "\n ", rxb[i]);
	o("\n");
}

static ssize_t cmd_store(struct kobject *k, struct kobj_attribute *a,
			 const char *buf, size_t count)
{
	char *line, *rest;
	void __iomem *b = NULL;

	line = kstrdup(buf, GFP_KERNEL);
	if (!line)
		return -ENOMEM;
	mutex_lock(&lock);
	out_len = 0;
	out_buf[0] = '\0';
	strim(line);
	rest = strchr(line, ' ');
	if (rest)
		*rest++ = '\0';
	else
		rest = "";

	if (!strcmp(line, "cs")) {
		set_cs_raw(rest[0] == '1');
		o("cs=%c\n", rest[0] == '1' ? '1' : '0');
		goto out;
	}
	if (strcmp(line, "regs") && strcmp(line, "xfer") && strcmp(line, "xferk")) {
		o("cmds: regs | xfer <b..> [: n] | xferk <b..> [: n] | cs 0|1\n");
		goto out;
	}
	if (!powered())
		goto out;
	b = ioremap(spi_base, 0x100);
	if (!b) {
		o("ioremap 失败\n");
		goto out;
	}
	if (!strcmp(line, "regs")) {
		dump(b, "regs");
	} else {
		char *c = strchr(rest, ':');
		int txlen, total = 0;

		if (c) {
			*c++ = '\0';
			total = simple_strtoul(c, NULL, 0);
		}
		txlen = parse_bytes(rest, txb, MAXLEN);
		total = clamp(max(total, txlen), 1, MAXLEN);
		do_xfer(b, txlen, total, !strcmp(line, "xferk"), false);
	}
	iounmap(b);
out:
	mutex_unlock(&lock);
	kfree(line);
	return count;
}
static struct kobj_attribute cmd_attr = __ATTR_WO(cmd);

static ssize_t out_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	int n;

	mutex_lock(&lock);
	n = min_t(int, out_len, PAGE_SIZE - 1);
	memcpy(buf, out_buf, n);
	buf[n] = '\0';
	mutex_unlock(&lock);
	return n;
}
static struct kobj_attribute out_attr = __ATTR_RO(out);

static ssize_t tx_write(struct file *f, struct kobject *k, const struct bin_attribute *a,
			char *buf, loff_t off, size_t count)
{
	void __iomem *b;
	int n = min_t(size_t, count, MAXLEN);

	mutex_lock(&lock);
	out_len = 0;
	out_buf[0] = '\0';
	if (!powered())
		goto out;
	b = ioremap(spi_base, 0x100);
	if (!b) {
		o("ioremap 失败\n");
		goto out;
	}
	memcpy(txb, buf, n);
	do_xfer(b, n, n, true, true);
	iounmap(b);
out:
	mutex_unlock(&lock);
	return n;
}
static const struct bin_attribute tx_attr = __BIN_ATTR_WO(tx, 0);

static struct attribute *attrs[] = { &cmd_attr.attr, &out_attr.attr, NULL };
static const struct bin_attribute *const bin_attrs[] = { &tx_attr, NULL };
static const struct attribute_group group = { .attrs = attrs, .bin_attrs = bin_attrs };

static int __init spi3raw_init(void)
{
	int ret;

	kobj = kobject_create_and_add("spi3raw4", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &group);
	if (ret) {
		kobject_put(kobj);
		return ret;
	}
	pr_info("spi3raw4: /sys/kernel/spi3raw4/{cmd,out}\n");
	return 0;
}
module_init(spi3raw_init);

MODULE_DESCRIPTION("Apple Samsung SPI v1 raw PIO test");
MODULE_LICENSE("GPL");
