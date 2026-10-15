#!/usr/bin/env python3
"""Compile the exact M2H sysfs callback body extracted from production source."""
from pathlib import Path
import subprocess
import tempfile

candidate_root = Path('/home/zz8011/Documents/Codex/2026-10-13/task-6/a1599_first_decode_candidate_m2h_snapshot_20261014') if Path(__file__).resolve().parent == Path('/tmp') else Path(__file__).resolve().parents[1]
source_path = candidate_root / 'a1599_fw_first_ready_platform_harness.c'
src = source_path.read_text()
marker = 'static ssize_t m2h_ring_raw_once_read('
start = src.index(marker)
brace = src.index('{', start)
depth = 0
end = None
for i in range(brace, len(src)):
    if src[i] == '{': depth += 1
    elif src[i] == '}':
        depth -= 1
        if depth == 0:
            end = i + 1
            break
assert end is not None
callback = src[start:end]

prefix = r'''#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef int64_t loff_t;
typedef uint8_t u8;
typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define dma_rmb() ((void)0)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
struct file { int unused; };
struct bin_attribute { int unused; };
struct device { int unused; };
struct kobject { int unused; };
static struct device the_device;
static struct device *kobj_to_dev(struct kobject *k) { (void)k; return &the_device; }
struct a1599_fw_reg_window { void *base; size_t bytes; };
struct a1599_fw_ring_snapshot {
 u32 h2m_descriptor, m2h_descriptor;
 u32 h2m_read_index, h2m_write_index, m2h_read_index, m2h_write_index;
 u32 h2m_start, h2m_end, m2h_start, m2h_end;
};
static int a1599_first_ready_live = 1;
static struct device *a1599_live_device = &the_device;
static int a1599_init_transport_consumed = 1;
static int a1599_decode_consumed = 1;
static int a1599_m2h_raw_consumed;
static struct a1599_fw_reg_window a1599_live_window;
static int mutex_held, snapshot_calls, snapshot_error_call, read_calls;
static int fail_after_snapshot;
static uintptr_t read_addresses[8];
static u32 read_values[8];
static struct a1599_fw_ring_snapshot snapshots[2];
static int atomic_cmpxchg(int *p, int old, int value) {
 int prior = *p; if (prior == old) *p = value; return prior;
}
static int atomic_read(int *p) { return *p; }
static void mutex_lock(int *p) { assert(!mutex_held); mutex_held = 1; (void)p; }
static void mutex_unlock(int *p) { assert(mutex_held); mutex_held = 0; (void)p; }
static int a1599_ring_lock;
static u32 readl(const void *address) {
 uintptr_t a = (uintptr_t)address;
 assert(mutex_held);
 assert(read_calls < 8);
 read_addresses[read_calls] = a;
 return read_values[read_calls++];
}
static void put_unaligned_le32(u32 value, u8 *p) {
 p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}
static int a1599_ring_snapshot_locked(struct a1599_fw_reg_window *w,
                                     struct a1599_fw_ring_snapshot *s) {
 int idx = snapshot_calls++;
 assert(mutex_held);
 assert(w == &a1599_live_window);
 if (snapshot_error_call == idx + 1) return -EIO;
 *s = snapshots[idx > 0 ? 1 : 0];
 if (idx == 1 && fail_after_snapshot) return -EIO;
 return 0;
}
static void reset_case(void) {
 memset(snapshots, 0, sizeof(snapshots));
 snapshots[0] = (struct a1599_fw_ring_snapshot){
  .h2m_descriptor=0x60cc01cc, .m2h_descriptor=0x60000033,
  .h2m_read_index=10, .h2m_write_index=10,
  .m2h_read_index=0, .m2h_write_index=8,
  .m2h_start=0x8000, .m2h_end=0x80cc,
 };
 snapshots[1] = snapshots[0];
 a1599_m2h_raw_consumed=0; snapshot_calls=0; snapshot_error_call=0;
 read_calls=0; mutex_held=0; fail_after_snapshot=0;
 memset(read_addresses,0,sizeof(read_addresses));
 for (int i=0;i<8;i++) read_values[i]=0x11223300U+i;
}

'''

suite = r'''
static ssize_t invoke(loff_t off, size_t count, unsigned char *out) {
 return m2h_ring_raw_once_read(NULL, NULL, NULL, (char *)out, off, count);
}
int main(void) {
 unsigned char out[32];
 /* Bad offset/length rejected before one-shot gate or any MMIO. */
 reset_case(); assert(invoke(1,32,out)==-EINVAL); assert(!a1599_m2h_raw_consumed && read_calls==0);
 assert(invoke(0,31,out)==-EINVAL); assert(!a1599_m2h_raw_consumed && read_calls==0);
 assert(invoke(0,33,out)==-EINVAL); assert(!a1599_m2h_raw_consumed && read_calls==0);
 /* Not ready is rejected before consumption. */
 reset_case(); a1599_decode_consumed=0; assert(invoke(0,32,out)==-EAGAIN); assert(!a1599_m2h_raw_consumed && read_calls==0); a1599_decode_consumed=1;
 /* Descriptor and index mismatches fail closed before payload reads, no retry. */
 reset_case(); snapshots[0].m2h_descriptor ^= 4; assert(invoke(0,32,out)==-ESTALE); assert(a1599_m2h_raw_consumed && read_calls==0); assert(invoke(0,32,out)==-EALREADY && read_calls==0);
 reset_case(); snapshots[0].m2h_write_index=7; assert(invoke(0,32,out)==-ESTALE); assert(read_calls==0 && a1599_m2h_raw_consumed);
 reset_case(); snapshots[0].h2m_read_index=9; assert(invoke(0,32,out)==-ESTALE); assert(read_calls==0 && a1599_m2h_raw_consumed);
 /* Successful path reads exactly 8 bounded words, returns LE bytes, and has no consumer-write operation. */
 reset_case(); assert(invoke(0,32,out)==32); assert(read_calls==8); assert(snapshot_calls==2); assert(!mutex_held);
 for (int i=0;i<8;i++) {
  assert(read_addresses[i] == 0x8000U + 4U*(uintptr_t)i);
  assert(out[i*4] == (u8)(0x11223300U+i));
  assert(out[i*4+1] == (u8)((0x11223300U+i)>>8));
  assert(out[i*4+2] == (u8)((0x11223300U+i)>>16));
  assert(out[i*4+3] == (u8)((0x11223300U+i)>>24));
 }
 assert(invoke(0,32,out)==-EALREADY && read_calls==8);
 /* Producer/consumer/index movement after payload suppresses bytes and forbids retry. */
 reset_case(); memset(out,0xA5,sizeof(out)); snapshots[1].m2h_write_index=9; assert(invoke(0,32,out)==-ESTALE); assert(out[0]==0xA5 && out[31]==0xA5); assert(read_calls==8); assert(invoke(0,32,out)==-EALREADY && read_calls==8);
 reset_case(); snapshots[1].m2h_read_index=1; assert(invoke(0,32,out)==-ESTALE && read_calls==8);
 reset_case(); snapshots[1].h2m_read_index=9; assert(invoke(0,32,out)==-ESTALE && read_calls==8);
 reset_case(); snapshots[1].m2h_descriptor ^= 4; assert(invoke(0,32,out)==-ESTALE && read_calls==8);
 reset_case(); snapshots[1].m2h_end=0x80c8; assert(invoke(0,32,out)==-ESTALE && read_calls==8);
 /* Snapshot API error before/after read fails closed and remains one-shot. */
 reset_case(); snapshot_error_call=1; assert(invoke(0,32,out)==-EIO); assert(read_calls==0 && a1599_m2h_raw_consumed);
 reset_case(); snapshot_error_call=2; assert(invoke(0,32,out)==-EIO); assert(read_calls==8 && a1599_m2h_raw_consumed); assert(invoke(0,32,out)==-EALREADY && read_calls==8);
 puts("source-driven M2H callback host tests: PASS");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='a1599-m2h-source-test-') as td:
    c = Path(td) / 'test.c'
    exe = Path(td) / 'test'
    c.write_text(prefix + '\n' + callback + '\n' + suite)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-o',str(exe),str(c)],check=True)
    subprocess.run([str(exe)],check=True)
print(f'compiled exact callback bytes={len(callback)} source={source_path}:{start}:{end}')
