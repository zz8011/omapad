#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../a1599_fw_decode_stream.h"

static uint32_t word(const unsigned char *p, unsigned int i)
{
	return (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
		((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
}

int main(int argc, char **argv)
{
	struct a1599_decode_resources r = {
		.preload = {0x50000000, 0x1000, 0x50000000},
		.vlc = {0x51000000, 0x1000, 0x51000000},
		.bitstream = {0x52000000, 0x1000, 0x52000028},
		.current_picture = {0x53000000, 0x4000, 0x53000000},
		.intra = {0x54000000, 0x10000, 0x54000000},
		.output = {0x55000000, 0x1000, 0x55000000},
		.command = {0x56000000, 0x1000, 0x56000000},
		.plane0_offset = 0, .plane1_offset = 2048,
	};
	struct a1599_decode_span existing[] = {
		{0x40000000, 0x0e000, 0x40000000},
		{0x40010000, 0x02000, 0x40010000},
		{0x40100000, 0x100000, 0x40100000},
		{0x40300000, 0x040000, 0x40300000},
		{0x40340000, 0x001000, 0x40340000},
	};
	unsigned char bytes[A1599_DECODE_STREAM_BYTES];

	memset(bytes, 0xa5, sizeof(bytes));
	r.existing_iova_maps = existing;
	r.existing_iova_map_count = sizeof(existing) / sizeof(existing[0]);
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) == 544);
	assert(word(bytes, 0) == 0x90000000);
	assert(word(bytes, 2) == 0x50000000);
	assert(word(bytes, 9) == 0xe0000440);
	assert(word(bytes, 11) == 0x101708ec);
	assert(word(bytes, 35) == 0x100309c0);
	assert(word(bytes, 39) == 0xb0000001);
	assert(word(bytes, 44) == 0x1001080c);
	assert(word(bytes, 46) == 0x10050a00);
	assert(word(bytes, 54) == 0x50010810);
	assert(word(bytes, 56) == 0x50070ad8);
	assert(word(bytes, 62) == 0x53000000);
	assert(word(bytes, 121) == 0x50051000);
	assert(word(bytes, 125) == 0x55000000);
	assert(word(bytes, 126) == 0x55000800);
	assert(word(bytes, 127) == 0x50011018);
	assert(word(bytes, 128) == 0x54000000);
	assert(word(bytes, 135) == 0x60000000);
	r.existing_iova_map_count--;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.existing_iova_map_count++;
	r.existing_iova_map_count = 0;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.existing_iova_map_count = sizeof(existing) / sizeof(existing[0]);
	existing[4] = existing[0];
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	existing[4] = (struct a1599_decode_span) {0x40340000, 0x001000, 0x40340000};
	r.bitstream.base_iova = 0x40000000;
	r.bitstream.codec_iova = 0x40000028;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.bitstream.base_iova = 0x52000000;
	r.bitstream.codec_iova = 0x52000028;

	r.current_picture.bytes = 0x3000;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.current_picture.bytes = 0x4000;
	r.intra.base_iova++;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.intra.base_iova = 0x54000000;
	r.bitstream.codec_iova = 0x52000ff8;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.bitstream.codec_iova = 0x52000028;
	r.plane1_offset = 0;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.plane1_offset = 2048;
	r.vlc.base_iova = r.preload.base_iova;
	r.vlc.codec_iova = r.vlc.base_iova;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.vlc.base_iova = 0x51000000;
	r.vlc.codec_iova = r.vlc.base_iova;
	r.command.base_iova = r.output.base_iova;
	r.command.codec_iova = r.command.base_iova;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.command.base_iova = 0x56000000;
	r.command.codec_iova = r.command.base_iova;
	r.command.codec_iova += 0x0ff0;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	r.command.codec_iova = r.command.base_iova;
	existing[0].base_iova = r.current_picture.base_iova;
	existing[0].codec_iova = existing[0].base_iova;
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	existing[0] = existing[1];
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) < 0);
	existing[0] = (struct a1599_decode_span) {0x40000000, 0x0000e000, 0x40000000};
	existing[1] = (struct a1599_decode_span) {0x40010000, 0x00002000, 0x40010000};
	existing[2] = (struct a1599_decode_span) {0x40100000, 0x00100000, 0x40100000};
	existing[3] = (struct a1599_decode_span) {0x40300000, 0x00040000, 0x40300000};
	existing[4] = (struct a1599_decode_span) {0x40340000, 0x00001000, 0x40340000};
	assert(a1599_build_fixture_decode_stream(bytes, sizeof(bytes), &r) == 544);
	assert(a1599_build_fixture_decode_stream(bytes, 543, &r) < 0);
	if (argc == 2) {
		FILE *file = fopen(argv[1], "wb");
		assert(file != NULL);
		assert(fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
		assert(fclose(file) == 0);
	}
	puts("decode command serializer host tests: PASS");
	return 0;
}
