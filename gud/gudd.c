// gudd：iPad A1599 上的 GUD（Generic USB Display）gadget 守护进程。
// 基于 FunctionFS：ep0 处理 GUD 厂商控制请求，ep1 bulk OUT 接收像素（可 LZ4），
// 写入 /dev/fb0（XRGB8888）。宿主端使用主线 drivers/gpu/drm/gud，无需额外软件。
//
// 用法：gudd [-F 文件:宽x高] [-x] <functionfs 挂载点>
//   -F  测试用：用普通文件代替 /dev/fb0
//   -x  额外声明 XRGB8888（默认只声明 RGB565，宿主转换，带宽减半）
//   -b  用 /dev/fb0（fbdev）而非 DRM dumb buffer
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/input.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include "third_party/lz4/lz4.h"
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG(...) do { fprintf(stderr, "gudd: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---- GUD 协议（include/drm/gud.h） ---- */
#define GUD_DISPLAY_MAGIC 0x1d50614d
#define GUD_DISPLAY_FLAG_STATUS_ON_SET 1
#define GUD_COMPRESSION_LZ4 1
#define GUD_PIXEL_FORMAT_RGB565 0x40
#define GUD_PIXEL_FORMAT_XRGB8888 0x80
#define GUD_CONNECTOR_TYPE_PANEL 0
#define GUD_CONNECTOR_STATUS_CONNECTED 1
#define GUD_CONNECTOR_FLAGS_POLL_STATUS 1
#define GUD_DISPLAY_MODE_FLAG_PREFERRED (1u << 10)
#define GUD_PROPERTY_BACKLIGHT_BRIGHTNESS 12

enum {
	GUD_REQ_GET_STATUS = 0x00, GUD_REQ_GET_DESCRIPTOR = 0x01,
	GUD_REQ_GET_FORMATS = 0x40, GUD_REQ_GET_PROPERTIES = 0x41,
	GUD_REQ_GET_CONNECTORS = 0x50, GUD_REQ_GET_CONNECTOR_PROPERTIES = 0x51,
	GUD_REQ_GET_CONNECTOR_TV_MODE_VALUES = 0x52, GUD_REQ_SET_CONNECTOR_FORCE_DETECT = 0x53,
	GUD_REQ_GET_CONNECTOR_STATUS = 0x54, GUD_REQ_GET_CONNECTOR_MODES = 0x55,
	GUD_REQ_GET_CONNECTOR_EDID = 0x56, GUD_REQ_SET_BUFFER = 0x60,
	GUD_REQ_SET_STATE_CHECK = 0x61, GUD_REQ_SET_STATE_COMMIT = 0x62,
	GUD_REQ_SET_CONTROLLER_ENABLE = 0x63, GUD_REQ_SET_DISPLAY_ENABLE = 0x64,
};
enum { ST_OK = 0, ST_BUSY = 1, ST_NOTSUPP = 2, ST_PROTO = 3, ST_INVAL = 4, ST_ERROR = 5 };

struct gud_display_descriptor_req {
	uint32_t magic; uint8_t version; uint32_t flags; uint8_t compression;
	uint32_t max_buffer_size, min_width, max_width, min_height, max_height;
} __attribute__((packed));

struct gud_display_mode_req {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	uint32_t flags;
} __attribute__((packed));

struct gud_connector_descriptor_req { uint8_t connector_type; uint32_t flags; } __attribute__((packed));

struct gud_set_buffer_req {
	uint32_t x, y, width, height, length; uint8_t compression; uint32_t compressed_length;
} __attribute__((packed));

struct gud_state_req { struct gud_display_mode_req mode; uint8_t format, connector; } __attribute__((packed));
struct gud_property_req { uint16_t prop; uint64_t val; } __attribute__((packed));

/* ---- 显示模式：原生 1536x2048，及 768x1024（设备端 2 倍放大，省带宽） ---- */
struct mode { uint16_t w, h; int scale; };
static struct mode modes[2];
static int nmodes;

/* ---- framebuffer ---- */
static uint8_t *fb;
static uint32_t fb_w, fb_h, fb_stride;
static int fb_roff = 16, fb_goff = 8, fb_boff = 0;

/* DRM dumb buffer 后端（默认）：普通内存 mmap，写完用 DIRTYFB 提交矩形。
 * fbdev mmap（deferred I/O）在 A1599 上与持续写入组合会使系统卡死（实测）。 */
static int drm_fd = -1;
static uint32_t drm_fb_id;

/* 脏矩形累积，由 flush 线程按最高 /run/gudd-fps（默认 60）合并提交。
 * 实测：每块（170 行）都 DIRTYFB、整帧 12 次连续提交会让 A1599 卡死。 */
static pthread_mutex_t dmg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dmg_cond = PTHREAD_COND_INITIALIZER;
static uint32_t dmg_x1 = UINT32_MAX, dmg_y1 = UINT32_MAX, dmg_x2, dmg_y2;

static void dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	if (drm_fd < 0) return;
	pthread_mutex_lock(&dmg_lock);
	if (x < dmg_x1) dmg_x1 = x;
	if (y < dmg_y1) dmg_y1 = y;
	if (x + w > dmg_x2) dmg_x2 = x + w;
	if (y + h > dmg_y2) dmg_y2 = y + h;
	pthread_cond_signal(&dmg_cond);
	pthread_mutex_unlock(&dmg_lock);
}

static uint64_t now_ms(void);
static long read_knob(const char *path, long def);

static void *flush_thread(void *arg)
{
	uint64_t last = 0;
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&dmg_lock);
		while (dmg_x1 == UINT32_MAX)
			pthread_cond_wait(&dmg_cond, &dmg_lock);
		pthread_mutex_unlock(&dmg_lock);

		long fps = read_knob("/run/gudd-fps", 60);
		uint64_t gap = 1000 / (fps > 0 ? fps : 30), t = now_ms();
		if (t - last < gap) usleep((gap - (t - last)) * 1000);

		pthread_mutex_lock(&dmg_lock);
		struct drm_clip_rect c = { dmg_x1, dmg_y1, dmg_x2, dmg_y2 };
		dmg_x1 = dmg_y1 = UINT32_MAX; dmg_x2 = dmg_y2 = 0;
		pthread_mutex_unlock(&dmg_lock);

		struct drm_mode_fb_dirty_cmd d = { .fb_id = drm_fb_id, .num_clips = 1,
						   .clips_ptr = (uintptr_t)&c };
		t = now_ms();
		if (ioctl(drm_fd, DRM_IOCTL_MODE_DIRTYFB, &d) && errno != ENOSYS) LOG("DIRTYFB: %m");
		last = now_ms();
		if (!access("/run/gudd-verbose", F_OK))
			LOG("flush %u,%u-%u,%u %llums", c.x1, c.y1, c.x2, c.y2, (unsigned long long)(last - t));
	}
	return NULL;
}

/* ---- 状态 ---- */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static uint8_t last_status = ST_OK;
static uint8_t formats[2];
static int nformats;
static struct { int mode; uint8_t format; } pending = { -1, 0 }, cur = { -1, 0 };
static int display_on;
static struct gud_set_buffer_req job;
static int job_ready, need_clear;
static uint64_t job_at; /* SET_BUFFER 被 ep0 线程接受的时刻（us） */
static int ep0 = -1;
static const char *ffs_dir;

static uint64_t now_us(void)
{
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint64_t now_ms(void)
{
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int bpp_of(uint8_t f) { return f == GUD_PIXEL_FORMAT_RGB565 ? 2 : 4; }

/* 单次 bulk 上限（宿主按此值拆分更新，未压缩长度不超过它）。
 * 早期为避免 FunctionFS 大块 kmalloc 设为 512 KiB；现在 ep1 按 ≤64 KiB 分段读取，不再受此限制。
 * 实测 512 KiB 时全分辨率一帧要拆 13 块，每块多一次 SET_BUFFER/GET_STATUS 往返，提交约 19 ms；
 * 改为 4 MiB 后一帧约 2 块。 */
#define BULK_MAX (4 * 1024 * 1024)

static void fb_clear(void)
{
	for (uint32_t y = 0; y < fb_h; y++)
		memset(fb + (size_t)y * fb_stride, 0, (size_t)fb_w * 4);
	dirty(0, 0, fb_w, fb_h);
}

/* RGB565 → fb 一行。分支都在循环外，便于编译器生成 NEON 代码。 */
static void row565(uint32_t *restrict d, const uint16_t *restrict sp, uint32_t w)
{
	const int ro = fb_roff, go = fb_goff, bo = fb_boff;
	for (uint32_t x = 0; x < w; x++) {
		uint32_t c = sp[x];
		uint32_t r = (c >> 8 & 0xf8) | (c >> 13), g = (c >> 3 & 0xfc) | (c >> 9 & 3),
			 b = (c << 3 & 0xf8) | (c >> 2 & 7);
		d[x] = r << ro | g << go | b << bo;
	}
}

static void row565x2(uint32_t *restrict d, const uint16_t *restrict sp, uint32_t w)
{
	const int ro = fb_roff, go = fb_goff, bo = fb_boff;
	for (uint32_t x = 0; x < w; x++) {
		uint32_t c = sp[x];
		uint32_t r = (c >> 8 & 0xf8) | (c >> 13), g = (c >> 3 & 0xfc) | (c >> 9 & 3),
			 b = (c << 3 & 0xf8) | (c >> 2 & 7);
		d[2 * x] = d[2 * x + 1] = r << ro | g << go | b << bo;
	}
}

static void row8888(uint32_t *restrict d, const uint32_t *restrict sp, uint32_t w)
{
	if (fb_roff == 16 && fb_goff == 8 && fb_boff == 0) { memcpy(d, sp, (size_t)w * 4); return; }
	for (uint32_t x = 0; x < w; x++) {
		uint32_t c = sp[x];
		d[x] = (c >> 16 & 255) << fb_roff | (c >> 8 & 255) << fb_goff | (c & 255) << fb_boff;
	}
}

/* 把矩形像素写入 fb，按当前模式缩放（源数据小端，平台也是小端） */
static void blit(const struct gud_set_buffer_req *r, const uint8_t *src, uint8_t fmt, int s)
{
	int bpp = bpp_of(fmt);
	size_t pitch = (size_t)r->width * bpp;

	for (uint32_t y = 0; y < r->height; y++) {
		const void *sp = src + y * pitch;
		uint32_t *row = (uint32_t *)(fb + (size_t)(r->y + y) * s * fb_stride) + (size_t)r->x * s;

		if (s == 1) {
			if (bpp == 2) row565(row, sp, r->width);
			else row8888(row, sp, r->width);
		} else {
			if (bpp == 2) row565x2(row, sp, r->width);
			else {
				const uint32_t *p = sp;
				for (uint32_t x = 0; x < r->width; x++) row[2 * x] = row[2 * x + 1] = p[x];
			}
			memcpy((uint8_t *)row + fb_stride, row, (size_t)r->width * 2 * 4);
		}
	}
}

static long read_knob(const char *path, long def)
{
	FILE *f = fopen(path, "r");
	long v = def;
	if (f) { if (fscanf(f, "%ld", &v) != 1) v = def; fclose(f); }
	return v;
}

/* 排查开关（/run 下的文件），每秒最多读一次，避免每块数据都做文件系统调用 */
static struct { int noblit, nodirty, verbose, sleep_ms; size_t chunk; uint64_t at; } knob;

static void refresh_knobs(void)
{
	uint64_t t = now_ms();
	if (knob.at && t - knob.at < 1000) return;
	knob.at = t;
	knob.noblit = !access("/run/gudd-noblit", F_OK);
	knob.nodirty = !access("/run/gudd-nodirty", F_OK);
	knob.verbose = !access("/run/gudd-verbose", F_OK);
	knob.sleep_ms = read_knob("/run/gudd-sleep", 0);
	long v = read_knob("/run/gudd-chunk", 65536);
	if (v < 512) v = 512;
	if (v > BULK_MAX) v = BULK_MAX;
	knob.chunk = v & ~511L; /* 保持 maxpacket 整数倍 */
}

/* ---- 接收 / 处理流水线 ----
 * 接收线程只负责从 ep1 读数据进槽位；处理线程负责解压、写屏。
 * 两者并行，USB 接收与 CPU 处理重叠，用上 A7 的第二个核。 */
#define NSLOT 3
struct slot { struct gud_set_buffer_req r; uint8_t fmt; int s, clear; uint8_t *buf; };
static struct slot slots[NSLOT];
static int q_head, q_count;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;

static struct slot *slot_get(void) /* 等待空槽 */
{
	pthread_mutex_lock(&q_lock);
	while (q_count == NSLOT) pthread_cond_wait(&q_cond, &q_lock);
	struct slot *sl = &slots[(q_head + q_count) % NSLOT];
	pthread_mutex_unlock(&q_lock);
	return sl;
}

static void slot_push(void)
{
	pthread_mutex_lock(&q_lock);
	q_count++;
	pthread_cond_broadcast(&q_cond);
	pthread_mutex_unlock(&q_lock);
}

static void *proc_thread(void *arg)
{
	uint8_t *pix = malloc(BULK_MAX);
	uint64_t frames = 0, bytes = 0, busy = 0, t0 = now_ms();

	(void)arg;
	if (!pix) { LOG("内存不足"); exit(1); }
	for (;;) {
		pthread_mutex_lock(&q_lock);
		while (!q_count) pthread_cond_wait(&q_cond, &q_lock);
		struct slot *sl = &slots[q_head];
		pthread_mutex_unlock(&q_lock);

		uint64_t tb = now_ms();
		refresh_knobs();
		if (sl->clear) {
			fb_clear();
		} else {
			const struct gud_set_buffer_req *r = &sl->r;
			const uint8_t *data = sl->buf;
			int ok = 1;
			if (r->compression) {
				int n = LZ4_decompress_safe((const char *)sl->buf, (char *)pix, r->compressed_length, BULK_MAX);
				if (n != (int)r->length) { LOG("LZ4 解压失败 %d/%u", n, r->length); ok = 0; }
				data = pix;
			}
			if (knob.verbose)
				LOG("buf %u,%u %ux%u len=%u comp=%u clen=%u%s", r->x, r->y, r->width, r->height,
				    r->length, r->compression, r->compressed_length, knob.noblit ? " (noblit)" : "");
			if (knob.sleep_ms) usleep(knob.sleep_ms * 1000);
			pthread_mutex_lock(&lock);
			int on = display_on;
			pthread_mutex_unlock(&lock);
			if (ok && on && !knob.noblit) {
				blit(r, data, sl->fmt, sl->s);
				if (!knob.nodirty)
					dirty(r->x * sl->s, r->y * sl->s, r->width * sl->s, r->height * sl->s);
			}
			frames++;
			bytes += r->compression ? r->compressed_length : r->length;
		}
		busy += now_ms() - tb;

		pthread_mutex_lock(&q_lock);
		q_head = (q_head + 1) % NSLOT;
		q_count--;
		pthread_cond_broadcast(&q_cond);
		pthread_mutex_unlock(&q_lock);

		uint64_t t = now_ms();
		if (t - t0 >= 10000) {
			LOG("10s: %llu 次更新, %.1f MB/s, 处理线程占用 %llu%%", (unsigned long long)frames,
			    bytes / 1e3 / (t - t0), (unsigned long long)(busy * 100 / (t - t0)));
			frames = bytes = busy = 0; t0 = t;
		}
	}
	return NULL;
}

static void *bulk_thread(void *arg)
{
	char path[256];
	int ep1 = -1;

	(void)arg;
	for (int i = 0; i < NSLOT; i++)
		if (!(slots[i].buf = malloc(BULK_MAX))) { LOG("内存不足"); exit(1); }
	snprintf(path, sizeof(path), "%s/ep1", ffs_dir);

	for (;;) {
		struct gud_set_buffer_req r;
		uint8_t fmt;
		int s, clear;

		pthread_mutex_lock(&lock);
		while (!job_ready && !need_clear)
			pthread_cond_wait(&cond, &lock);
		clear = need_clear; need_clear = 0;
		r = job;
		fmt = cur.format;
		s = cur.mode >= 0 ? modes[cur.mode].scale : 1;
		int have_job = job_ready;
		uint64_t t_job = job_at;
		job_ready = 0;
		pthread_mutex_unlock(&lock);

		if (clear) { /* 清屏也走队列，保证与写屏顺序一致 */
			struct slot *sl = slot_get();
			sl->clear = 1;
			slot_push();
		}
		if (!have_job) continue;

		if (ep1 < 0 && (ep1 = open(path, O_RDWR)) < 0) { LOG("打开 %s 失败: %m", path); sleep(1); continue; }

		struct slot *sl = slot_get();
		/* 分段读取：A1599 的 dwc2 对“多包且以短包结束”的 OUT 请求会算错实际长度
		 * （实测：请求 9667 报告 9727，随后数据错位、端点卡死）；全满包的大请求正常。
		 * 因此先用 ≤chunk（默认 64 KiB，/run/gudd-chunk 可调）的请求读完整包部分，
		 * 再单独读不足 512 字节的尾部短包。 */
		size_t want = r.compression ? r.compressed_length : r.length, got = 0;
		size_t full = want & ~511UL, chunk = knob.chunk ? knob.chunk : 65536;
		ssize_t n = 0;
		uint64_t t0 = now_us(), t_full = t0;
		while (got < full) {
			size_t len = full - got < chunk ? full - got : chunk;
			n = read(ep1, sl->buf + got, len);
			if (n <= 0) break;
			got += n;
			if ((size_t)n < len) break; /* 意外短包 */
		}
		t_full = now_us();
		if (got == full && want > full) {
			n = read(ep1, sl->buf + got, want - full);
			if (n > 0) got += n;
		}
		if (knob.verbose)
			LOG("rx %zu B: 唤醒 %llu us, 整包 %zu B %llu us, 尾部 %zu B %llu us", want,
			    (unsigned long long)(t0 - t_job), full, (unsigned long long)(t_full - t0),
			    want - full, (unsigned long long)(now_us() - t_full));
		if (got != want) {
			LOG("bulk 读取 %zu/%zu (%s)", got, want, n < 0 ? strerror(errno) : "short");
			if (n < 0) { close(ep1); ep1 = -1; }
			continue;
		}
		/* 心跳：供 gudd-watchdog 判断 USB 是否仍在工作 */
		utimensat(AT_FDCWD, "/run/gudd-hb", NULL, 0);
		sl->r = r; sl->fmt = fmt; sl->s = s; sl->clear = 0;
		slot_push();
	}
	return NULL;
}

/* ---- 背光（PMIC 背光，位于 /sys/class/backlight 下） ----
 * 通过 GUD 背光属性暴露给宿主：宿主生成 card*-USB-*-backlight（0–100）。
 * 宿主关闭显示（DPMS/熄屏）时背光写 0，是最主要的省电手段。 */
static char bl_path[256];
static long bl_max;
static int bl_pct = -1, bl_pending = -1;

static void bl_write(int pct);

/* 亮度百分比另存一份：熄屏时 sysfs 读回 0，gudd 重启后不能据此反推亮度 */
static void bl_save(void)
{
	FILE *f = fopen("/run/gudd-bl-pct", "w");
	if (f) { fprintf(f, "%d\n", bl_pct); fclose(f); }
}

static void bl_init(void)
{
	/* 取 /sys/class/backlight 下第一个设备（A1599：pmic@3c:backlight@600；J207：backlight@b00） */
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	char p[600];
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		snprintf(p, sizeof(p), "/sys/class/backlight/%s/max_brightness", e->d_name);
		long max = read_knob(p, 0);
		if (max <= 0) continue;
		snprintf(bl_path, sizeof(bl_path), "/sys/class/backlight/%s/brightness", e->d_name);
		bl_max = max;
		bl_pct = (int)((read_knob(bl_path, max) * 100 + max / 2) / max);
		long saved = read_knob("/run/gudd-bl-pct", -1);
		if (saved >= 0 && saved <= 100) bl_pct = (int)saved;
		else if (!access("/run/gudd-bl-initialized", F_OK)) {
			/* 从不写该文件的旧版热更新而来：按 sysfs 补记；已是 0 说明处于熄屏，无从得知，按 100% */
			if (bl_pct <= 0) bl_pct = 100;
			bl_save();
		}
		/* 每次开机后首次启动时默认亮度 100%；之后 gudd 重启（热更新、supervisor）保留当前亮度 */
		if (access("/run/gudd-bl-initialized", F_OK)) {
			bl_pct = 100;
			bl_write(bl_pct);
			bl_save();
			close(open("/run/gudd-bl-initialized", O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
		}
		LOG("背光 %s 当前 %d%%", e->d_name, bl_pct);
		closedir(d);
		return;
	}
	if (d) closedir(d);
	LOG("没有找到背光设备");
}

static void bl_write(int pct)
{
	if (!bl_max) return;
	FILE *f = fopen(bl_path, "w");
	if (!f) { LOG("写背光失败: %m"); return; }
	fprintf(f, "%ld\n", pct <= 0 ? 0 : (bl_max * pct + 99) / 100);
	fclose(f);
}

/* ---- 电源键：本地熄屏 ----
 * 按 iPad 电源键切换熄屏/亮屏。只关背光，GUD 链路、画面接收和写缓冲照常，
 * 宿主仍把它当作在线显示器；亮屏时立即显示最新画面。
 * 与宿主 DPMS 相互独立：两者都为"开"时才点亮背光。
 * 状态记在 /run/gudd-local-off，gudd 重启（热更新、supervisor）后保持。 */
static pthread_mutex_t bl_lock = PTHREAD_MUTEX_INITIALIZER;
static int local_off;

static void bl_apply(void)
{
	pthread_mutex_lock(&bl_lock);
	bl_write(display_on && !local_off ? bl_pct : 0);
	pthread_mutex_unlock(&bl_lock);
}

static void *key_thread(void *arg)
{
	struct pollfd pf[8];
	int n = 0;
	(void)arg;
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && (e = readdir(d)) && n < 8) {
		unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
		char p[300];
		if (strncmp(e->d_name, "event", 5)) continue;
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		int fd = open(p, O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0 ||
		    !(bits[KEY_POWER / (8 * sizeof(long))] & (1UL << (KEY_POWER % (8 * sizeof(long)))))) {
			close(fd);
			continue;
		}
		LOG("电源键 %s", p);
		pf[n].fd = fd; pf[n].events = POLLIN; n++;
	}
	if (d) closedir(d);
	if (!n) { LOG("没有找到电源键"); return NULL; }

	int dead = 0;
	for (;;) {
		if (poll(pf, n, -1) < 0) { if (errno != EINTR) { LOG("电源键 poll: %m"); sleep(1); } continue; }
		for (int i = 0; i < n; i++) {
			struct input_event ev[16];
			if (!pf[i].revents) continue;
			ssize_t r = read(pf[i].fd, ev, sizeof(ev));
			if (r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN)) {
				/* 设备被移除（ENODEV）：关掉，poll 忽略负 fd，否则会空转 */
				LOG("电源键设备失效: %m");
				close(pf[i].fd); pf[i].fd = -1;
				if (++dead == n) return NULL;
				continue;
			}
			for (int k = 0; k < r / (ssize_t)sizeof(ev[0]); k++) {
				/* 只认按下（value 1），忽略松开和自动重复 */
				if (ev[k].type != EV_KEY || ev[k].code != KEY_POWER || ev[k].value != 1) continue;
				local_off = !local_off;
				if (local_off) close(open("/run/gudd-local-off", O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
				else unlink("/run/gudd-local-off");
				bl_apply();
				LOG("电源键：%s", local_off ? "熄屏（保持连接）" : "亮屏");
			}
		}
	}
}

/* ---- EDID（按设备树 compatible 选机型，按 fb 尺寸生成首选时序） ---- */
/* ⚠️ 这张表和 rootfs/sbin/setup-gadget 的 charging 分支、以及 DTB 的 compatible
 * 是**同一组板名字符串的三个消费方**，换 DTB 必须三处一起改，否则会出现
 * "能显示但没有 EDID" 或 "完全不充电" 这类静默失效。 */
static uint8_t edid[128];
static int edid_ok;

static void edid_text(uint8_t *d, uint8_t tag, const char *str)
{
	memset(d, 0, 18);
	d[3] = tag;
	size_t n = strlen(str);
	if (n > 13) n = 13;
	memcpy(d + 5, str, n);
	if (n < 13) { d[5 + n] = '\n'; memset(d + 6 + n, ' ', 12 - n); }
}

static void edid_build(const char *name, const char *serial, int wmm, int hmm, uint16_t pid);

static void edid_init(void)
{
	static const struct { const char *compat, *name, *serial; int wmm, hmm; uint16_t pid; } boards[] = {
		{ "apple,j85m", "iPad mini 3", "A1599", 120, 160, 0x1599 },
		/* iPad mini 2 Cellular（iPad4,5）。面板与 mini 3 同为 7.9" 4:3，尺寸一致。
		 * 2026-10-16 实测 m1n1 报 target-type=J86、Model=iPad4,5；A1490 取自内核源码
		 * s5l8960x-j86.dts 的注释，与用户此前"A1600"的说法不符，待确认。
		 * serial 同时也是宿主侧区分两台 mini 的字段。 */
		{ "apple,j86", "iPad mini 2", "A1490", 120, 160, 0x1490 },
		{ "apple,j207", "iPad Pro 10.5", "A1701", 160, 214, 0x1701 },
	};
	char compat[64] = "";
	FILE *f = fopen("/proc/device-tree/compatible", "r");
	if (f) { if (!fgets(compat, sizeof(compat), f)) compat[0] = 0; fclose(f); } /* 第一个字符串 */
	for (size_t i = 0; i < sizeof(boards) / sizeof(boards[0]); i++) {
		if (strcmp(compat, boards[i].compat)) continue;
		edid_build(boards[i].name, boards[i].serial, boards[i].wmm, boards[i].hmm, boards[i].pid);
		LOG("EDID: %s %s", boards[i].name, boards[i].serial);
		return;
	}
	LOG("未知机型 %s，不提供 EDID", compat);
}

static void edid_build(const char *name, const char *serial, int wmm, int hmm, uint16_t pid)
{
	{
		uint8_t *e = edid;
		uint32_t w = fb_w, h = fb_h, hb = 24, vb = 6;
		uint32_t clk = ((w + hb) * (h + vb) * 60 + 5000) / 10000; /* 10 kHz */
		static const uint8_t hdr[8] = { 0, 255, 255, 255, 255, 255, 255, 0 };
		static const uint8_t srgb[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
		memset(e, 0, 128);
		memcpy(e, hdr, 8);
		uint16_t v = ('A' - 64) << 10 | ('P' - 64) << 5 | ('P' - 64); /* PNP: APP */
		e[8] = v >> 8; e[9] = v & 255;
		e[10] = pid & 255; e[11] = pid >> 8;
		uint32_t sn = (pid >> 12) * 1000 + ((pid >> 8) & 15) * 100 + ((pid >> 4) & 15) * 10 + (pid & 15); /* 0x1599 → 1599 */
		e[12] = sn & 255; e[13] = sn >> 8 & 255; e[14] = sn >> 16 & 255; e[15] = sn >> 24;
		e[16] = 42; e[17] = 24; e[18] = 1; e[19] = 3;
		e[20] = 0x80; e[21] = (wmm + 5) / 10; e[22] = (hmm + 5) / 10;
		e[23] = 120; e[24] = 0x0A;
		memcpy(e + 25, srgb, 10);
		memset(e + 38, 1, 16);
		uint8_t *d = e + 54;
		d[0] = clk & 255; d[1] = clk >> 8;
		d[2] = w & 255; d[3] = hb; d[4] = (w >> 8) << 4;
		d[5] = h & 255; d[6] = vb; d[7] = (h >> 8) << 4;
		d[8] = 8; d[9] = 8; d[10] = 2 << 4 | 2;
		d[12] = wmm & 255; d[13] = hmm & 255;
		d[14] = (wmm >> 8) << 4 | (hmm >> 8);
		d[17] = 0x18;
		edid_text(e + 72, 0xFC, name);
		edid_text(e + 90, 0xFF, serial);
		e[111] = 0x10;
		uint8_t sum = 0;
		for (int j = 0; j < 127; j++) sum += e[j];
		e[127] = -sum;
		edid_ok = 1;
	}
}

/* ---- ep0 ---- */
static void reply(const void *data, size_t len, uint16_t wlen)
{
	if (len > wlen) len = wlen;
	if (write(ep0, data, len) < 0) LOG("ep0 写失败: %m");
}

static void stall(int dir_in)
{
	/* FunctionFS：反方向 I/O 即 stall */
	if (dir_in) { if (read(ep0, NULL, 0) < 0 && errno != EL2HLT) LOG("stall: %m"); }
	else { if (write(ep0, NULL, 0) < 0 && errno != EL2HLT) LOG("stall: %m"); }
}

static void mode_req(int i, struct gud_display_mode_req *m)
{
	uint16_t w = modes[i].w, h = modes[i].h;
	memset(m, 0, sizeof(*m));
	m->hdisplay = htole16(w); m->hsync_start = htole16(w + 8);
	m->hsync_end = htole16(w + 16); m->htotal = htole16(w + 24);
	m->vdisplay = htole16(h); m->vsync_start = htole16(h + 2);
	m->vsync_end = htole16(h + 4); m->vtotal = htole16(h + 6);
	/* 与 EDID 的 10 kHz 精度一致，避免出现两条几乎相同的模式 */
	m->clock = htole32((uint32_t)(((uint64_t)(w + 24) * (h + 6) * 60 + 5000) / 10000 * 10));
	m->flags = htole32(i == 0 ? GUD_DISPLAY_MODE_FLAG_PREFERRED : 0);
}

static void handle_in(const struct usb_ctrlrequest *c)
{
	uint16_t wlen = le16toh(c->wLength), idx = le16toh(c->wValue);

	switch (c->bRequest) {
	case GUD_REQ_GET_STATUS:
		reply(&last_status, 1, wlen);
		return;
	case GUD_REQ_GET_DESCRIPTOR: {
		struct gud_display_descriptor_req d = {
			.magic = htole32(GUD_DISPLAY_MAGIC), .version = 1,
			.flags = htole32(GUD_DISPLAY_FLAG_STATUS_ON_SET),
			.compression = GUD_COMPRESSION_LZ4,
			.max_buffer_size = htole32(BULK_MAX),
			.min_width = htole32(modes[nmodes - 1].w), .max_width = htole32(modes[0].w),
			.min_height = htole32(modes[nmodes - 1].h), .max_height = htole32(modes[0].h),
		};
		reply(&d, sizeof(d), wlen);
		break;
	}
	case GUD_REQ_GET_FORMATS:
		reply(formats, nformats, wlen);
		break;
	case GUD_REQ_GET_PROPERTIES:
		reply(NULL, 0, wlen);
		break;
	case GUD_REQ_GET_CONNECTOR_PROPERTIES:
		if (bl_max && !idx) {
			struct gud_property_req pr = { htole16(GUD_PROPERTY_BACKLIGHT_BRIGHTNESS), htole64(bl_pct) };
			reply(&pr, sizeof(pr), wlen);
		} else {
			reply(NULL, 0, wlen);
		}
		break;
	case GUD_REQ_GET_CONNECTOR_EDID:
		/* 合成 EDID：让宿主得到稳定的描述（如 Apple Computer Inc iPad mini 3 A1599）和物理尺寸 */
		if (idx || !edid_ok) reply(NULL, 0, wlen);
		else reply(edid, sizeof(edid), wlen);
		break;
	case GUD_REQ_GET_CONNECTORS: {
		/* POLL_STATUS：宿主每 10 秒查询一次连接状态，作为链路心跳（见 usb-linkwatch） */
		struct gud_connector_descriptor_req d = { GUD_CONNECTOR_TYPE_PANEL, htole32(GUD_CONNECTOR_FLAGS_POLL_STATUS) };
		reply(&d, sizeof(d), wlen);
		break;
	}
	case GUD_REQ_GET_CONNECTOR_STATUS: {
		uint8_t st = GUD_CONNECTOR_STATUS_CONNECTED;
		if (idx) { stall(1); last_status = ST_INVAL; return; }
		reply(&st, 1, wlen);
		break;
	}
	case GUD_REQ_GET_CONNECTOR_MODES: {
		struct gud_display_mode_req m[2];
		if (idx) { stall(1); last_status = ST_INVAL; return; }
		for (int i = 0; i < nmodes; i++) mode_req(i, &m[i]);
		reply(m, nmodes * sizeof(m[0]), wlen);
		break;
	}
	default:
		LOG("不支持的 IN 请求 0x%02x", c->bRequest);
		stall(1);
		last_status = ST_NOTSUPP;
		return;
	}
	last_status = ST_OK;
}

static uint8_t do_set(uint8_t req, uint16_t idx, const uint8_t *buf, size_t len)
{
	switch (req) {
	case GUD_REQ_SET_CONNECTOR_FORCE_DETECT:
		return idx ? ST_INVAL : ST_OK;
	case GUD_REQ_SET_CONTROLLER_ENABLE:
		return len == 1 ? ST_OK : ST_PROTO;
	case GUD_REQ_SET_DISPLAY_ENABLE:
		if (len != 1) return ST_PROTO;
		pthread_mutex_lock(&lock);
		display_on = buf[0];
		if (!display_on) { need_clear = 1; pthread_cond_signal(&cond); }
		pthread_mutex_unlock(&lock);
		bl_apply(); /* 熄屏关背光 */
		LOG("display %s", display_on ? "on" : "off");
		return ST_OK;
	case GUD_REQ_SET_STATE_CHECK: {
		struct gud_state_req s;
		int i, f;
		if (len < sizeof(s)) return ST_PROTO;
		memcpy(&s, buf, sizeof(s));
		for (i = 0; i < nmodes; i++)
			if (le16toh(s.mode.hdisplay) == modes[i].w && le16toh(s.mode.vdisplay) == modes[i].h) break;
		for (f = 0; f < nformats; f++)
			if (formats[f] == s.format) break;
		if (i == nmodes || f == nformats || s.connector) {
			LOG("拒绝状态 %ux%u fmt=0x%02x", le16toh(s.mode.hdisplay), le16toh(s.mode.vdisplay), s.format);
			return ST_INVAL;
		}
		pending.mode = i; pending.format = s.format;
		for (size_t off = sizeof(s); off + sizeof(struct gud_property_req) <= len; off += sizeof(struct gud_property_req)) {
			struct gud_property_req pr;
			memcpy(&pr, buf + off, sizeof(pr));
			if (le16toh(pr.prop) == GUD_PROPERTY_BACKLIGHT_BRIGHTNESS && le64toh(pr.val) <= 100)
				bl_pending = (int)le64toh(pr.val);
		}
		return ST_OK;
	}
	case GUD_REQ_SET_STATE_COMMIT:
		if (pending.mode < 0) return ST_PROTO;
		pthread_mutex_lock(&lock);
		if (pending.mode != cur.mode) need_clear = 1;
		cur = pending;
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&lock);
		if (bl_pending >= 0 && bl_pending != bl_pct) {
			bl_pct = bl_pending;
			bl_save();
			bl_apply();
			LOG("背光 %d%%", bl_pct);
		}
		LOG("模式 %ux%u (x%d) fmt=0x%02x", modes[cur.mode].w, modes[cur.mode].h,
		    modes[cur.mode].scale, cur.format);
		return ST_OK;
	case GUD_REQ_SET_BUFFER: {
		struct gud_set_buffer_req r;
		if (len != sizeof(r) || cur.mode < 0) return ST_PROTO;
		memcpy(&r, buf, sizeof(r));
		r.x = le32toh(r.x); r.y = le32toh(r.y); r.width = le32toh(r.width);
		r.height = le32toh(r.height); r.length = le32toh(r.length);
		r.compressed_length = le32toh(r.compressed_length);
		const struct mode *m = &modes[cur.mode];
		if (!r.width || !r.height || r.x + r.width > m->w || r.y + r.height > m->h ||
		    r.length != (uint64_t)r.width * r.height * bpp_of(cur.format) ||
		    (r.compression & ~GUD_COMPRESSION_LZ4) ||
		    r.length > BULK_MAX ||
		    (r.compression && (!r.compressed_length || r.compressed_length > BULK_MAX)))
			return ST_INVAL;
		pthread_mutex_lock(&lock);
		if (job_ready) LOG("上一次 bulk 尚未处理，覆盖");
		job = r; job_ready = 1; job_at = now_us();
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&lock);
		return ST_OK;
	}
	default:
		LOG("不支持的 OUT 请求 0x%02x", req);
		return ST_NOTSUPP;
	}
}

static void handle_out(const struct usb_ctrlrequest *c)
{
	uint8_t buf[512];
	uint16_t wlen = le16toh(c->wLength);

	if (wlen > sizeof(buf)) { stall(0); last_status = ST_PROTO; return; }
	ssize_t n = read(ep0, buf, wlen);
	if (n < 0) { LOG("ep0 读失败: %m"); last_status = ST_ERROR; return; }
	last_status = do_set(c->bRequest, le16toh(c->wValue), buf, n);
}

/* ---- FunctionFS 描述符 ---- */
/* 两个目标均为小端，静态初始化直接用常量 */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little endian only");
#define L32(x) (x)
#define L16(x) (x)
static const struct {
	struct usb_functionfs_descs_head_v2 header;
	uint32_t fs_count, hs_count;
	struct {
		struct usb_interface_descriptor intf;
		struct usb_endpoint_descriptor_no_audio ep;
	} __attribute__((packed)) fs, hs;
} __attribute__((packed)) descriptors = {
	.header = {
		.magic = L32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
		.flags = L32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC),
		.length = L32(sizeof(descriptors)),
	},
	.fs_count = L32(2), .hs_count = L32(2),
#define DESC(mps) { \
	.intf = { .bLength = sizeof(struct usb_interface_descriptor), .bDescriptorType = USB_DT_INTERFACE, \
		  .bNumEndpoints = 1, .bInterfaceClass = USB_CLASS_VENDOR_SPEC, .iInterface = 1 }, \
	.ep = { .bLength = sizeof(struct usb_endpoint_descriptor_no_audio), .bDescriptorType = USB_DT_ENDPOINT, \
		.bEndpointAddress = 1 | USB_DIR_OUT, .bmAttributes = USB_ENDPOINT_XFER_BULK, \
		.wMaxPacketSize = L16(mps) } }
	.fs = DESC(64), .hs = DESC(512),
};

#define STR_INTF "A1599 GUD Display"
static const struct {
	struct usb_functionfs_strings_head header;
	struct { uint16_t code; char str1[sizeof(STR_INTF)]; } __attribute__((packed)) lang0;
} __attribute__((packed)) strings = {
	.header = {
		.magic = L32(FUNCTIONFS_STRINGS_MAGIC), .length = L32(sizeof(strings)),
		.str_count = L32(1), .lang_count = L32(1),
	},
	.lang0 = { L16(0x0409), STR_INTF },
};

static int setup_drm(void)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t crtcs[8], conns[8];
	const char *dev = getenv("GUDD_DRM") ? getenv("GUDD_DRM") : "/dev/dri/card0";
	int fd = open(dev, O_RDWR | O_CLOEXEC);

	if (fd < 0) { LOG("%s: %m", dev); return -1; }
	const char *step = "SET_MASTER";
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0)) LOG("SET_MASTER: %m（继续尝试）");
	step = "GETRESOURCES";
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs || !res.count_connectors ||
	    res.count_crtcs > 8 || res.count_connectors > 8) goto fail;
	res.count_fbs = res.count_encoders = 0;
	res.crtc_id_ptr = (uintptr_t)crtcs; res.connector_id_ptr = (uintptr_t)conns;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) goto fail;

	struct drm_mode_get_connector conn = { .connector_id = conns[0] };
	step = "GETCONNECTOR";
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || !conn.count_modes) goto fail;
	struct drm_mode_modeinfo *m = calloc(conn.count_modes, sizeof(*m));
	uint32_t n = conn.count_modes;
	memset(&conn, 0, sizeof(conn));
	conn.connector_id = conns[0]; conn.count_modes = n; conn.modes_ptr = (uintptr_t)m;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn)) goto fail;

	struct drm_mode_create_dumb cd = { .width = m[0].hdisplay, .height = m[0].vdisplay, .bpp = 32 };
	step = "CREATE_DUMB";
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) goto fail;
	struct drm_mode_fb_cmd fbc = { .width = cd.width, .height = cd.height, .pitch = cd.pitch,
				       .bpp = 32, .depth = 24, .handle = cd.handle };
	step = "ADDFB";
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbc)) goto fail;
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	step = "MAP_DUMB";
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) goto fail;
	fb = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	step = "MMAP";
	if (fb == MAP_FAILED) goto fail;
	struct drm_mode_crtc sc = { .crtc_id = crtcs[0], .fb_id = fbc.fb_id,
				    .set_connectors_ptr = (uintptr_t)&conns[0], .count_connectors = 1,
				    .mode = m[0], .mode_valid = 1 };
	step = "SETCRTC";
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) goto fail;

	fb_w = cd.width; fb_h = cd.height; fb_stride = cd.pitch;
	fb_roff = 16; fb_goff = 8; fb_boff = 0;
	drm_fd = fd; drm_fb_id = fbc.fb_id;
	LOG("DRM dumb 后端: crtc=%u conn=%u fb=%u", crtcs[0], conns[0], drm_fb_id);
	return 0;
fail:
	LOG("DRM 后端不可用：%s 失败 (%m)，改用 fbdev", step);
	close(fd);
	return -1;
}

static void setup_fb(const char *test, int use_fbdev)
{
	if (!test && !use_fbdev && !setup_drm()) {
		/* DRM 已就绪 */
	} else if (test) {
		char path[200];
		if (sscanf(test, "%199[^:]:%ux%u", path, &fb_w, &fb_h) != 3) { LOG("-F 参数格式: 文件:宽x高"); exit(2); }
		fb_stride = fb_w * 4;
		int fd = open(path, O_RDWR | O_CREAT, 0644);
		if (fd < 0 || ftruncate(fd, (off_t)fb_stride * fb_h)) { LOG("%s: %m", path); exit(1); }
		fb = mmap(NULL, (size_t)fb_stride * fb_h, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	} else {
		struct fb_var_screeninfo v;
		struct fb_fix_screeninfo f;
		int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
		if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f) ||
		    v.bits_per_pixel != 32 || v.red.length != 8 || v.green.length != 8 || v.blue.length != 8) {
			LOG("/dev/fb0 不可用或不是 32bpp"); exit(1);
		}
		fb_w = v.xres; fb_h = v.yres; fb_stride = f.line_length;
		fb_roff = v.red.offset; fb_goff = v.green.offset; fb_boff = v.blue.offset;
		fb = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		/* 存在 /run/gudd-kdgraphics 时让 fbcon 停止绘制（排查期间默认不设，便于看到内核 oops） */
		if (!access("/run/gudd-kdgraphics", F_OK)) {
			int tty = open("/dev/tty0", O_RDWR | O_CLOEXEC | O_NOCTTY);
			if (tty >= 0 && ioctl(tty, KDSETMODE, KD_GRAPHICS)) LOG("KD_GRAPHICS: %m");
		}
	}
	if (fb == MAP_FAILED) { LOG("mmap fb: %m"); exit(1); }
	modes[0] = (struct mode){ fb_w, fb_h, 1 };
	modes[1] = (struct mode){ fb_w / 2, fb_h / 2, 2 };
	nmodes = 2;
	LOG("fb %ux%u stride=%u", fb_w, fb_h, fb_stride);
}

/* 崩溃时把信号和地址写进日志（只用 async-signal-safe 的 write） */
static void crash_handler(int sig, siginfo_t *si, void *uc)
{
	char buf[96];
	(void)uc;
	int n = snprintf(buf, sizeof(buf), "gudd: 致命信号 %d addr=%p\n", sig, si->si_addr);
	if (n > 0) { ssize_t r = write(2, buf, n); (void)r; }
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char **argv)
{
	const char *test = NULL;
	char path[256];
	int opt, xrgb = 0, use_fbdev = 0;
	pthread_t th;

	while ((opt = getopt(argc, argv, "F:xb")) != -1) {
		if (opt == 'F') test = optarg;
		else if (opt == 'x') xrgb = 1;
		else if (opt == 'b') use_fbdev = 1;
		else return 2;
	}
	if (optind != argc - 1) { fprintf(stderr, "用法: gudd [-F 文件:宽x高] [-x] [-b] <ffs 目录>\n"); return 2; }
	ffs_dir = argv[optind];
	setvbuf(stderr, NULL, _IOLBF, 0);
	struct sigaction sa = { .sa_sigaction = crash_handler, .sa_flags = SA_SIGINFO };
	sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL); sigaction(SIGFPE, &sa, NULL); sigaction(SIGILL, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* 心跳文件需预先存在，utimensat 只更新不创建 */
	close(open("/run/usb-hb", O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
	close(open("/run/gudd-hb", O_CREAT | O_WRONLY | O_CLOEXEC, 0644));
	formats[nformats++] = GUD_PIXEL_FORMAT_RGB565;
	if (xrgb) formats[nformats++] = GUD_PIXEL_FORMAT_XRGB8888;
	setup_fb(test, use_fbdev);
	if (!test) {
		bl_init(); edid_init();
		if (!access("/run/gudd-local-off", F_OK)) { local_off = 1; bl_write(0); LOG("保持电源键熄屏"); }
	}
	fb_clear();

	snprintf(path, sizeof(path), "%s/ep0", ffs_dir);
	if ((ep0 = open(path, O_RDWR)) < 0) { LOG("%s: %m", path); return 1; }
	if (write(ep0, &descriptors, sizeof(descriptors)) < 0) { LOG("写描述符: %m"); return 1; }
	if (write(ep0, &strings, sizeof(strings)) < 0) { LOG("写字符串: %m"); return 1; }
	LOG("描述符已写入，等待宿主");
	refresh_knobs();
	pthread_create(&th, NULL, proc_thread, NULL);
	pthread_create(&th, NULL, bulk_thread, NULL);
	if (drm_fd >= 0) pthread_create(&th, NULL, flush_thread, NULL);
	if (!test) pthread_create(&th, NULL, key_thread, NULL);

	int errs = 0;
	for (;;) {
		struct usb_functionfs_event ev[4];
		ssize_t n = read(ep0, ev, sizeof(ev));
		if (n < 0) {
			if (errno == EINTR) continue;
			/* 宿主取消/超时的控制请求会让 ep0 返回 EL2HLT、EIDRM 等可恢复错误，不能因此退出 */
			LOG("ep0 事件读取错误（第 %d 次）: %m", ++errs);
			if (errs >= 100) { LOG("ep0 连续出错，退出"); return 1; }
			usleep(10000);
			continue;
		}
		errs = 0;
		for (int i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
			switch (ev[i].type) {
			case FUNCTIONFS_SETUP: {
				const struct usb_ctrlrequest *c = &ev[i].u.setup;
				/* 链路心跳：收到宿主的任何控制请求 */
				utimensat(AT_FDCWD, "/run/usb-hb", NULL, 0);
				if ((c->bRequestType & USB_TYPE_MASK) != USB_TYPE_VENDOR) {
					stall(c->bRequestType & USB_DIR_IN);
					break;
				}
				if (c->bRequestType & USB_DIR_IN) handle_in(c);
				else handle_out(c);
				break;
			}
			case FUNCTIONFS_ENABLE: LOG("enable"); break;
			case FUNCTIONFS_DISABLE:
				LOG("disable");
				pthread_mutex_lock(&lock);
				cur.mode = pending.mode = -1; job_ready = 0;
				pthread_mutex_unlock(&lock);
				break;
			case FUNCTIONFS_BIND: LOG("bind"); break;
			case FUNCTIONFS_UNBIND: LOG("unbind"); break;
			default: break;
			}
		}
	}
}
