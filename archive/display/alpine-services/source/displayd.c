#define _DEFAULT_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <jpeglib.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FB_W 1536u
#define FB_H 2048u
#define FB_BPP 4u
#define MAX_JPEG (8u * 1024u * 1024u)
#define MAX_JPEG_PIXELS (8u * 1024u * 1024u)
#define MAX_FB_BYTES (32u * 1024u * 1024u)
#define FRAME_TIMEOUT_MS 10000
#define DISPLAYD_AS_LIMIT (128u * 1024u * 1024u)

static volatile sig_atomic_t stop_requested;
static uint8_t *fb, *snapshot, *decoded, *packet;
static size_t fb_bytes, fb_stride;
static jmp_buf jpeg_jmp;
#ifdef DISPLAYD_TEST_BACKEND
static uint8_t test_fb[(size_t)FB_W * FB_H * FB_BPP];
#endif

struct jpeg_failure { struct jpeg_error_mgr pub; };

static void jpeg_fail(j_common_ptr cinfo) {
  (void)cinfo;
  longjmp(jpeg_jmp, 1);
}

static void on_signal(int sig) { (void)sig; stop_requested = 1; }

static uint32_t crc32_bytes(const uint8_t *p, size_t n) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (unsigned b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
  }
  return ~crc;
}

static int write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = buf;
  while (len) {
    ssize_t n = write(fd, p, len);
    if (n > 0) { p += (size_t)n; len -= (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

static int send_line(int fd, const char *s) {
  return write_all(fd, s, strlen(s));
}

static int read_line(int fd, char *buf, size_t cap) {
  size_t used = 0;
  for (;;) {
    struct pollfd pfd = {fd, POLLIN, 0};
    int pr = poll(&pfd, 1, -1);
    if (pr < 0 && errno == EINTR) { if (stop_requested) return -2; continue; }
    if (pr <= 0 || (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))) return -1;
    char ch;
    ssize_t n = read(fd, &ch, 1);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    if (ch == '\n') { if (used && buf[used - 1] == '\r') --used; buf[used] = 0; return (int)used; }
    if (used + 1 >= cap) {
      do {
        struct pollfd drain = {fd, POLLIN, 0};
        if (poll(&drain, 1, 1000) <= 0 || read(fd, &ch, 1) != 1) break;
      } while (ch != '\n');
      return -3;
    }
    buf[used++] = ch;
  }
}

static int read_packet(int fd, uint8_t *dst, size_t len) {
  size_t done = 0;
  while (done < len) {
    struct pollfd pfd = {fd, POLLIN, 0};
    int pr = poll(&pfd, 1, FRAME_TIMEOUT_MS);
    if (pr < 0 && errno == EINTR) { if (stop_requested) return -1; continue; }
    if (pr == 0) return -2;
    if (pr < 0 || (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))) return -1;
    ssize_t n = read(fd, dst + done, len - done);
    if (n > 0) { done += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

static int decode_jpeg(const uint8_t *data, size_t len) {
  struct jpeg_decompress_struct *c = calloc(1, sizeof(*c));
  struct jpeg_failure *err = calloc(1, sizeof(*err));
  if (!c || !err) { free(c); free(err); return -1; }
  c->err = jpeg_std_error(&err->pub);
  err->pub.error_exit = jpeg_fail;
  if (setjmp(jpeg_jmp)) {
    jpeg_destroy_decompress(c); free(c); free(err); return -1;
  }
  jpeg_create_decompress(c);
  jpeg_mem_src(c, data, (unsigned long)len);
  if (jpeg_read_header(c, TRUE) != JPEG_HEADER_OK ||
      !c->image_width || !c->image_height || c->image_width > 4096 ||
      c->image_height > 4096 || (uint64_t)c->image_width * c->image_height > MAX_JPEG_PIXELS) {
    jpeg_destroy_decompress(c); free(c); free(err); return -1;
  }
  c->out_color_space = JCS_RGB;
  c->scale_num = 1; c->scale_denom = 1;
  jpeg_start_decompress(c);
  if (c->output_components != 3 || !c->output_width || !c->output_height ||
      (uint64_t)c->output_width * c->output_height > MAX_JPEG_PIXELS) {
    jpeg_destroy_decompress(c); free(c); free(err); return -1;
  }
  size_t row_bytes = (size_t)c->output_width * 3u;
  JSAMPARRAY row = (*c->mem->alloc_sarray)((j_common_ptr)c, JPOOL_IMAGE,
                                           (JDIMENSION)row_bytes, 1);
  memset(decoded, 0, fb_bytes);
  uint64_t by_w = (uint64_t)FB_W * c->output_height;
  uint64_t by_h = (uint64_t)FB_H * c->output_width;
  unsigned dw, dh;
  if (by_w <= by_h) { dw = FB_W; dh = (unsigned)((uint64_t)c->output_height * FB_W / c->output_width); }
  else { dh = FB_H; dw = (unsigned)((uint64_t)c->output_width * FB_H / c->output_height); }
  if (!dw) dw = 1;
  if (!dh) dh = 1;
  unsigned ox = (FB_W - dw) / 2u, oy = (FB_H - dh) / 2u;
  unsigned dy = 0;
  while (c->output_scanline < c->output_height) {
    JDIMENSION sy = c->output_scanline;
    if (jpeg_read_scanlines(c, row, 1) != 1) { jpeg_destroy_decompress(c); free(c); free(err); return -1; }
    while (dy < dh && (uint64_t)dy * c->output_height / dh == sy) {
      uint8_t *dst = decoded + (size_t)(oy + dy) * fb_stride + (size_t)ox * FB_BPP;
      for (unsigned x = 0; x < dw; ++x) {
        unsigned sx = (unsigned)((uint64_t)x * c->output_width / dw);
        const uint8_t *src = row[0] + (size_t)sx * 3u;
        dst[(size_t)x * 4u + 0] = src[2];
        dst[(size_t)x * 4u + 1] = src[1];
        dst[(size_t)x * 4u + 2] = src[0];
        dst[(size_t)x * 4u + 3] = 0;
      }
      ++dy;
    }
  }
  int ok = jpeg_finish_decompress(c);
  jpeg_destroy_decompress(c);
  free(c); free(err);
  return ok && dy == dh ? 0 : -1;
}

static int setup_fb(int *fd_out) {
#ifdef DISPLAYD_TEST_BACKEND
  fb_stride = (size_t)FB_W * FB_BPP;
  fb_bytes = fb_stride * FB_H;
  memset(test_fb, 0xa5, sizeof(test_fb));
  fb = test_fb;
  *fd_out = -1;
  return 0;
#else
  int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (fd < 0) return -1;
  struct fb_var_screeninfo v;
  struct fb_fix_screeninfo f;
  if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f) ||
      v.xres != FB_W || v.yres != FB_H || v.xres_virtual < FB_W ||
      v.yres_virtual < FB_H || v.bits_per_pixel != 32 ||
      v.red.offset != 16 || v.red.length != 8 || v.green.offset != 8 ||
      v.green.length != 8 || v.blue.offset != 0 || v.blue.length != 8 ||
      f.line_length < FB_W * FB_BPP ||
      (uint64_t)f.line_length * FB_H > MAX_FB_BYTES ||
      f.smem_len < (uint64_t)f.line_length * FB_H ||
      f.smem_len > MAX_FB_BYTES ||
      f.visual != FB_VISUAL_TRUECOLOR || f.type != FB_TYPE_PACKED_PIXELS) {
    close(fd); return -1;
  }
  fb_stride = f.line_length;
  fb_bytes = f.smem_len;
  fb = mmap(NULL, fb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (fb == MAP_FAILED) { fb = NULL; close(fd); return -1; }
  *fd_out = fd;
  return 0;
#endif
}

static int handle_frame(int tty, uint32_t *next_seq, unsigned seq,
                        size_t len, uint32_t expected_crc) {
  if (seq != *next_seq || len == 0 || len > MAX_JPEG) return -1;
  packet = malloc(len);
  if (!packet) return -1;
  char line[96];
  snprintf(line, sizeof(line), "FRAME-READY seq=%u\n", seq);
  if (send_line(tty, line) || read_packet(tty, packet, len) != 0) { free(packet); packet = NULL; return -1; }
  uint32_t actual_crc = crc32_bytes(packet, len);
  if (actual_crc != expected_crc || decode_jpeg(packet, len) != 0) {
    snprintf(line, sizeof(line), "FRAME-ERR seq=%u code=%s\n", seq,
             actual_crc != expected_crc ? "crc" : "jpeg");
    (void)send_line(tty, line);
    free(packet); packet = NULL;
    return -1;
  }
  memcpy(fb, decoded, fb_bytes);
#ifdef DISPLAYD_TEST_BACKEND
  fprintf(stderr, "framebuffer_crc=%08x\n", crc32_bytes(fb, fb_bytes));
#endif
  snprintf(line, sizeof(line), "FRAME-OK seq=%u bytes=%zu crc=%08x\n", seq, len, actual_crc);
  free(packet); packet = NULL;
  if (send_line(tty, line)) return -1;
  *next_seq = seq + 1u;
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "--decode-only")) {
    FILE *f = fopen(argv[2], "rb");
    if (!f || fseek(f, 0, SEEK_END) || ftell(f) <= 0 || (unsigned long)ftell(f) > MAX_JPEG) return 2;
    long n = ftell(f); rewind(f);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    fb_bytes = (size_t)FB_W * FB_H * FB_BPP; fb_stride = (size_t)FB_W * FB_BPP;
    decoded = malloc(fb_bytes);
    if (!decoded || decode_jpeg(buf, (size_t)n)) return 3;
    printf("JPEG decode OK bytes=%ld output=%ux%u fb=%ux%u\n", n, FB_W, FB_H, FB_W, FB_H);
    free(decoded); free(buf); return 0;
  }
  if (argc != 2) return 2;
  signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, on_signal);
  int tty = open(argv[1], O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (tty < 0) return 2;
  struct rlimit as_limit = {DISPLAYD_AS_LIMIT, DISPLAYD_AS_LIMIT};
  if (setrlimit(RLIMIT_AS, &as_limit) != 0) { close(tty); return 2; }
  struct termios saved, raw;
  int fb_fd = -1;
  if (tcgetattr(tty, &saved) || setup_fb(&fb_fd)) return 2;
  raw = saved;
  cfmakeraw(&raw);
  raw.c_cflag |= CLOCAL | CREAD;
  raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
  if (tcsetattr(tty, TCSANOW, &raw)) {
    munmap(fb, fb_bytes); close(fb_fd); close(tty); return 2;
  }
  snapshot = malloc(fb_bytes); decoded = malloc(fb_bytes);
  if (!snapshot || !decoded) return 2;
  int active = 0, snapshot_valid = 0;
  uint32_t next_seq = 1;
  (void)send_line(tty, "DISPLAYD 1 READY\n");
  while (!stop_requested) {
    char line[96];
    int n = read_line(tty, line, sizeof(line));
    if (n == -2) break;
    if (n < 0) { if (n == -3) (void)send_line(tty, "ERR line-too-long\n"); else break; continue; }
    if (!strcmp(line, "HELLO")) { (void)send_line(tty, "DISPLAYD 1 READY\n"); continue; }
    if (!strcmp(line, "START")) {
      memcpy(snapshot, fb, fb_bytes); snapshot_valid = 1;
      active = 1; next_seq = 1;
      (void)send_line(tty, "SESSION-READY seq=1\n");
      continue;
    }
    if (!strcmp(line, "STOP")) {
      if (snapshot_valid) memcpy(fb, snapshot, fb_bytes);
#ifdef DISPLAYD_TEST_BACKEND
      fprintf(stderr, "restored_crc=%08x\n", crc32_bytes(fb, fb_bytes));
#endif
      snapshot_valid = active = 0; next_seq = 1;
      (void)send_line(tty, "SESSION-STOPPED\n");
      continue;
    }
    unsigned seq, bytes; unsigned crc;
    char extra;
    if (sscanf(line, "FRAME %u %u %x%c", &seq, &bytes, &crc, &extra) == 3) {
      if (!active) { (void)send_line(tty, "FRAME-ERR code=no-session\n"); continue; }
      if (handle_frame(tty, &next_seq, seq, bytes, crc) != 0) {
        active = 0;
        (void)send_line(tty, "SESSION-ENDED; send START to begin again\n");
      }
      continue;
    }
    (void)send_line(tty, "ERR command\n");
  }
  if (snapshot_valid && fb) memcpy(fb, snapshot, fb_bytes);
  if (tcsetattr(tty, TCSANOW, &saved)) { close(tty); return 4; }
  if (decoded) free(decoded);
  if (snapshot) free(snapshot);
  if (packet) free(packet);
  if (fb) munmap(fb, fb_bytes);
  if (fb_fd >= 0) close(fb_fd);
  close(tty);
  return 0;
}
