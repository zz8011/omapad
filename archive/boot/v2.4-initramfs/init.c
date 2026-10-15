/*
 * Minimal A1599 initramfs PID 1. It mounts only virtual filesystems, enables
 * one USB ACM configfs function, and provides a tiny command console on
 * ttyGS0. It contains no block-device, network, telnet, or host-mount logic.
 */
typedef unsigned long usize;
typedef long isize;

#define AT_FDCWD (-100)
#define O_WRONLY 1
#define O_RDONLY 0
#define O_RDWR 2
#define O_DIRECTORY 0x4000 /* AArch64 asm/fcntl.h: 1 << 14 */
#define O_PATH 0x200000
#define MODE 0755

static int diag_fd = 1;
static int kmsg_fd = -1;

static inline isize syscall3(usize n, usize a, usize b, usize c) {
  register usize x0 asm("x0") = a;
  register usize x1 asm("x1") = b;
  register usize x2 asm("x2") = c;
  register usize x8 asm("x8") = n;
  asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return (isize)x0;
}

static inline isize syscall4(usize n, usize a, usize b, usize c, usize d) {
  register usize x0 asm("x0") = a;
  register usize x1 asm("x1") = b;
  register usize x2 asm("x2") = c;
  register usize x3 asm("x3") = d;
  register usize x8 asm("x8") = n;
  asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return (isize)x0;
}

static inline isize syscall5(usize n, usize a, usize b, usize c, usize d, usize e) {
  register usize x0 asm("x0") = a;
  register usize x1 asm("x1") = b;
  register usize x2 asm("x2") = c;
  register usize x3 asm("x3") = d;
  register usize x4 asm("x4") = e;
  register usize x8 asm("x8") = n;
  asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return (isize)x0;
}

static usize slen(const char *s) { usize n = 0; while (s[n]) n++; return n; }
static int eq(const char *a, const char *b) {
  usize i = 0;
  while (a[i] && b[i] && a[i] == b[i]) i++;
  return a[i] == b[i];
}

static isize write_all(int fd, const char *s, usize n) {
  usize done = 0;
  while (done < n) {
    isize r = syscall3(64, fd, (usize)(s + done), n - done);
    if (r < 0) return r;
    if (r == 0) return -5;
    done += (usize)r;
  }
  return (isize)done;
}

static void say(int fd, const char *s) { write_all(fd, s, slen(s)); }

static void append_char(char *buf, usize cap, usize *used, char ch) {
  if (*used + 1 < cap) buf[(*used)++] = ch;
}

static void append_text(char *buf, usize cap, usize *used, const char *s) {
  while (*s) append_char(buf, cap, used, *s++);
}

static void append_uint(char *buf, usize cap, usize *used, usize value) {
  char digits[24];
  usize n = 0;
  do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
  while (n) append_char(buf, cap, used, digits[--n]);
}

static usize errno_value(isize result) {
  return result < 0 ? (usize)(-(result + 1)) + 1 : (usize)result;
}

static void report(const char *stage, isize result) {
  char line[224];
  usize used = 0;
  append_text(line, sizeof(line), &used, "init: ");
  append_text(line, sizeof(line), &used, stage);
  if (result < 0) {
    append_text(line, sizeof(line), &used, " failed errno=");
    append_uint(line, sizeof(line), &used, errno_value(result));
  } else {
    append_text(line, sizeof(line), &used, " ok");
  }
  append_char(line, sizeof(line), &used, '\n');

  if (kmsg_fd >= 0) {
    char record[228];
    record[0] = '<'; record[1] = '6'; record[2] = '>';
    for (usize i = 0; i < used; i++) record[i + 3] = line[i];
    isize r = write_all(kmsg_fd, record, used + 3);
    if (r < 0) {
      kmsg_fd = -1;
      say(diag_fd, "init: /dev/kmsg mirror write failed errno=");
      char digits[24]; usize n = 0; usize value = errno_value(r);
      do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
      while (n) write_all(diag_fd, &digits[--n], 1);
      say(diag_fd, "\r\n");
    }
  }

  if (used && line[used - 1] == '\n') line[used - 1] = '\r';
  line[used++] = '\n';
  write_all(diag_fd, line, used);
}

__attribute__((noreturn)) static void stop(void) { for (;;) {} }

__attribute__((noreturn)) static void fail(const char *stage, isize result) {
  report(stage, result);
  stop();
}

static isize open_path(const char *path, int flags) {
  return syscall4(56, (usize)AT_FDCWD, (usize)path, (usize)flags, 0);
}

static isize mkdir_path(const char *path) {
  return syscall3(34, (usize)AT_FDCWD, (usize)path, MODE);
}

static void ensure_dir(const char *path) {
  isize r = mkdir_path(path);
  if (r == -17) r = 0;
  if (r < 0) fail(path, r);
}

static isize check_directory(const char *path) {
  /* O_PATH checks lookup/type without invoking configfs_dir_open(). */
  isize fd = open_path(path, O_PATH | O_DIRECTORY);
  if (fd < 0) return fd;
  syscall3(57, (usize)fd, 0, 0);
  return 0;
}

static isize mount_fs(const char *source, const char *target, const char *type) {
  return syscall5(40, (usize)source, (usize)target, (usize)type, 0, 0);
}

static isize write_path(const char *path, const char *value) {
  isize fd = open_path(path, O_WRONLY);
  if (fd < 0) return fd;
  isize result = write_all((int)fd, value, slen(value));
  syscall3(57, (usize)fd, 0, 0);
  return result < 0 ? result : 0;
}

static int set_path(const char *path, const char *value) {
  isize r = write_path(path, value);
  if (r < 0) report(path, r);
  return r == 0;
}

static int link_function(void) {
  const char *target = "/sys/kernel/config/usb_gadget/a1599/functions/acm.usb0";
  const char *link = "/sys/kernel/config/usb_gadget/a1599/configs/c.1/acm.usb0";
  isize r = syscall3(36, (usize)target, (usize)AT_FDCWD, (usize)link);
  report("link ACM function", r);
  if (r < 0) return 0;

  r = check_directory(link);
  report("verify ACM link before UDC bind", r);
  return r == 0;
}

static int bind_first_udc(void) {
  const char *udc_dir = "/sys/class/udc";
  const char *udc_path = "/sys/kernel/config/usb_gadget/a1599/UDC";
  /* getdents64 below also verifies this is a directory. */
  isize fd = open_path(udc_dir, O_RDONLY);
  if (fd < 0) fail("open /sys/class/udc", fd);

  char entries[512];
  isize count = syscall3(61, (usize)fd, (usize)entries, sizeof(entries));
  syscall3(57, (usize)fd, 0, 0);
  if (count < 0) fail("read /sys/class/udc", count);
  if ((usize)count > sizeof(entries)) fail("read /sys/class/udc (oversized result)", -22);
  if (count == 0) fail("enumerate /sys/class/udc (no UDC registered)", -19);

  usize pos = 0;
  isize last_error = -19;
  while (pos <= (usize)count && (usize)count - pos >= 20) {
    unsigned short reclen = (unsigned char)entries[pos + 16];
    reclen |= (unsigned short)(unsigned char)entries[pos + 17] << 8;
    if (reclen < 20 || (usize)reclen > (usize)count - pos)
      fail("parse /sys/class/udc entry", -22);

    char *name = entries + pos + 19;
    usize max_name = reclen - 19;
    usize name_len = 0;
    while (name_len < max_name && name[name_len]) name_len++;
    if (name_len == max_name) fail("parse UDC name", -22);

    if (name_len && name[0] != '.') {
      char udc_name[96];
      if (name_len >= sizeof(udc_name)) fail("UDC name too long", -36);
      for (usize i = 0; i <= name_len; i++) udc_name[i] = name[i];
      say(diag_fd, "init: try UDC ");
      say(diag_fd, udc_name);
      say(diag_fd, "\r\n");
      isize r = write_path(udc_path, udc_name);
      report("bind ACM gadget to UDC", r);
      if (r == 0) return 1;
      last_error = r;
    }
    pos += reclen;
  }
  if (pos != (usize)count) fail("parse UDC directory", -22);
  fail("bind ACM gadget (all UDCs failed)", last_error);
}

static void cat_small(int fd, const char *path) {
  isize in = open_path(path, O_RDONLY);
  if (in < 0) { report(path, in); return; }
  char buf[256];
  for (;;) {
    isize n = syscall3(63, (usize)in, (usize)buf, sizeof(buf));
    if (n < 0) { report(path, n); break; }
    if (n == 0) break;
    write_all(fd, buf, (usize)n);
  }
  syscall3(57, (usize)in, 0, 0);
}

static void console_loop(int fd) {
  char line[96];
  for (;;) {
    say(fd, "a1599> ");
    usize n = 0;
    while (n + 1 < sizeof(line)) {
      char ch;
      isize r = syscall3(63, (usize)fd, (usize)&ch, 1);
      if (r <= 0) continue;
      if (ch == '\r' || ch == '\n') { say(fd, "\r\n"); break; }
      if (ch == 8 || ch == 127) { if (n) n--; continue; }
      if (ch >= 32 && ch <= 126) { line[n++] = ch; syscall3(64, (usize)fd, (usize)&ch, 1); }
    }
    line[n] = 0;
    if (eq(line, "help")) say(fd, "help, version, cmdline, meminfo, mounts, clear\r\n");
    else if (eq(line, "version")) cat_small(fd, "/proc/version");
    else if (eq(line, "cmdline")) cat_small(fd, "/proc/cmdline");
    else if (eq(line, "meminfo")) cat_small(fd, "/proc/meminfo");
    else if (eq(line, "mounts")) cat_small(fd, "/proc/mounts");
    else if (eq(line, "clear")) say(fd, "\033[2J\033[H");
    else if (n) say(fd, "unknown command\r\n");
  }
}

__attribute__((noreturn)) void init(void) {
  isize cfd = open_path("/dev/console", O_WRONLY);
  if (cfd < 0) {
    say(1, "init: open /dev/console failed errno=");
    char digits[24]; usize n = 0; usize value = errno_value(cfd);
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) write_all(1, &digits[--n], 1);
    say(1, "\r\n");
    stop();
  }
  diag_fd = (int)cfd;
  report("open /dev/console", 0);

  const char stdout_probe[] = "init: stdout fd 1 write probe\r\n";
  isize stdout_result = write_all(1, stdout_probe, sizeof(stdout_probe) - 1);
  report("write stdout fd 1", stdout_result < 0 ? stdout_result : 0);

  isize r;
  const char *mountpoints[] = { "/dev", "/proc", "/sys", "/run" };
  for (usize i = 0; i < sizeof(mountpoints) / sizeof(mountpoints[0]); i++)
    ensure_dir(mountpoints[i]);

  r = mount_fs("devtmpfs", "/dev", "devtmpfs");
  if (r < 0) fail("mount devtmpfs", r);
  kmsg_fd = (int)open_path("/dev/kmsg", O_WRONLY);
  if (kmsg_fd < 0) report("open /dev/kmsg for printk mirror", kmsg_fd);
  report("mount devtmpfs on /dev", 0);
  r = mount_fs("proc", "/proc", "proc");
  if (r < 0) fail("mount proc", r);
  report("mount proc on /proc", 0);
  say(diag_fd, "init: active kernel consoles from /proc/consoles:\r\n");
  cat_small(diag_fd, "/proc/consoles");

  r = mount_fs("sysfs", "/sys", "sysfs");
  if (r < 0) fail("mount sysfs", r);
  report("mount sysfs on /sys", 0);
  ensure_dir("/sys/kernel");
  ensure_dir("/sys/kernel/config");
  r = mount_fs("configfs", "/sys/kernel/config", "configfs");
  if (r < 0) fail("mount configfs", r);
  report("mount configfs", 0);

  const char *dirs[] = {
    "/sys/kernel/config/usb_gadget/a1599",
    "/sys/kernel/config/usb_gadget/a1599/strings",
    "/sys/kernel/config/usb_gadget/a1599/strings/0x409",
    "/sys/kernel/config/usb_gadget/a1599/configs",
    "/sys/kernel/config/usb_gadget/a1599/configs/c.1",
    "/sys/kernel/config/usb_gadget/a1599/configs/c.1/strings",
    "/sys/kernel/config/usb_gadget/a1599/configs/c.1/strings/0x409",
    "/sys/kernel/config/usb_gadget/a1599/functions",
    "/sys/kernel/config/usb_gadget/a1599/functions/acm.usb0"
  };
  for (usize i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
    ensure_dir(dirs[i]);
  r = check_directory("/sys/kernel/config/usb_gadget/a1599/functions/acm.usb0");
  if (r < 0) fail("verify ACM function source", r);
  report("ACM configfs tree and function source ready", 0);

  if (!set_path("/sys/kernel/config/usb_gadget/a1599/idVendor", "0x1d6b")) stop();
  if (!set_path("/sys/kernel/config/usb_gadget/a1599/idProduct", "0x0104")) stop();
  if (!set_path("/sys/kernel/config/usb_gadget/a1599/bcdUSB", "0x0200")) stop();
  if (!set_path("/sys/kernel/config/usb_gadget/a1599/bcdDevice", "0x0100")) stop();
  if (!set_path("/sys/kernel/config/usb_gadget/a1599/strings/0x409/manufacturer", "Hoolock lab")) stop();
  if (!set_path("/sys/kernel/config/usb_gadget/a1599/strings/0x409/product", "A1599 USB serial")) stop();
  if (!set_path("/sys/kernel/config/usb_gadget/a1599/strings/0x409/serialnumber", "a1599-debug")) stop();
  if (!set_path("/sys/kernel/config/usb_gadget/a1599/configs/c.1/MaxPower", "100")) stop();
  report("write ACM gadget descriptors", 0);
  if (!link_function()) stop();
  bind_first_udc();

  struct { long sec; long nsec; } pause = {0, 500000000};
  syscall3(101, (usize)&pause, 0, 0);
  isize fd = open_path("/dev/ttyGS0", O_RDWR);
  report("open /dev/ttyGS0", fd < 0 ? fd : 0);
  if (fd < 0) stop();
  console_loop((int)fd);
  stop();
}

__attribute__((naked, noreturn)) void _start(void) {
  asm volatile("bl init\n"
               "1: b 1b\n");
}
