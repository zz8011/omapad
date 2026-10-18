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
 * 读时序（2026-10-16 改）：读回数据时每一位都由 BQ27540 自己拉低，主机不发起读时隙。
 *   旧实现每位先拉低 2µs、再在固定 50µs 处采样，不和从机下降沿同步；而从机"1"的
 *   低电平可长达 50µs（tDW1 32–50µs，tDW0 80–145µs），采样点正好落在边界上 →
 *   实测 SOC 稳定读成 0x17（真值 0x37，bit5 1→0），5 次表决也救不回来。
 *   现在默认（rd_mode=1）等从机下降沿、测低电平宽度，宽度 < t_rd_thr 判 1。
 *   整个字节事务关中断，避免 PREEMPT 内核在位窗口中途调度。
 *
 * 接口：/sys/kernel/<模块名>/{cmd,out}（模块名默认 hdq3；测试版可编成 hdq4 并存）
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
#include <linux/mutex.h>
#include <linux/timekeeping.h>
#include <linux/irqflags.h>

#define HDQ_GPIO_DEFAULT	556	/* 44 + 512 (GPIO_DYNAMIC_BASE) */
#define HDQ_PIN			44
#define PINCTRL_BASE		0x20e300000UL
#define PINCTRL_MAP_SIZE	0x1000
#define OUT_SZ			PAGE_SIZE	/* sysfs show 缓冲只有一页 */

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
static int rd_mode = 1;		/* 0=旧的固定点采样  1=沿同步测宽 */
static int irqoff = 1;		/* 字节事务期间关中断 */
static int t_rd_thr = 65;	/* 低电平宽度阈值（µs）：< 判 1，>= 判 0 */
static int t_rd_to = 500;	/* 等从机第一个下降沿的超时（µs） */

/* 沿同步模式的统计，stat 命令查看 */
static unsigned int st_w1_min = ~0u, st_w1_max, st_w0_min = ~0u, st_w0_max;
static unsigned int st_timeout, st_bytes;

static DEFINE_MUTEX(hdq_lock);	/* cmd/out 共享 out_buf 和总线，必须串行 */

static bool hdq_ok;
static void __iomem *pc;
static char out_buf[OUT_SZ];
static int out_len;
static struct kobject *kobj;

static void o_reset(void) { out_len = 0; out_buf[0] = '\0'; }

static __printf(1, 2) void o(const char *fmt, ...)
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

/* 等引脚变成 lvl，返回到达时刻（ns）；超时返回 0 */
static u64 wait_lvl(int lvl, int to_us)
{
	u64 t0 = ktime_get_ns(), t;

	do {
		t = ktime_get_ns();
		if (f_lvl() == lvl)
			return t;
	} while (t - t0 < (u64)to_us * 1000);
	return 0;
}

/* 沿同步读一个字节：每位等从机拉低，按低电平宽度判 0/1。失败返回负值 */
static int hdq_read_byte_edge(void)
{
	int i, v = 0;

	for (i = 0; i < 8; i++) {
		u64 fall = wait_lvl(0, i ? 300 : t_rd_to), rise;
		unsigned int w;

		if (!fall || !(rise = wait_lvl(1, 300))) {
			st_timeout++;
			return -ETIMEDOUT;
		}
		w = (rise - fall) / 1000;
		if (w < t_rd_thr) {
			v |= 1 << i;
			st_w1_min = min(st_w1_min, w);
			st_w1_max = max(st_w1_max, w);
		} else {
			st_w0_min = min(st_w0_min, w);
			st_w0_max = max(st_w0_max, w);
		}
	}
	st_bytes++;
	return v;
}

static int hdq_read_byte_cmd(u8 cmd)
{
	unsigned long flags = 0;
	int v;

	if (irqoff)
		local_irq_save(flags);
	hdq_break();
	hdq_write_byte(cmd & 0x7f);
	if (rd_mode && use_fast) {
		v = hdq_read_byte_edge();
	} else {
		udelay(t_bit);
		v = hdq_read_byte();
	}
	if (irqoff)
		local_irq_restore(flags);
	return v;
}

/* 多数表决：同一字节读 3 次取多数 */
static int hdq_read_byte_v(u8 cmd)
{
	int a = hdq_read_byte_cmd(cmd);
	int b = hdq_read_byte_cmd(cmd);
	int c = hdq_read_byte_cmd(cmd);

	if (a >= 0 && (a == b || a == c))
		return a;
	if (b >= 0 && b == c)
		return b;
	return a >= 0 ? a : b >= 0 ? b : c;	/* 无多数：返回第一个有效值 */
}

/* 失败返回负值，*out 不变 */
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
	if (lo < 0 || hi < 0)
		return lo < 0 ? lo : hi;
	*out = lo | (hi << 8);
	return 0;
}

/* 连读 n 次取多数（16 位整体表决） */
static int hdq_read16_vote(u8 cmd, int n, int *out)
{
	int vals[9], cnt[9], i, j, best = -1, bestn = 0;

	if (n < 1) n = 1;
	if (n > 9) n = 9;
	for (i = 0; i < n; i++) {
		int v = -1;

		hdq_read16_cmd(cmd, &v, 0);
		vals[i] = v;	/* 失败记 -1，不参与表决 */
	}
	for (i = 0; i < n; i++) {
		cnt[i] = 0;
		if (vals[i] < 0)
			continue;
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

	line = kstrdup(buf, GFP_KERNEL);
	if (!line)
		return -ENOMEM;
	mutex_lock(&hdq_lock);
	o_reset();
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
		ret = gpio_request(n, KBUILD_MODNAME);
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
	} else if (!strcmp(tok, "mode")) {
		/* mode [rd_mode irqoff thr]：不带参数只显示 */
		int a1, b1, c1;

		if (rest && sscanf(rest, "%d %d %d", &a1, &b1, &c1) == 3) {
			rd_mode = a1; irqoff = b1; t_rd_thr = c1;
		}
		o("mode: rd_mode=%d（0=旧固定点采样 1=沿同步测宽） irqoff=%d thr=%dus fast=%d\n",
		  rd_mode, irqoff, t_rd_thr, use_fast);
	} else if (!strcmp(tok, "stat")) {
		o("stat: bytes=%u timeout=%u  宽度 1:[%u,%u]us 0:[%u,%u]us  阈值 %dus\n",
		  st_bytes, st_timeout, st_w1_min, st_w1_max, st_w0_min, st_w0_max,
		  t_rd_thr);
		if (rest && !strcmp(rest, "reset")) {
			st_w1_min = st_w0_min = ~0u;
			st_w1_max = st_w0_max = st_timeout = st_bytes = 0;
		}
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
		o("read 0x%02lx -> %d (vote %d)\n", x,
		  hdq_read_byte_cmd((u8)x), hdq_read_byte_v((u8)x));
	} else if (!strcmp(tok, "read16")) {
		int v = -1, vv = -1;

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
		o("cmds: take [n] | fast <0|1> | mode [rd irqoff thr] | stat [reset] |\n"
		  "      timing ... | read <c> | read16 <c> | vote <c> [n] | battery |\n"
		  "      dump <a> <b> | reg <off> [val]\n");
	}
out:
	mutex_unlock(&hdq_lock);
	kfree(line);
	return count;
}
static struct kobj_attribute cmd_attr = __ATTR_WO(cmd);

static ssize_t out_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	int n;

	mutex_lock(&hdq_lock);
	n = min_t(int, out_len, PAGE_SIZE - 1);
	memcpy(buf, out_buf, n);
	buf[n] = '\0';
	mutex_unlock(&hdq_lock);
	return n;
}
static struct kobj_attribute out_attr = __ATTR_RO(out);

static struct attribute *attrs[] = { &cmd_attr.attr, &out_attr.attr, NULL };
static const struct attribute_group grp = { .attrs = attrs };

static int __init hdq3_init(void)
{
	int ret;

	kobj = kobject_create_and_add(KBUILD_MODNAME, kernel_kobj);
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

	ret = gpio_request(hdq_gpio, KBUILD_MODNAME);
	if (!ret) {
		hdq_ok = true;
		gpio_direction_input(hdq_gpio);
		pr_info(KBUILD_MODNAME ": gpio %d (pin %d) acquired, idle=%d\n",
			hdq_gpio, hdq_pin, hdq_lvl());
	} else if (pc) {
		/* 已被别的 hdq 模块占用（内核不能卸载模块，测试版只能并存）：
		 * 快速路径直接写 pinctrl 寄存器，不需要 GPIO 所有权 */
		hdq_ok = true;
		use_fast = 1;
		f_rel();
		pr_info(KBUILD_MODNAME ": gpio %d 已被占用 (%d)，只用寄存器快速路径\n",
			hdq_gpio, ret);
	} else {
		pr_err(KBUILD_MODNAME ": gpio %d 申请失败 %d\n", hdq_gpio, ret);
	}
	pr_info(KBUILD_MODNAME ": /sys/kernel/" KBUILD_MODNAME "/{cmd,out}\n");
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
