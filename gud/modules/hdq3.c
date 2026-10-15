// SPDX-License-Identifier: GPL-2.0
/*
 * hdq3: BQ27540 HDQ 主控（直接寄存器访问版）
 *
 * 相较 hdq2 的关键改动：
 *   位操作不再走 GPIO 框架（gpio_direction_output/input 每次都要 regmap 加锁，
 *   有数微秒抖动，而 BQ27540 的 HDQ bit 窗口很窄 → 随机位错误），
 *   改为直接读写 pinctrl 寄存器（readl/writel，纳秒级）。
 *
 * 寄存器布局（drivers/pinctrl/pinctrl-apple-gpio.c）：
 *   REG_GPIO(x) = 4*x
 *     bit0      DATA（输入时=引脚电平；输出时=驱动值）
 *     bits3:1   MODE  1=OUT, 7=IN_IRQ_OFF
 *     bits6:5   PERIPH  0=GPIO 功能
 *     bits8:7   PULL
 *     bit9      INPUT_ENABLE
 *
 * HDQ 位编码（TI HDQ 规范）：
 *   逻辑 1 = 短低电平（~35µs）   逻辑 0 = 长低电平（~120µs）
 *   BREAK  >=190µs 低；bit 周期 ~190-205µs
 *
 * 接口：/sys/kernel/hdq3/{cmd,out}
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/io.h>

#define HDQ_GPIO_DEFAULT	556	/* 44 + 512 (GPIO_DYNAMIC_BASE) */
#define HDQ_PIN			44
#define PINCTRL_BASE		0x20e300000UL
#define PINCTRL_MAP_SIZE	0x1000
#define OUT_SZ			32768

#define REG_GPIO(x)		(4 * (x))
#define R_DATA			BIT(0)
#define R_MODE			GENMASK(3, 1)
#define R_PERIPH		GENMASK(6, 5)
#define R_INPUT_ENABLE		BIT(9)
#define R_MODE_OUT		1
#define R_MODE_IN_IRQ_OFF	7

static int hdq_gpio = HDQ_GPIO_DEFAULT;
static int hdq_pin = HDQ_PIN;
static int use_fast = 1;
static int t_break = 500, t_brk_rec = 100, t_w0_low = 120, t_w1_low = 40;
static int t_bit = 200, t_rd_low = 2, t_rd_samp = 50;

static bool hdq_ok;
static void __iomem *pc;
static char out_buf[OUT_SZ];
static int out_len;
static struct kobject *kobj;

static void o_reset(void) { out_len = 0; out_buf[0] = '\0'; }

static void o(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (out_len >= OUT_SZ - 300)
		return;
	va_start(ap, fmt);
	n = vscnprintf(out_buf + out_len, OUT_SZ - out_len - 1, fmt, ap);
	va_end(ap);
	if (n > 0)
		out_len += n;
}

/* ---------- 快速路径：直接寄存器 ---------- */
static inline void f_low(void)
{
	u32 v = readl(pc + REG_GPIO(hdq_pin));

	v = (v & ~(R_PERIPH | R_MODE | R_DATA)) | (R_MODE_OUT << 1);
	writel(v, pc + REG_GPIO(hdq_pin));
}

static inline void f_rel(void)
{
	u32 v = readl(pc + REG_GPIO(hdq_pin));

	v = (v & ~(R_PERIPH | R_MODE | R_INPUT_ENABLE)) |
	    (R_MODE_IN_IRQ_OFF << 1) | R_INPUT_ENABLE;
	writel(v, pc + REG_GPIO(hdq_pin));
}

static inline int f_lvl(void)
{
	return !!(readl(pc + REG_GPIO(hdq_pin)) & R_DATA);
}

/* ---------- 慢速路径：GPIO 框架 ---------- */
static inline void s_low(void) { gpio_direction_output(hdq_gpio, 0); }
static inline void s_rel(void) { gpio_direction_input(hdq_gpio); }
static inline int s_lvl(void) { return gpio_get_value(hdq_gpio); }

static inline void hdq_low(void) { if (use_fast) f_low(); else s_low(); }
static inline void hdq_rel(void) { if (use_fast) f_rel(); else s_rel(); }
static inline int hdq_lvl(void) { return use_fast ? f_lvl() : s_lvl(); }

/* ---------- HDQ 原语 ---------- */
static void hdq_break(void)
{
	hdq_low();
	udelay(t_break);
	hdq_rel();
	udelay(t_brk_rec);
}

static void hdq_write_bit(int b)
{
	int low = b ? t_w1_low : t_w0_low;

	hdq_low();
	udelay(low);
	hdq_rel();
	if (t_bit > low)
		udelay(t_bit - low);
}

static int hdq_read_bit(void)
{
	int v;

	hdq_low();
	udelay(t_rd_low);
	hdq_rel();
	if (t_rd_samp > t_rd_low)
		udelay(t_rd_samp - t_rd_low);
	v = hdq_lvl();
	if (t_bit > t_rd_samp)
		udelay(t_bit - t_rd_samp);
	return v;
}

static void hdq_write_byte(u8 b)
{
	int i;

	for (i = 0; i < 8; i++)
		hdq_write_bit((b >> i) & 1);
}

static u8 hdq_read_byte(void)
{
	u8 v = 0;
	int i;

	for (i = 0; i < 8; i++)
		if (hdq_read_bit())
			v |= (1 << i);
	return v;
}

static int hdq_read_byte_cmd(u8 cmd)
{
	hdq_break();
	hdq_write_byte(cmd & 0x7f);
	udelay(t_bit);
	return hdq_read_byte();
}

/* 多数表决：同一字节读 3 次取多数 */
static int hdq_read_byte_v(u8 cmd)
{
	int a = hdq_read_byte_cmd(cmd);
	int b = hdq_read_byte_cmd(cmd);
	int c = hdq_read_byte_cmd(cmd);

	if (a == b || a == c)
		return a;
	if (b == c)
		return b;
	return a;	/* 三次全不同：返回第一次 */
}

static int hdq_read16_cmd(u8 cmd, int *out, int vote)
{
	int lo, hi;

	if (vote) {
		lo = hdq_read_byte_v(cmd);
		hi = hdq_read_byte_v(cmd + 1);
	} else {
		lo = hdq_read_byte_cmd(cmd);
		hi = hdq_read_byte_cmd(cmd + 1);
	}
	*out = (lo & 0xff) | ((hi & 0xff) << 8);
	return 0;
}

/* 连读 n 次取多数（16 位整体表决） */
static int hdq_read16_vote(u8 cmd, int n, int *out)
{
	int vals[9], cnt[9], i, j, best = 0, bestn = 0;

	if (n < 1) n = 1;
	if (n > 9) n = 9;
	for (i = 0; i < n; i++) {
		int v;

		hdq_read16_cmd(cmd, &v, 0);
		vals[i] = v;
	}
	for (i = 0; i < n; i++) {
		cnt[i] = 0;
		for (j = 0; j < n; j++)
			if (vals[j] == vals[i])
				cnt[i]++;
		if (cnt[i] > bestn) {
			bestn = cnt[i];
			best = vals[i];
		}
	}
	*out = best;
	return bestn;
}

/* ---------- sysfs ---------- */
static ssize_t cmd_store(struct kobject *k, struct kobj_attribute *a,
			 const char *buf, size_t count)
{
	char *line, *tok, *rest;
	unsigned long x, y;

	o_reset();
	line = kstrdup(buf, GFP_KERNEL);
	if (!line)
		return -ENOMEM;
	strim(line);
	tok = line;
	rest = strchr(line, ' ');
	if (rest) {
		*rest++ = '\0';
		while (*rest == ' ')
			rest++;
	}

	if (!strcmp(tok, "take")) {
		int n = hdq_gpio, ret;

		if (rest && !kstrtoul(rest, 0, &x))
			n = (int)x;
		/* 无条件释放后再申请（旧模块可能占着） */
		gpio_free(n);
		hdq_ok = false;
		gpio_free(hdq_gpio);
		ret = gpio_request(n, "hdq3");
		if (ret) {
			o("take %d 失败: %d\n", n, ret);
			goto out;
		}
		hdq_gpio = n;
		hdq_pin = n - 512;
		hdq_ok = true;
		gpio_direction_input(hdq_gpio);
		o("take %d ok (pin %d), idle=%d\n", n, hdq_pin, hdq_lvl());
	} else if (!strcmp(tok, "fast")) {
		if (kstrtoul(rest, 0, &x)) {
			o("usage: fast <0|1>\n");
			goto out;
		}
		use_fast = !!x;
		o("use_fast=%d\n", use_fast);
	} else if (!hdq_ok) {
		o("gpio 未申请，先 take <n>\n");
	} else if (!strcmp(tok, "timing")) {
		int a1, b1, c1, d1, e1, f1, g1;

		if (sscanf(rest, "%d %d %d %d %d %d %d", &a1, &b1, &c1, &d1,
			   &e1, &f1, &g1) != 7) {
			o("timing: break=%d brk_rec=%d w0=%d w1=%d bit=%d rd_low=%d rd_samp=%d\n",
			  t_break, t_brk_rec, t_w0_low, t_w1_low, t_bit,
			  t_rd_low, t_rd_samp);
			goto out;
		}
		t_break = a1; t_brk_rec = b1; t_w0_low = c1; t_w1_low = d1;
		t_bit = e1; t_rd_low = f1; t_rd_samp = g1;
		o("timing: break=%d brk_rec=%d w0=%d w1=%d bit=%d rd_low=%d rd_samp=%d\n",
		  t_break, t_brk_rec, t_w0_low, t_w1_low, t_bit, t_rd_low,
		  t_rd_samp);
	} else if (!strcmp(tok, "read")) {
		if (kstrtoul(rest, 0, &x)) {
			o("usage: read <cmd>\n");
			goto out;
		}
		o("read 0x%02lx -> 0x%02x (vote 0x%02x)\n", x,
		  hdq_read_byte_cmd((u8)x), hdq_read_byte_v((u8)x));
	} else if (!strcmp(tok, "read16")) {
		int v, vv;

		if (kstrtoul(rest, 0, &x)) {
			o("usage: read16 <cmd>\n");
			goto out;
		}
		hdq_read16_cmd((u8)x, &v, 0);
		hdq_read16_cmd((u8)x, &vv, 1);
		o("read16 0x%02lx -> 0x%04x (%d) | 表决后 0x%04x (%d)\n",
		  x, v, (s16)v, vv, (s16)vv);
	} else if (!strcmp(tok, "vote")) {
		int v, n = 5;

		if (kstrtoul(rest, 0, &x)) {
			o("usage: vote <cmd> [n]\n");
			goto out;
		}
		if (strchr(rest, ' '))
			sscanf(strchr(rest, ' ') + 1, "%d", &n);
		hdq_read16_vote((u8)x, n, &v);
		o("vote 0x%02lx (%d次) -> 0x%04x (%d)\n", x, n, v, (s16)v);
	} else if (!strcmp(tok, "battery")) {
		static const struct { u8 reg; const char *nm; } m[] = {
			{ 0x02, "AtRate" },	{ 0x06, "温度(0.1K)" },
			{ 0x08, "电压(mV)" },	{ 0x0a, "FLAGS" },
			{ 0x0c, "RemCap(mAh)" },{ 0x0e, "?" },
			{ 0x10, "?" },		{ 0x12, "满充容量(mAh)" },
			{ 0x14, "平均电流(mA)" },{ 0x16, "TTE(min)" },
			{ 0x1c, "?" },		{ 0x20, "?" },
			{ 0x22, "?" },		{ 0x24, "?" },
			{ 0x28, "内温(0.1K)" },	{ 0x2a, "循环次数" },
			{ 0x2c, "SOC(%)" },	{ 0x2e, "?" },
			{ 0x30, "?" },		{ 0x32, "?" },
			{ 0x34, "?" },		{ 0x3c, "设计容量(mAh)" },
		};
		int i;

		for (i = 0; i < ARRAY_SIZE(m); i++) {
			int v, n = hdq_read16_vote(m[i].reg, 5, &v);

			o("  0x%02x %-16s = 0x%04x (%6d)  [%d/5]\n",
			  m[i].reg, m[i].nm, v, (s16)v, n);
		}
	} else if (!strcmp(tok, "dump")) {
		unsigned long e;
		int c;

		if (sscanf(rest, "%lx %lx", &x, &e) != 2) {
			o("usage: dump <a> <b>\n");
			goto out;
		}
		for (c = (int)x; c <= (int)e && c < 0x80; c += 2) {
			int v, n = hdq_read16_vote((u8)c, 5, &v);

			o("  0x%02x = 0x%04x (%6d) [%d/5]\n", c, v, (s16)v, n);
		}
	} else if (!strcmp(tok, "reg")) {
		if (kstrtoul(rest, 0, &x)) {
			o("usage: reg <off> [val]\n");
			goto out;
		}
		if (sscanf(rest, "%lx %lx", &x, &y) == 2) {
			writel((u32)y, pc + x);
			o("reg[0x%lx] <= 0x%lx, 读回 0x%08x\n", x, y,
			  readl(pc + x));
		} else {
			o("reg[0x%lx] = 0x%08x\n", x, readl(pc + x));
		}
	} else {
		o("cmds: take [n] | fast <0|1> | timing ... | read <c> | read16 <c> |\n"
		  "      vote <c> [n] | battery | dump <a> <b> | reg <off> [val]\n");
	}
out:
	kfree(line);
	return count;
}
static struct kobj_attribute cmd_attr = __ATTR_WO(cmd);

static ssize_t out_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	memcpy(buf, out_buf, out_len);
	buf[out_len] = '\0';
	return out_len;
}
static struct kobj_attribute out_attr = __ATTR_RO(out);

static struct attribute *attrs[] = { &cmd_attr.attr, &out_attr.attr, NULL };
static const struct attribute_group grp = { .attrs = attrs };

static int __init hdq3_init(void)
{
	int ret;

	kobj = kobject_create_and_add("hdq3", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &grp);
	if (ret) {
		kobject_put(kobj);
		return ret;
	}

	pc = ioremap(PINCTRL_BASE, PINCTRL_MAP_SIZE);
	if (!pc)
		pr_err("hdq3: ioremap 失败\n");

	gpio_free(hdq_gpio);
	ret = gpio_request(hdq_gpio, "hdq3");
	if (ret)
		pr_err("hdq3: gpio %d 申请失败 %d\n", hdq_gpio, ret);
	else {
		hdq_ok = true;
		gpio_direction_input(hdq_gpio);
		pr_info("hdq3: gpio %d (pin %d) acquired, idle=%d\n",
			hdq_gpio, hdq_pin, hdq_lvl());
	}
	pr_info("hdq3: /sys/kernel/hdq3/{cmd,out}\n");
	return 0;
}

static void __exit hdq3_exit(void)
{
	sysfs_remove_group(kobj, &grp);
	kobject_put(kobj);
	if (pc)
		iounmap(pc);
	if (hdq_ok)
		gpio_free(hdq_gpio);
}

module_init(hdq3_init);
module_exit(hdq3_exit);

MODULE_AUTHOR("iPad A1599 Linux Project");
MODULE_DESCRIPTION("BQ27540 HDQ master, direct-register fast path");
MODULE_LICENSE("GPL");
