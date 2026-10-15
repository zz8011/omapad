/* Offline real acknowledgement helper with a strict MMIO boundary double.
 * Breaks caught: early/duplicate/extra writes, stale or truncated record,
 * missing gates, async producer changes, uncertain post-write readback. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../a1599_fw_decode_submit.h"
#include "../a1599_fw_c0_ack.h"
struct ack_fake {
 unsigned int regs[6], payload[3], reads, writes, barriers, readbars;
 unsigned int fail_at, change_at; bool bad_readback, late_message;
};
static int ack_read(void *c,unsigned int off,unsigned int *v) {
 struct ack_fake *f=c; unsigned int offsets[]={0x2ff0,0x2fe4,0x2ff4,0x2ffc,0x2fe8,0x2fec};
 f->reads++;if(f->reads==f->fail_at)return -EIO;
 if(f->reads==f->change_at)f->regs[5]=8;
 for(unsigned int i=0;i<6;i++)if(off==offsets[i]){*v=f->regs[i];return 0;}
 assert(off>=0x8000 && off<=0x8008 && !(off&3));
 *v=f->payload[(off-0x8000)/4];return 0;
}
static void ack_write(void *c,unsigned int off,unsigned int value) {
 struct ack_fake *f=c;
 assert(off==0x2fe8 && value==3 && f->writes==0);
 assert(f->reads==15 && f->barriers==1 && f->readbars==1);
 f->writes++;if(!f->bad_readback)f->regs[4]=value;
 if(f->late_message)f->regs[5]=8;
}
static void ack_barrier(void*c){((struct ack_fake*)c)->barriers++;}
static void ack_rbarrier(void*c){((struct ack_fake*)c)->readbars++;}
static const struct a1599_init_mmio_ops ack_ops={.read32=ack_read,.write32=ack_write,.publish_barrier=ack_barrier,.read_barrier=ack_rbarrier};
static struct a1599_ring_message valid_message(void){
 struct a1599_ring_message m={.length=12,.read_index=0,.write_index=3,
  .bytes={0x8c,0xc0,1,0,3,0,0,0,0xd5,0x44,6,0x60}};return m;
}
static struct ack_fake valid_device(void){
 struct ack_fake f={.regs={0x60cc01cc,0x60000033,10,10,0,3},
  .payload={0x0001c08c,3,0x600644d5}};return f;
}
int main(void){
 struct ack_fake f=valid_device();struct a1599_ring_message m=valid_message();
 struct a1599_c0_ack_state s={0};
 assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)==0);
 assert(s.attempted && s.write_issued && s.verified && f.writes==1 && f.reads==21);
 assert(s.after[4]==3 && s.after[5]==3 && s.after_valid_count==6);
 assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)==-EALREADY && f.reads==21 && f.writes==1);
 for(int gate=0;gate<3;gate++){
  f=valid_device();s=(struct a1599_c0_ack_state){0};
  assert(a1599_c0_ack_once(&ack_ops,&f,&m,gate!=0,gate!=1,gate!=2,&s)<0);
  assert(!s.attempted && f.reads==0 && f.writes==0);
 }
 for(int bad=0;bad<7;bad++){
  f=valid_device();m=valid_message();s=(struct a1599_c0_ack_state){0};
  if(bad==0)m.length=8;
  if(bad==1)m.read_index=1;
  if(bad==2)m.write_index=8;
  if(bad==3)m.bytes[0]=16;
  if(bad==4)m.bytes[1]=0xc7;
  if(bad==5)m.bytes[2]=2;
  if(bad==6)m.bytes[11]|=8;
  assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)<0);
  assert(f.reads==0 && f.writes==0 && s.attempted);
 }
 m=valid_message();
 for(int bad=0;bad<6;bad++){
  f=valid_device();s=(struct a1599_c0_ack_state){0};f.regs[bad]^=1;
  assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)<0 && f.writes==0);
 }
 f=valid_device();s=(struct a1599_c0_ack_state){0};f.payload[1]^=1;
 assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)==-ESTALE && f.writes==0);
 for(unsigned int i=1;i<=21;i++){
  f=valid_device();s=(struct a1599_c0_ack_state){0};f.fail_at=i;
  assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)==-EIO);
  assert(s.attempted && !s.verified && s.write_issued==(i>15) && f.writes==(i>15));
  assert(s.after_valid_count==(i>15?i-16:0));
  unsigned int reads=f.reads;
  assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)==-EALREADY && f.reads==reads);
 }
 f=valid_device();s=(struct a1599_c0_ack_state){0};f.change_at=10;
 assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)==-ESTALE && !f.writes);
 f=valid_device();s=(struct a1599_c0_ack_state){0};f.late_message=true;
 assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)==-ESTALE && f.writes==1 && s.write_issued && !s.verified);
 assert(f.regs[4]==3 && f.regs[5]==8); /* late message not acknowledged */
 f=valid_device();s=(struct a1599_c0_ack_state){0};f.bad_readback=true;
 assert(a1599_c0_ack_once(&ack_ops,&f,&m,true,true,true,&s)==-ESTALE && f.writes==1 && !s.verified);
 puts("PASS: C0 consumer-only transaction, gates, 21 I/O fault sites, stale/late response, no retry");
}
