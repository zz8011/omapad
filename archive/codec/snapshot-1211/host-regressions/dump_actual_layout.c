/* Offline byte dump only. No device access. */
#include <stdio.h>
#include "../a1599_fw_decode_stream.h"
int main(void)
{
	struct a1599_decode_span init[] = {
		{0x40000000,0xe000,0x40000000}, {0x40010000,0x2000,0x40010000},
		{0x40100000,0x100000,0x40100000}, {0x40300000,0x40000,0x40300000},
		{0x40340000,0x1000,0x40340000},
	};
	struct a1599_decode_resources r = {
		.preload={0x40012000,0x1000,0x40012000},
		.vlc={0x40013000,0x1000,0x40013000},
		.bitstream={0x40014000,0x1000,0x40014004},
		.current_picture={0x40015000,0x4000,0x40015000},
		.intra={0x40019000,0x10000,0x40019000},
		.output={0x40029000,0x1000,0x40029000},
		.command={0x4002a000,0x1000,0x4002a000},
		.plane0_offset=0, .plane1_offset=2048,
		.existing_iova_maps=init, .existing_iova_map_count=5,
	};
	unsigned char bytes[A1599_DECODE_STREAM_BYTES];
	if (a1599_build_fixture_decode_stream(bytes,sizeof(bytes),&r)!=544) return 1;
	return fwrite(bytes,1,sizeof(bytes),stdout)!=sizeof(bytes);
}
