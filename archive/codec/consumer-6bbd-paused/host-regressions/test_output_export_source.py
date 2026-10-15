#!/usr/bin/env python3
"""Compile and execute the exact sysfs output callback with host-only state."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'a1599_fw_first_ready_platform_harness.c').read_text()
a=s.index('static ssize_t decode_output_read(')
b=s.index('\nstatic BIN_ATTR_RO(decode_output',a)
prefix=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
typedef int64_t loff_t;
#define READ_ONCE(x) (x)
#define smp_load_acquire(p) (*(p))
#define A1599_FW_DECODE_OUTPUT_BYTES 4096
#define A1599_DECODE_OUTPUT 0
struct file { int unused; }; struct bin_attribute { int unused; };
struct device { int unused; }; struct kobject { int unused; };
static struct device device;
static struct device *kobj_to_dev(struct kobject *k) { (void)k; return &device; }
struct a1599_fw_static_allocation { void *cpu; size_t bytes; };
static struct { bool hardware_started; struct a1599_fw_static_allocation decode[1]; } memory;
static __typeof__(memory) *a1599_memory=&memory;
static struct device *a1599_live_device=&device;
static bool allow_output_export, a1599_first_ready_live=true;
static bool a1599_decode_result_valid=true, a1599_decode_output_inspection_allowed;
static bool a1599_decode_output_snapshot_ready=true;
static unsigned char a1599_decode_output_snapshot[4096];
static int a1599_decode_consumed=1;
static int atomic_read(int *p) { return *p; }
'''
suite=r'''
static ssize_t read_output(loff_t off, size_t size, char *out) {
 return decode_output_read(NULL,NULL,NULL,out,off,size);
}
int main(void) {
 char out[4096]; memset(out,0x5a,sizeof out);
 memory.hardware_started=true; memory.decode[0].bytes=4096;
 memory.decode[0].cpu=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(memory.decode[0].cpu!=MAP_FAILED);
 assert(read_output(0,4096,out)==-EACCES);
 allow_output_export=true;
 assert(read_output(0,4096,out)==-ENODATA && out[0]==0x5a);
 /* Artificial eligibility is test-only: production has no grant path. */
 a1599_decode_output_inspection_allowed=true;
 a1599_decode_output_snapshot_ready=false;
 assert(read_output(0,4096,out)==-ENODATA && out[0]==0x5a);
 a1599_decode_output_snapshot_ready=true;
 a1599_decode_result_valid=false;
 assert(read_output(0,4096,out)==-ENODATA);
 a1599_decode_result_valid=true;
 for(unsigned int i=0;i<4096;i++) a1599_decode_output_snapshot[i]=(unsigned char)(i*7+19);
 assert(read_output(-1,1,out)==-EINVAL);
 assert(read_output(4097,1,out)==-EINVAL);
 assert(read_output(4096,1,out)==0);
 assert(read_output(0,4096,out)==4096);
 assert(!memcmp(out,a1599_decode_output_snapshot,4096));
 assert(read_output(4090,20,out)==6);
 assert(!memcmp(out,a1599_decode_output_snapshot+4090,6));
 munmap(memory.decode[0].cpu,4096);
 puts("production output export callback: PASS (gates, bounds, immutable snapshot; inaccessible DMA backing)");
}
'''
with tempfile.TemporaryDirectory(prefix='a1599-output-export-') as td:
 p=Path(td)/'export.c'; e=Path(td)/'export'
 p.write_text(prefix+s[a:b]+suite)
 subprocess.run(['cc','-D_DEFAULT_SOURCE','-std=c11','-O2','-Wall','-Wextra','-Werror',str(p),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
