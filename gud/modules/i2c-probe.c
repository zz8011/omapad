// SPDX-License-Identifier: GPL-2.0
/*
 * i2c-probe: I2C 总线扫描与原始传输工具（iPad mini D2089 探索用）
 *
 * 相比 pmu-direct 的改进：
 *   - 输出写入 sysfs 文件，不依赖 dmesg（不会滚动丢失）
 *   - 支持任意 I2C 总线 / 地址
 *   - 支持原始字节序列收发（用于试探 PMU 命令协议）
 *
 * 接口：/sys/kernel/i2cprobe/cmd  (写命令)
 *       /sys/kernel/i2cprobe/out  (读上次输出)
 *
 * 命令：
 *   scan <bus>                       扫描总线地址 0x08-0x77
 *   raw  <bus> <addr> <b0> <b1> ...  原始写
 *   rawr <bus> <addr> <b0> ... : <rlen>   写后读
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/mutex.h>

#define OUT_SZ	PAGE_SIZE	/* sysfs show 缓冲只有一页 */

/* d2089-watchdog 每 30 秒写一次 cmd，宿主也会同时读写：cmd/out 必须串行 */
static DEFINE_MUTEX(ip_lock);

static char out_buf[OUT_SZ];
static int out_len;
static struct kobject *ip_kobj;

static void o_reset(void)
{
	out_len = 0;
	out_buf[0] = '\0';
}

static __printf(1, 2) void o(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (out_len >= OUT_SZ - 200)
		return;
	va_start(ap, fmt);
	n = vscnprintf(out_buf + out_len, OUT_SZ - out_len - 1, fmt, ap);
	va_end(ap);
	if (n > 0)
		out_len += n;
}

/* 解析空格分隔的十六进制字节（手写，不依赖 simple_strtoull） */
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
			v = v * 16 + (isdigit(*s) ? (*s - '0')
						  : (tolower(*s) - 'a' + 10));
			s++;
			digits++;
		}
		if (!digits)
			break;
		buf[n++] = (u8)v;
	}
	return n;
}

static void do_scan(int bus)
{
	struct i2c_adapter *adap;
	int addr, found = 0;

	adap = i2c_get_adapter(bus);
	if (!adap) {
		o("scan bus %d: NO ADAPTER\n", bus);
		return;
	}
	o("scan bus %d (%s):\n", bus, adap->name);

	for (addr = 0x08; addr <= 0x77; addr++) {
		int ret;

		/* 优先 SMBus quick（无数据，最安全） */
		ret = i2c_smbus_xfer(adap, addr, 0, I2C_SMBUS_QUICK,
				     I2C_SMBUS_WRITE, 0, NULL);
		if (ret == 0) {
			o("  0x%02x ACK (quick)\n", addr);
			found++;
			continue;
		}
		if (ret == -EOPNOTSUPP || ret == -EINVAL) {
			/* 适配器不支持 quick：退化为 1 字节读探测 */
			struct i2c_msg msg;
			u8 b = 0;

			msg.addr = addr;
			msg.flags = I2C_M_RD;
			msg.len = 1;
			msg.buf = &b;
			ret = i2c_transfer(adap, &msg, 1);
			if (ret == 1) {
				o("  0x%02x ACK (rd) = 0x%02x\n", addr, b);
				found++;
			}
		}
	}
	o("scan bus %d done: %d device(s)\n", bus, found);
	i2c_put_adapter(adap);
}

static void do_raw(int bus, u8 addr, const u8 *w, int wlen,
		   int do_read, int rlen)
{
	struct i2c_adapter *adap;
	struct i2c_msg msgs[2];
	u8 rbuf[64];
	int ret, i;

	if (wlen > 64 || rlen > 64) {
		o("raw: length too big\n");
		return;
	}

	adap = i2c_get_adapter(bus);
	if (!adap) {
		o("raw bus %d: NO ADAPTER\n", bus);
		return;
	}

	o("raw bus %d addr 0x%02x write[%d]:", bus, addr, wlen);
	for (i = 0; i < wlen; i++)
		o(" %02x", w[i]);
	o("\n");

	msgs[0].addr = addr;
	msgs[0].flags = 0;
	msgs[0].len = wlen;
	msgs[0].buf = (u8 *)w;

	if (do_read) {
		msgs[1].addr = addr;
		msgs[1].flags = I2C_M_RD | I2C_M_IGNORE_NAK;
		msgs[1].len = rlen;
		msgs[1].buf = rbuf;

		ret = i2c_transfer(adap, msgs, 2);
		if (ret == 2) {
			o("  read[%d]:", rlen);
			for (i = 0; i < rlen; i++)
				o(" %02x", rbuf[i]);
			o("\n");
		} else {
			o("  transfer failed: %d\n", ret);
		}
	} else {
		ret = i2c_transfer(adap, msgs, 1);
		o("  write %s (%d)\n", ret == 1 ? "OK" : "FAILED", ret);
	}

	i2c_put_adapter(adap);
}

/* ---- sysfs ---- */

static ssize_t cmd_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	char *line, *tok, *rest;
	unsigned long bus, addr;

	line = kstrdup(buf, GFP_KERNEL);
	if (!line)
		return -ENOMEM;
	mutex_lock(&ip_lock);
	o_reset();

	/* 去掉换行 */
	strim(line);
	tok = line;
	rest = strchr(line, ' ');
	if (rest) {
		*rest++ = '\0';
		while (*rest == ' ')
			rest++;
	}

	if (!strcmp(tok, "scan")) {
		if (kstrtoul(rest, 0, &bus)) {
			o("usage: scan <bus>\n");
			goto out;
		}
		do_scan((int)bus);
	} else if (!strcmp(tok, "raw") || !strcmp(tok, "rawr")) {
		u8 wbuf[64], rbuf[64];
		int wlen, rlen = 0;
		char *sp, *colon;

		sp = strchr(rest, ' ');
		if (!sp) {
			o("usage: %s <bus> <addr> <bytes...> [: rlen]\n", tok);
			goto out;
		}
		*sp++ = '\0';
		if (kstrtoul(rest, 0, &bus)) {
			o("bad bus\n");
			goto out;
		}
		while (*sp == ' ')
			sp++;
		colon = strchr(sp, ' ');
		if (!colon) {
			o("bad addr\n");
			goto out;
		}
		*colon++ = '\0';
		if (kstrtoul(sp, 0, &addr)) {
			o("bad addr\n");
			goto out;
		}
		while (*colon == ' ')
			colon++;

		/* 若含 ':' 则后面是读长度 */
		{
			char *c2 = strchr(colon, ':');

			if (c2) {
				*c2++ = '\0';
				rlen = (int)simple_strtoul(c2, NULL, 0);
			}
		}
		wlen = parse_bytes(colon, wbuf, 64);
		if (wlen < 1) {
			o("no bytes\n");
			goto out;
		}
		if (!strcmp(tok, "rawr") && rlen < 1)
			rlen = 1;
		do_raw((int)bus, (u8)addr, wbuf, wlen,
		       !strcmp(tok, "rawr"), rlen);
		(void)rbuf;
	} else {
		o("unknown cmd '%s'\n", tok);
		o("cmds: scan <bus> | raw <bus> <addr> <b..> | "
		  "rawr <bus> <addr> <b..> : <rlen>\n");
	}

out:
	mutex_unlock(&ip_lock);
	kfree(line);
	return count;
}
static struct kobj_attribute cmd_attr = __ATTR_WO(cmd);

static ssize_t out_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	int n;

	mutex_lock(&ip_lock);
	n = min_t(int, out_len, PAGE_SIZE - 1);
	memcpy(buf, out_buf, n);
	buf[n] = '\0';
	mutex_unlock(&ip_lock);
	return n;
}
static struct kobj_attribute out_attr = __ATTR_RO(out);

static struct attribute *ip_attrs[] = {
	&cmd_attr.attr,
	&out_attr.attr,
	NULL,
};

static const struct attribute_group ip_group = {
	.attrs = ip_attrs,
};

static int __init i2cprobe_init(void)
{
	int ret;

	ip_kobj = kobject_create_and_add("i2cprobe", kernel_kobj);
	if (!ip_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(ip_kobj, &ip_group);
	if (ret) {
		kobject_put(ip_kobj);
		return ret;
	}

	pr_info("i2c-probe: /sys/kernel/i2cprobe/{cmd,out}\n");
	return 0;
}

static void __exit i2cprobe_exit(void)
{
	sysfs_remove_group(ip_kobj, &ip_group);
	kobject_put(ip_kobj);
}

module_init(i2cprobe_init);
module_exit(i2cprobe_exit);

MODULE_AUTHOR("iPad A1599 Linux Project");
MODULE_DESCRIPTION("I2C bus probe and raw transfer");
MODULE_LICENSE("GPL");
