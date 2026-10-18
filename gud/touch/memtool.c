/* memtool：/dev/mem 32 位读写。用法: memtool r ADDR [N] | memtool w ADDR VAL */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 3) return 2;
	uint64_t a = strtoull(argv[2], 0, 0), pg = a & ~0x3fffULL;
	int n = argc > 3 && argv[1][0] == 'r' ? atoi(argv[3]) : 1;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open"); return 1; }
	size_t len = ((a - pg) + n * 4 + 0x3fff) & ~0x3fffULL;
	volatile uint32_t *m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pg);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	volatile uint32_t *p = m + (a - pg) / 4;
	if (argv[1][0] == 'w') { *p = strtoul(argv[3], 0, 0); printf("%llx <- %08x, now %08x\n", (unsigned long long)a, (unsigned)strtoul(argv[3], 0, 0), *p); }
	else for (int i = 0; i < n; i++) printf("%llx: %08x\n", (unsigned long long)(a + 4 * i), p[i]);
	return 0;
}
