// 离线测试：RGB565 转换与旧公式逐值一致、LZ4 往返、2 倍放大
#define main gudd_main
#include "../gudd.c"
#undef main
static uint32_t ref565(uint16_t c)
{
	uint32_t r5 = c >> 11, g6 = (c >> 5) & 63, b5 = c & 31;
	return (r5 << 3 | r5 >> 2) << 16 | (g6 << 2 | g6 >> 4) << 8 | (b5 << 3 | b5 >> 2);
}
int main(void)
{
	static uint16_t src[65536]; static uint32_t dst[65536 * 2];
	for (int i = 0; i < 65536; i++) src[i] = i;
	row565(dst, src, 65536);
	for (int i = 0; i < 65536; i++) if (dst[i] != ref565(i)) { printf("565 不一致 %d\n", i); return 1; }
	row565x2(dst, src, 65536);
	for (int i = 0; i < 65536; i++) if (dst[2*i] != ref565(i) || dst[2*i+1] != ref565(i)) { printf("x2 不一致 %d\n", i); return 1; }
	/* LZ4 往返：渐变式数据 */
	static char in[BULK_MAX], cmp[BULK_MAX + 4096], out[BULK_MAX];
	for (int i = 0; i < BULK_MAX; i++) in[i] = (i / 7) ^ (i >> 11);
	int c = LZ4_compress_default(in, cmp, BULK_MAX, sizeof(cmp));
	int d = LZ4_decompress_safe(cmp, out, c, BULK_MAX);
	if (d != BULK_MAX || memcmp(in, out, BULK_MAX)) { printf("LZ4 往返失败\n"); return 1; }
	/* 2 倍放大 blit */
	fb_w = 64; fb_h = 64; fb_stride = 64 * 4; fb = calloc(1, 64 * 64 * 4);
	struct gud_set_buffer_req r = { .x = 3, .y = 5, .width = 10, .height = 7 };
	uint16_t s2[70]; for (int i = 0; i < 70; i++) s2[i] = i * 937;
	blit(&r, (uint8_t *)s2, GUD_PIXEL_FORMAT_RGB565, 2);
	uint32_t *f = (uint32_t *)fb;
	for (int y = 0; y < 14; y++) for (int x = 0; x < 20; x++)
		if (f[(10 + y) * 64 + 6 + x] != ref565(s2[(y / 2) * 10 + x / 2])) { printf("放大不一致 %d,%d\n", x, y); return 1; }
	if (f[9 * 64 + 6] || f[24 * 64 + 6] || f[10 * 64 + 5] || f[10 * 64 + 26]) { printf("越界写\n"); return 1; }
	printf("ok: 565 全值一致, x2 一致, LZ4 往返 %d->%d, 放大 blit 一致且无越界\n", BULK_MAX, c);
	return 0;
}
