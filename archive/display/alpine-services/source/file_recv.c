#define _DEFAULT_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <unistd.h>

#define MAX_NAME 64u
#define MAX_FILE (32u * 1024u * 1024u)
#define CHUNK 16384u
#define IDLE_TIMEOUT_MS 30000

static volatile sig_atomic_t stop_requested;
static void on_signal(int sig) { (void)sig; stop_requested = 1; }

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (unsigned b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
  }
  return crc;
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

static int line_read(int fd, char *buf, size_t cap) {
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
      while (ch != '\n') {
        if (poll(&pfd, 1, 1000) <= 0 || read(fd, &ch, 1) != 1) break;
      }
      return -3;
    }
    buf[used++] = ch;
  }
}

static int safe_name(const char *s) {
  size_t n = strlen(s);
  if (!n || n > MAX_NAME || s[0] == '.' || strstr(s, "..")) return 0;
  for (size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+'))
      return 0;
  }
  return 1;
}

static int receive_file(int tty, int dirfd, const char *name,
                        size_t bytes, uint32_t wanted) {
  struct statvfs sv;
  if (fstatvfs(dirfd, &sv) != 0) return -1;
  uint64_t free_bytes = (uint64_t)sv.f_bavail * sv.f_frsize;
  if (bytes > free_bytes || free_bytes - bytes < (8u * 1024u * 1024u)) return -1;
  char temp_name[80];
  int out = -1;
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    snprintf(temp_name, sizeof(temp_name), ".part-%ld-%u", (long)getpid(), attempt);
    out = openat(dirfd, temp_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out >= 0 || errno != EEXIST) break;
  }
  if (out < 0) return -1;
  char reply[128];
  if (write_all(tty, "FILE-READY\n", 11)) { close(out); unlinkat(dirfd, temp_name, 0); return -1; }
  uint8_t buf[CHUNK];
  size_t done = 0;
  uint32_t crc = 0xffffffffu;
  while (done < bytes) {
    struct pollfd pfd = {tty, POLLIN, 0};
    int pr = poll(&pfd, 1, IDLE_TIMEOUT_MS);
    if (pr < 0 && errno == EINTR) { if (stop_requested) break; continue; }
    if (pr <= 0 || (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))) break;
    size_t want = bytes - done < sizeof(buf) ? bytes - done : sizeof(buf);
    ssize_t n = read(tty, buf, want);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || write_all(out, buf, (size_t)n)) break;
    crc = crc32_update(crc, buf, (size_t)n);
    done += (size_t)n;
  }
  crc = ~crc;
  int close_rc = close(out);
  if (done != bytes || close_rc || crc != wanted) {
    (void)unlinkat(dirfd, temp_name, 0);
    return -1;
  }
  if (linkat(dirfd, temp_name, dirfd, name, 0) != 0) {
    (void)unlinkat(dirfd, temp_name, 0);
    return -1;
  }
  (void)unlinkat(dirfd, temp_name, 0);
  snprintf(reply, sizeof(reply), "FILE-OK name=%s bytes=%zu crc=%08x\n", name, bytes, crc);
  return write_all(tty, reply, strlen(reply));
}

static void remove_stale_parts(int dirfd) {
  int copy = dup(dirfd);
  if (copy < 0) return;
  DIR *dir = fdopendir(copy);
  if (!dir) { close(copy); return; }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strncmp(entry->d_name, ".part-", 6))
      (void)unlinkat(dirfd, entry->d_name, 0);
  }
  closedir(dir);
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, on_signal);
  int tty = open(argv[1], O_RDWR | O_NOCTTY | O_CLOEXEC);
  int dirfd = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (tty < 0 || dirfd < 0) return 2;
  remove_stale_parts(dirfd);
  struct termios saved, raw;
  if (tcgetattr(tty, &saved)) return 2;
  raw = saved; cfmakeraw(&raw); raw.c_cflag |= CLOCAL | CREAD;
  raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
  if (tcsetattr(tty, TCSANOW, &raw)) return 2;
  (void)write_all(tty, "FILE-RECV 1 READY\n", 18);
  while (!stop_requested) {
    char line[128], name[MAX_NAME + 1], extra;
    int n = line_read(tty, line, sizeof(line));
    if (n == -2) break;
    if (n < 0) { if (n == -3) (void)write_all(tty, "FILE-ERR line-too-long\n", 23); else break; continue; }
    if (!strcmp(line, "HELLO")) { (void)write_all(tty, "FILE-RECV 1 READY\n", 18); continue; }
    unsigned long bytes; unsigned crc;
    if (sscanf(line, "PUT %64s %lu %x%c", name, &bytes, &crc, &extra) != 3 ||
        !safe_name(name) || bytes == 0 || bytes > MAX_FILE) {
      (void)write_all(tty, "FILE-ERR request\n", 17); continue;
    }
    if (receive_file(tty, dirfd, name, (size_t)bytes, (uint32_t)crc) != 0 && !stop_requested)
      (void)write_all(tty, "FILE-ERR transfer\n", 18);
  }
  (void)tcsetattr(tty, TCSANOW, &saved);
  close(dirfd); close(tty);
  return 0;
}
