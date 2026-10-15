/* Direct baseline characterization: the real 1211 receive helper cannot
 * meet the new consumer advancement contract. This test must fail. */
#include <assert.h>
#include <stdint.h>
#include "../../a1599_first_decode_candidate_c0_classification_20261014/a1599_fw_ring_capacity.h"
static unsigned int read_idx;
static int rd(void*c,unsigned int off,unsigned int*v){
 (void)c;switch(off){case 0x2fec:*v=3;break;case 0x2fe8:*v=read_idx;break;
 case 0x8000:*v=0x0001c08c;break;case 0x8004:*v=3;break;case 0x8008:*v=0x600644d5;break;
 default:assert(0);}return 0;
}
static void bar(void*c){(void)c;}
static uint64_t tm(void*c){(void)c;return 0;}
static void sl(void*c){(void)c;assert(0);}
int main(void){
 struct a1599_init_mmio_ops ops={.read32=rd,.read_barrier=bar,.time_ns=tm,.sleep_1_1_5_ms=sl};
 struct a1599_ring_message msg;
 assert(a1599_m2h_mmio_wait_message(&ops,0,0x8000,51,0,2,&msg)==0);
 assert(msg.length==12 && msg.bytes[1]==0xc0);
 assert(read_idx==3 && "new consumer advancement contract absent in unchanged 1211");
}
