#!/usr/bin/env python3
"""Compile the actual sysfs callback against the real acknowledgement helper.
Catches omitted authorization/publication/lifetime gates and accidentally
resetting the one-decode latch. Only kernel primitives and MMIO are doubled.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = (root / 'a1599_fw_first_ready_platform_harness.c').read_text()
start = src.index('static ssize_t c0_consumer_once_store(')
end = src.index('\n/* One-shot host-side diagnostic capture.', start)
callback = src[start:end]
base = (root / 'host-regressions/test_c0_ack.c').read_text().split('int main(void)')[0]
base = base.replace('"../', '"')
prefix = r'''
#include <sys/types.h>
struct device { int id; }; struct device_attribute { int id; };
static struct device device,wrong_device;
static struct device *a1599_live_device=&device;
static bool allow_c0_consumer_observation,allow_output_export=true;
static bool a1599_first_ready_live=true,a1599_decode_output_snapshot_ready=true;
static unsigned int a1599_decode_consumed=1;
static struct {bool hardware_started;struct{bool started;}decode_lifetime;} memory={true,{true}},*a1599_memory=&memory;
static struct ack_fake a1599_live_window;
static struct a1599_ring_message a1599_ack_c0;
static struct a1599_c0_ack_state a1599_ack_state;
static int a1599_ring_lock,locks,unlocks;
#define READ_ONCE(x) (x)
#define smp_load_acquire(p) (*(p))
#define a1599_c0_ack_ops ack_ops
#define dev_warn(...) ((void)0)
static int sysfs_streq(const char *a,const char *b){return !strcmp(a,b);}
static unsigned int atomic_read(unsigned int *p){return *p;}
static void mutex_lock(int*p){assert(!*p);*p=1;locks++;}
static void mutex_unlock(int*p){assert(*p);*p=0;unlocks++;}
'''
suite = r'''
int main(void){
 a1599_live_window=valid_device();a1599_ack_c0=valid_message();
 assert(c0_consumer_once_store(&device,NULL,"garbage",7)==-EINVAL);
 assert(c0_consumer_once_store(&device,NULL,"1",1)==-EACCES);
 allow_c0_consumer_observation=true;allow_output_export=false;
 assert(c0_consumer_once_store(&device,NULL,"1",1)==-EACCES);
 allow_output_export=true;
 assert(c0_consumer_once_store(&wrong_device,NULL,"1",1)==-EAGAIN);
 a1599_decode_consumed=0;
 assert(c0_consumer_once_store(&device,NULL,"1",1)==-EAGAIN);
 a1599_decode_consumed=1;a1599_decode_output_snapshot_ready=false;
 assert(c0_consumer_once_store(&device,NULL,"1",1)==-ENODATA);
 a1599_decode_output_snapshot_ready=true;memory.decode_lifetime.started=false;
 assert(c0_consumer_once_store(&device,NULL,"1",1)==-ENODATA);
 assert(!a1599_live_window.reads && !a1599_live_window.writes);
 memory.decode_lifetime.started=true;
 assert(c0_consumer_once_store(&device,NULL,"1",1)==1);
 assert(a1599_live_window.writes==1 && a1599_ack_state.verified);
 assert(a1599_decode_consumed==1 && a1599_decode_output_snapshot_ready);
 assert(c0_consumer_once_store(&device,NULL,"1",1)==-EALREADY);
 assert(a1599_live_window.writes==1 && locks==unlocks && !a1599_ring_lock);
 puts("PASS: actual c0_consumer_once callback gates, mutex, one write, immutable publication, decode latch retained");
}
'''
with tempfile.TemporaryDirectory(prefix='a1599-ack-callback-') as td:
    c = Path(td) / 'integration.c'
    exe = Path(td) / 'integration'
    c.write_text(base + prefix + callback + suite)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root), str(c), str(root.parent / 'id81_host_contract/id81_contract.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
