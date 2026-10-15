// 运行时生成的 A1599 EDID 必须与实测过的 edid.inc 逐字节一致
#define main gudd_main
#include "../gudd.c"
#undef main
int main(void)
{
	static const uint8_t ref[128] = {
#include "../edid.inc"
	};
	fb_w = 1536; fb_h = 2048;
	edid_build("iPad mini 3", "A1599", 120, 160, 0x1599);
	for (int i = 0; i < 128; i++) if (edid[i] != ref[i]) { printf("第 %d 字节不同: %02x vs %02x\n", i, edid[i], ref[i]); return 1; }
	fb_w = 1668; fb_h = 2224;
	edid_build("iPad Pro 10.5", "A1701", 160, 214, 0x1701);
	FILE *f = fopen("/tmp/edid-j207.bin", "wb"); fwrite(edid, 1, 128, f); fclose(f);
	printf("ok: A1599 EDID 一致；已写出 J207 EDID\n");
	return 0;
}
