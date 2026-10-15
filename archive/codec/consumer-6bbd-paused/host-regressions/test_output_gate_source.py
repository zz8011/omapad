#!/usr/bin/env python3
"""Execute the exact production post-response block and allocation checks.
Real snapshot logic; only ring reads, delays and publication are host doubles.
"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'a1599_fw_first_ready_platform_harness.c').read_text()
a=src.index('\t/* CPU-only snapshot publication is separate')
b=src.index('\tdev_warn(dev, "DECODE_ONE_SHOT',a)
block=src[a:b]
a=src.index('static bool a1599_output_allocation_matches(')
b=src.index('\nstatic int a1599_output_read_ring',a)
allocation_check=src[a:b]
a=src.index('static int a1599_output_read_ring(')
b=src.index('\nstatic void a1599_output_wait_ms',a)
ring_adapter=src[a:b]
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "a1599_fw_decode_submit.h"
#include "a1599_fw_decode_diagnostics.h"
static unsigned int memory_reads;
static void *tracked_copy(void *d,const void *s,size_t n) {
 memory_reads++; assert(n==4096); return memcpy(d,s,n);
}
#define memcpy tracked_copy
#include "a1599_fw_output_snapshot.h"
#undef memcpy
#define A1599_DECODE_OUTPUT 0
#define A1599_FW_DECODE_OUTPUT_BYTES 4096
#define WRITE_ONCE(x,v) ((x)=(v))
#define smp_store_release(p,v) (assert(a1599_decode_result_valid && a1599_decode_output_inspection_allowed), (*(p)=(v)))
struct device { int x; } device;
struct a1599_fw_static_allocation { struct device *dev; void *alloc_cpu; unsigned long long alloc_dma; size_t alloc_bytes; void *cpu; unsigned long long dma; size_t bytes; };
struct a1599_decode_dma_view { void *cpu; unsigned long long dma; size_t bytes; };
static struct { bool allocated,hardware_started; struct a1599_fw_static_allocation decode[1]; struct { bool started; struct a1599_decode_dma_view buffers[1]; } decode_lifetime; } memory, *a1599_memory=&memory;
static unsigned char backing[4096], a1599_decode_output_snapshot[4096], a1599_decode_output_second_sample[4096];
static bool a1599_decode_output_snapshot_ready, allow_output_export;
static bool a1599_decode_result_valid, a1599_decode_output_inspection_allowed;
static int a1599_live_window;
static unsigned int sleeps, barriers, rings;
#define a1599_fw_ring_snapshot a1599_decode_ring_snapshot
static int a1599_output_read_ring(void *, struct a1599_decode_ring_snapshot *);
static int a1599_ring_snapshot(void *c,struct a1599_decode_ring_snapshot *s) {
 assert(c==&a1599_live_window); rings++;
 *s=(struct a1599_decode_ring_snapshot){
  .h2m_descriptor=0x60cc01cc,.m2h_descriptor=0x60000033,
  .h2m_capacity_words=460,.m2h_capacity_words=51,
  .h2m_start=0x80cc,.m2h_start=0x8000,
  .h2m_read_index=10,.h2m_write_index=10,.m2h_read_index=0,.m2h_write_index=3,
 }; return 0;
}
static void barrier(void *c) { (void)c; barriers++; }
static void wait_ms(void *c,unsigned int ms) { (void)c; assert(ms==50); sleeps++; }
static const struct a1599_output_snapshot_ops a1599_output_snapshot_ops={a1599_output_read_ring,barrier,wait_ms};
static void a1599_log_c0_capture(struct device *d,const struct a1599_decode_submit_result *r,const struct a1599_decode_diagnostic *g,bool checked) { (void)d;(void)r;(void)g;(void)checked; }
'''
runner=r'''
static void run_case(unsigned int flags,bool valid,bool export,bool allocation_valid,bool pinned,int submit_ret,int ring_ret) {
 struct a1599_decode_submit_result result={0};
 struct a1599_decode_diagnostic diagnostic;
 struct a1599_output_snapshot_result output_result={0};
 struct device *dev=&device;
 int ret=submit_ret,after_ret=ring_ret,output_ret=-EACCES;
 result.published=result.consumed=result.response_received=true;
 result.response_record_valid=valid;
 result.response.status=flags; result.response.fence=1;
 result.response.dispatch_class=(flags & (1U<<27)) ? C0_DISPATCH_8010 : C0_DISPATCH_10;
 result.response_error=!!(flags & (1U<<27));
 result.status_class=(flags & (1U<<27)) ? A1599_DECODE_C0_CLASS_8010 : A1599_DECODE_C0_CLASS_10;
 result.message.length=12;result.message.write_index=3;
 result.message.bytes[0]=0x8c;result.message.bytes[1]=0xc0;result.message.bytes[2]=1;
 for(unsigned int i=0;i<4;i++)result.message.bytes[8+i]=flags>>(i*8);
 allow_output_export=export;
 memory.allocated=true;memory.hardware_started=true;memory.decode_lifetime.started=pinned;
 memory.decode[0]=(struct a1599_fw_static_allocation){.dev=dev,.alloc_cpu=backing,.alloc_dma=0x8038c0000ULL,.alloc_bytes=4096,.cpu=backing,.dma=0x8038c0000ULL,.bytes=4096};
 memory.decode_lifetime.buffers[0]=(struct a1599_decode_dma_view){backing,0x8038c0000ULL,allocation_valid?4096:4095};
 memset(backing,0xa5,sizeof backing);for(unsigned int i=0;i<3072;i++)backing[i]=(unsigned char)(i*7);
 memory_reads=sleeps=barriers=rings=0;
 a1599_decode_output_snapshot_ready=true; /* invalidate stale publication */
 a1599_decode_result_valid=a1599_decode_output_inspection_allowed=false;
'''
suite=r'''
 bool want=valid && export && !(flags & (1U<<27)) && allocation_valid && pinned && !submit_ret && !ring_ret;
 assert(a1599_decode_output_snapshot_ready==want);
 assert(output_result.snapshot_ready==want);
 if(want) {
  assert(output_ret==0 && memory_reads==2 && sleeps==1 && barriers==2 && rings==3);
  assert(diagnostic.guard_state==A1599_GUARD_INTACT);
  assert(!memcmp(backing,a1599_decode_output_snapshot,4096));
 } else {
  assert(memory_reads==0 && sleeps==0 && barriers==0 && rings==0);
  assert(diagnostic.guard_state==A1599_GUARD_NOT_CHECKED);
 }
}
int main(void) {
 unsigned int statuses[]={0,0x600644d5,0x48064455};
 for(unsigned int i=0;i<3;i++)for(int valid=0;valid<2;valid++)for(int export=0;export<2;export++)run_case(statuses[i],valid,export,true,true,0,0);
 run_case(0x600644d5,true,true,false,true,0,0);
 run_case(0x600644d5,true,true,true,false,0,0);
 run_case(0x600644d5,true,true,true,true,-EIO,0);
 run_case(0x600644d5,true,true,true,true,0,-ESTALE);
 puts("production output integration: PASS (16 cases; independent coherent/lifetime/response/authorization gates, two bounded copies, release publication)");
}
'''
with tempfile.TemporaryDirectory(prefix='a1599-output-gate-') as td:
 p=Path(td)/'gate.c';e=Path(td)/'gate'
 p.write_text(prefix+allocation_check+ring_adapter+runner+block+suite)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I',str(root),str(p),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
