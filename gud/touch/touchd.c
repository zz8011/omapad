/* touchd：J207 触摸 → USB HID 多点触控（/dev/hidg0）。
 * 读取链路与 run/aht-resume-20261016/read-z2.sh 相同：EB 长度请求 → E1 应答 → A5 数据读取，
 * 经 spi3raw4 的 sysfs cmd/out 文本接口；GPIO104 低电平时才读（-p 改为定时轮询）。
 * 0x76 报告格式见 re/aht/FINDINGS-aht.md「人工触摸窗口」「坐标标定」两节。
 * 用法: touchd -D            输出 HID 报告描述符（setup-gadget 写入 report_desc）
 *       touchd [-p] [-s] [-x] [-y] [-v]
 *         -p 不看 IRQ，每 4 ms 读一次   -s 交换 X/Y   -x/-y 翻转 X/Y   -v 打印每个报告
 *         -c a,b,c,d,e,f 换校准参数：u = a·x + b·y + c，v = d·x + e·y + f */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CMD	"/sys/kernel/spi3raw4/cmd"
#define OUT	"/sys/kernel/spi3raw4/out"
#define HIDG	"/dev/hidg0"
#define GPIO_PG	0x20f100000ULL
#define IRQ_OFF	0x1a0		/* GPIO104，低 = 有帧待读 */
#define MAXN	1024		/* E1 声明长度上限 */
#define SLOTS	10
#define XMAX	21350		/* 0.01 mm，横屏宽（标定：屏幕跨原始 y 约 213190 µm） */
#define YMAX	16012		/* 0.01 mm，横屏高（标定：屏幕跨原始 x 约 160950 µm） */
#define REL_MS	250		/* 有触点却这么久没报告 → 全部抬起 */
#define F3_MS	10000		/* 空闲时每隔这么久重设 F3=1 */

/* 报告 ID 1：10 × {tip:1 pad:7, id:8, X:16, Y:16} + 触点数:8 = 62 字节（含 ID）。
 * 不含 feature report：hid-multitouch 缺 Contact Count Maximum 时默认 10，不发 GET_REPORT。 */
static const uint8_t finger[] = {
	0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02,		/* Digitizer / Finger / Collection(Logical) */
	0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, /* Tip Switch */
	0x75, 0x07, 0x81, 0x03,				/* 填充 */
	0x09, 0x51, 0x26, 0xff, 0x00, 0x75, 0x08, 0x81, 0x02, /* Contact Identifier */
	0x05, 0x01, 0x55, 0x0d, 0x65, 0x11, 0x75, 0x10, 0x35, 0x00,	/* 单位 cm×10^-3 */
	0x09, 0x30, 0x26, XMAX & 0xff, XMAX >> 8, 0x46, XMAX & 0xff, XMAX >> 8, 0x81, 0x02, /* X */
	0x09, 0x31, 0x26, YMAX & 0xff, YMAX >> 8, 0x46, YMAX & 0xff, YMAX >> 8, 0x81, 0x02, /* Y */
	0x55, 0x00, 0x65, 0x00, 0xc0,
};
static const uint8_t head[] = { 0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x01 };	/* Touch Screen */
static const uint8_t tail[] = {
	0x05, 0x0d, 0x09, 0x54, 0x15, 0x00, 0x25, SLOTS, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, /* Contact Count */
	0xc0,
};

static int cmdfd, outfd, hidfd = -1, verbose;
static unsigned hid_ok, hid_drop;
static volatile uint32_t *gpio;
static char obuf[16384];

static uint64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000ULL + t.tv_nsec / 1000000;
}

/* 一次 xfer：发送 n 字节，返回收到的字节数（解析 out 中 rx[N]: 之后的十六进制）。 */
static int xfer(const uint8_t *tx, int n, uint8_t *rx)
{
	static char line[3 * MAXN + 64];
	int k = sprintf(line, "xfer");
	for (int i = 0; i < n; i++)
		k += sprintf(line + k, " %02x", tx[i]);
	line[k++] = '\n';
	if (pwrite(cmdfd, line, k, 0) != k)
		return -1;
	ssize_t r = pread(outfd, obuf, sizeof obuf - 1, 0);
	if (r <= 0)
		return -1;
	obuf[r] = 0;
	char *p = strstr(obuf, "rx[");
	if (!p || !(p = strchr(p, '\n')))
		return -1;
	int m = 0;
	while (m < n) {
		while (*p == ' ' || *p == '\n')
			p++;
		char *e;
		unsigned long v = strtoul(p, &e, 16);
		if (e == p)
			break;
		rx[m++] = v;
		p = e;
	}
	return m;
}

static void cs_release(void)
{
	pwrite(cmdfd, "cs 1\n", 5, 0);
}

static const uint8_t setf3[16] = { 0xe4, 0xf3, 0x01, 0x01, [14] = 0xd9, 0x01 };
static const uint8_t stat_q[16] = { 0xe1, [14] = 0xe1 };

static void set_f3(void)
{
	uint8_t rx[16];
	xfer(setf3, 16, rx);
	usleep(125);
	xfer(stat_q, 16, rx);
	fprintf(stderr, "touchd: SET F3=1 确认 %02x %02x %02x\n", rx[6], rx[7], rx[8]);
	usleep(2000);
}

/* 返回负载长度（0 = 无帧），-1 = 长度应答无效；有效时把负载拷到 pl。 */
static int seq = 1;
static int read_frame(uint8_t *pl)
{
	uint8_t tx[MAXN + 8], rx[MAXN + 8];
	uint8_t req[16] = { 0xeb, seq, [14] = seq == 1 ? 0xec : 0xed };
	if (xfer(req, 16, rx) != 16 || rx[0] != 0xe1)
		return -1;
	unsigned sum = 0;
	for (int i = 0; i < 14; i++)
		sum += rx[i];
	if ((sum & 0xffff) != (rx[14] | rx[15] << 8))
		return -1;
	int n = rx[1] | rx[2] << 8;
	if (n > MAXN)
		return -1;
	seq = 3 - seq;		/* iOS 在成功的零长度应答后也交替序号 */
	if (!n)
		return 0;
	int total = (n + 5 + 3) / 4 * 4;
	memset(tx, 0xa5, total);
	usleep(125);
	if (xfer(tx, total, rx) != total)
		return -2;
	/* 五字节头：ea|eb seq LE16(n) 校验，字节和为 0 */
	if ((rx[0] != 0xea && rx[0] != 0xeb) || (uint8_t)(rx[0] + rx[1] + rx[2] + rx[3] + rx[4]) ||
	    (rx[2] | rx[3] << 8) != n)
		return -2;
	sum = 0;
	for (int i = 0; i < n - 2; i++)
		sum += rx[5 + i];
	if ((sum & 0xffff) != (rx[5 + n - 2] | rx[5 + n - 1] << 8))
		return -2;
	memcpy(pl, rx + 5, n);
	return n;
}

struct slot { int id, tip; uint16_t x, y; };
static struct slot act[SLOTS];	/* 上一报告中按下的触点 */
static int nact;
static int swap_xy, flip_x, flip_y;

/* 原始坐标 → 横屏（充电口朝右）分数，左上为原点。2026-10-17 九点定点标定（hid/cal/）最小二乘拟合，
 * 最大残差 8.8 px。实测 Hyprland 不按输出 transform 旋转触摸，直接对到横屏逻辑区域。 */
static float cal[6] = { -4.46717e-08f, -4.69102e-06f, 0.993619f, -6.24276e-06f, -1.30029e-08f, 0.984553f };

static void map(float fx, float fy, uint16_t *ox, uint16_t *oy)
{
	float u = cal[0] * fx + cal[1] * fy + cal[2], v = cal[3] * fx + cal[4] * fy + cal[5], t;
	if (swap_xy)
		t = u, u = v, v = t;
	if (flip_x)
		u = 1.0f - u;
	if (flip_y)
		v = 1.0f - v;
	u = u < 0 ? 0 : u > 1 ? 1 : u;
	v = v < 0 ? 0 : v > 1 ? 1 : v;
	*ox = u * XMAX + 0.5f;
	*oy = v * YMAX + 0.5f;
}

static void hid_send(struct slot *s, int n)
{
	uint8_t r[2 + 6 * SLOTS] = { 1 };
	for (int i = 0; i < n; i++) {
		uint8_t *p = r + 1 + 6 * i;
		p[0] = s[i].tip;
		p[1] = s[i].id;
		p[2] = s[i].x, p[3] = s[i].x >> 8;
		p[4] = s[i].y, p[5] = s[i].y >> 8;
	}
	r[1 + 6 * SLOTS] = n;
	if (hidfd < 0) {
		static uint64_t last;
		if (now_ms() - last < 1000)
			return;
		last = now_ms();
		if ((hidfd = open(HIDG, O_RDWR | O_NONBLOCK)) < 0)
			return;
		fprintf(stderr, "touchd: 打开 " HIDG "\n");
	}
	/* 宿主还没取走上一个报告时 EAGAIN，最多等 16 ms */
	for (int i = 0; i < 16; i++) {
		if (write(hidfd, r, sizeof r) == sizeof r) {
			hid_ok++;
			return;
		}
		if (errno != EAGAIN)
			break;
		usleep(1000);
	}
	hid_drop++;
	if (errno != EAGAIN) {
		fprintf(stderr, "touchd: 写 " HIDG " 失败 %s，稍后重开\n", strerror(errno));
		close(hidfd);
		hidfd = -1;
	}
}

/* 0x76 报告 → HID。返回 0 = 已处理，-1 = 格式不符。 */
static int report76(const uint8_t *r, int len)
{
	int nf = r[22];
	if (len < 32 || r[0] != 0x76 || (r[16] | r[17] << 8) != 48 * nf || len != 32 + 48 * nf)
		return -1;
	struct slot out[SLOTS], now[SLOTS];
	int n = 0, k = 0;
	for (int i = 0; i < nf && k < SLOTS; i++) {
		const uint8_t *f = r + 32 + 48 * i;
		float x, y;
		memcpy(&x, f + 4, 4);
		memcpy(&y, f + 8, 4);
		if (verbose)
			fprintf(stderr, "  id=%d stage=%d b2=%d x=%.0f y=%.0f\n", f[0], f[1], f[2], x, y);
		if (f[1] != 3 && f[1] != 4)	/* 3 MakeTouch / 4 Touching（按 MTPathStage 推断） */
			continue;
		now[k].id = f[0];
		now[k].tip = 1;
		map(x, y, &now[k].x, &now[k].y);
		k++;
	}
	memcpy(out, now, k * sizeof *now);
	n = k;
	/* 上次按下、这次不再按下的触点：补一个 tip=0 */
	for (int i = 0; i < nact && n < SLOTS; i++) {
		int j;
		for (j = 0; j < k && now[j].id != act[i].id; j++)
			;
		if (j == k) {
			out[n] = act[i];
			out[n++].tip = 0;
		}
	}
	if (n)
		hid_send(out, n);
	memcpy(act, now, k * sizeof *now);
	nact = k;
	return 0;
}

static void release_all(void)
{
	for (int i = 0; i < nact; i++)
		act[i].tip = 0;
	if (nact)
		hid_send(act, nact);
	nact = 0;
}

/* configfs 的 report_desc 每次 write 都整体替换，必须一次写完 */
static void descriptor(void)
{
	uint8_t d[sizeof head + SLOTS * sizeof finger + sizeof tail], *p = d;
	memcpy(p, head, sizeof head), p += sizeof head;
	for (int i = 0; i < SLOTS; i++)
		memcpy(p, finger, sizeof finger), p += sizeof finger;
	memcpy(p, tail, sizeof tail);
	write(1, d, sizeof d);
}

int main(int argc, char **argv)
{
	int poll_mode = 0, c;
	while ((c = getopt(argc, argv, "Dpsxyvc:")) != -1)
		switch (c) {
		case 'D': descriptor(); return 0;
		case 'p': poll_mode = 1; break;
		case 's': swap_xy = 1; break;
		case 'x': flip_x = 1; break;
		case 'y': flip_y = 1; break;
		case 'v': verbose = 1; break;
		case 'c':
			if (sscanf(optarg, "%f,%f,%f,%f,%f,%f", cal, cal + 1, cal + 2, cal + 3, cal + 4, cal + 5) != 6)
				return 2;
			break;
		default: return 2;
		}
	setvbuf(stderr, NULL, _IOLBF, 0);
	if ((cmdfd = open(CMD, O_WRONLY)) < 0 || (outfd = open(OUT, O_RDONLY)) < 0) {
		perror("touchd: spi3raw4");
		return 1;
	}
	int mem = open("/dev/mem", O_RDONLY | O_SYNC);
	void *m = mem < 0 ? MAP_FAILED : mmap(0, 0x4000, PROT_READ, MAP_SHARED, mem, GPIO_PG);
	if (m == MAP_FAILED) {
		perror("touchd: GPIO mmap，改用轮询");
		poll_mode = 1;
	} else
		gpio = (volatile uint32_t *)((char *)m + IRQ_OFF);
	cs_release();
	set_f3();
	fprintf(stderr, "touchd: 开始（%s）\n", poll_mode ? "轮询" : "IRQ");

	uint8_t pl[MAXN];
	uint64_t last_rep = 0, last_f3 = now_ms(), last_poll = 0, last_stat = now_ms();
	unsigned frames = 0, bad_len = 0, bad_data = 0, other = 0, lost = 0;
	uint8_t prevseq = 0;
	for (;;) {
		uint64_t t = now_ms();
		if (nact && t - last_rep > REL_MS)
			release_all();
		if (!nact && t - last_rep > F3_MS && t - last_f3 > F3_MS) {
			set_f3();
			last_f3 = t;
		}
		if (t - last_stat > (verbose ? 5000 : 10000)) {
			fprintf(stderr, "touchd: 报告 %u 其他 %u 无效长度应答 %u 数据错 %u HID 写入 %u 丢弃 %u 漏帧 %u\n", frames, other, bad_len, bad_data, hid_ok, hid_drop, lost);
			last_stat = t;
		}
		/* IRQ 模式下每 50 ms 也读一次，防止漏掉电平 */
		if (!poll_mode && (*gpio & 1) && t - last_poll < 50) {
			usleep(1000);
			continue;
		}
		if (poll_mode && t - last_poll < 4) {
			usleep(1000);
			continue;
		}
		last_poll = t;
		int n = read_frame(pl);
		if (n == -1)
			bad_len++;
		else if (n == -2)
			bad_data++;
		/* 负载：90 <接口> <LE16 报告长度> <报告> <LE16 和> */
		if (n < 6) {
			usleep(1000);	/* 与 read-z2.sh 轮间隔相同；IRQ 常低时也不空转 */
			continue;
		}
		if (pl[0] != 0x90 || (pl[2] | pl[3] << 8) != n - 6 || report76(pl + 4, n - 6)) {
			other++;
			continue;
		}
		/* 报告头 +1 为芯片递增序号（约 8 ms 一帧）：连续触摸中的跳号 = 没读到的帧 */
		uint8_t d = pl[5] - prevseq;
		if (frames && t - last_rep < 100 && d > 1 && d < 64)
			lost += d - 1;
		prevseq = pl[5];
		frames++;
		last_rep = t;
	}
}
