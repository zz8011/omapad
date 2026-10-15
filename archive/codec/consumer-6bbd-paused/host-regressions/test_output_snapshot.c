// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../a1599_fw_decode_submit.h"

static unsigned int copies, barriers, waits, ring_reads;
static unsigned char backing[4096];
static void *observe_copy(void *dst, const void *src, size_t size)
{
	assert(src == backing && size == 4096);
	assert(barriers == copies + 1);
	copies++;
	return memcpy(dst, src, size);
}
#define memcpy observe_copy
#include "../a1599_fw_output_snapshot.h"
#undef memcpy

struct fake {
	struct a1599_decode_ring_snapshot ring;
	unsigned int bad_ring_at, error_ring_at;
	bool change_pixels, change_guard;
};
static int read_ring(void *context, struct a1599_decode_ring_snapshot *ring)
{
	struct fake *f = context;
	ring_reads++;
	if (ring_reads == f->error_ring_at)
		return -EIO;
	*ring = f->ring;
	if (ring_reads == f->bad_ring_at)
		ring->m2h_write_index = 8; /* additional message, including possible C7 */
	return 0;
}
static void barrier(void *context) { (void)context; barriers++; }
static void settle(void *context, unsigned int ms)
{
	struct fake *f = context;
	assert(ms == 50 && copies == 1 && barriers == 1);
	waits++;
	if (f->change_pixels) backing[17] ^= 1;
	if (f->change_guard) backing[4095] ^= 1;
}
static const struct a1599_output_snapshot_ops ops = {
	.read_ring=read_ring, .read_barrier=barrier, .wait_ms=settle,
};
static struct fake f;
static struct a1599_decode_submit_result response;
static struct a1599_output_snapshot_input input;
static struct a1599_output_snapshot_result result;
static unsigned char first[4096], second[4096];
static void reset(void)
{
	memset(&f,0,sizeof f); memset(&response,0,sizeof response);
	f.ring=(struct a1599_decode_ring_snapshot) {
		.h2m_descriptor=0x60cc01cc, .m2h_descriptor=0x60000033,
		.h2m_capacity_words=460, .m2h_capacity_words=51,
		.h2m_start=0x80cc, .m2h_start=0x8000,
		.h2m_read_index=10, .h2m_write_index=10,
		.m2h_read_index=0, .m2h_write_index=3,
	};
	response.published=response.consumed=response.response_received=true;
	response.response_record_valid=true;
	response.status_class=A1599_DECODE_C0_CLASS_10;
	response.response.fence=1;
	response.response.status=0x600644d5;
	response.response.dispatch_class=C0_DISPATCH_10;
	response.response.word4=3;
	response.message.length=12; response.message.write_index=3;
	input=(struct a1599_output_snapshot_input) {
		.response=&response, .cpu=backing, .bytes=4096, .authorized=true,
		.coherent_allocation=true, .resources_pinned=true,
	};
	for(unsigned int i=0;i<3072;i++)backing[i]=(unsigned char)(i*13+7);
	memset(backing+3072,0xa5,1024);
	memset(first,0,4096); memset(second,0,4096);
	copies=barriers=waits=ring_reads=0;
}
static int run(void)
{
	return a1599_output_snapshot_once(&input,&ops,&f,first,second,4096,&result);
}
static void rejected_before_reads(int expected)
{
	assert(run()==expected);
	assert(!copies && !barriers && !waits && !ring_reads);
	assert(!result.snapshot_ready && !result.eligible && !result.guard_checked);
}
int main(void)
{
	const unsigned int flags[]={0,0x600644d5,0x400644d5,0x200644d5,1};
	for(unsigned int i=0;i<sizeof flags/sizeof flags[0];i++) {
		reset(); response.response.status=flags[i]; assert(run()==0);
		assert(copies==2 && barriers==2 && waits==1 && ring_reads==3);
		assert(result.eligible && result.snapshot_ready && result.guard_checked &&
		       result.guard_intact && result.stability_checked && result.stable);
		assert(!memcmp(first,backing,4096) && !memcmp(first,second,4096));
	}
	reset(); response.response.word4=0xffff; assert(run()==0); /* opaque, never an index */
	reset(); input.authorized=false; rejected_before_reads(-EACCES);
	reset(); input.coherent_allocation=false; rejected_before_reads(-EINVAL);
	reset(); input.resources_pinned=false; rejected_before_reads(-EINVAL);
	reset(); input.cpu=NULL; rejected_before_reads(-EINVAL);
	reset(); input.bytes=4095; rejected_before_reads(-EINVAL);
	reset(); input.bytes=4097; rejected_before_reads(-EINVAL);
	reset(); input.response=NULL; rejected_before_reads(-ENODATA);
	reset(); response.published=false; rejected_before_reads(-ENODATA);
	reset(); response.consumed=false; rejected_before_reads(-ENODATA);
	reset(); response.response_received=false; rejected_before_reads(-ENODATA);
	reset(); response.response_record_valid=false; rejected_before_reads(-ENODATA);
	reset(); response.response_error=true; rejected_before_reads(-ENODATA);
	reset(); response.status_class=A1599_DECODE_C0_CLASS_8010; rejected_before_reads(-ENODATA);
	reset(); response.response.dispatch_class=C0_DISPATCH_8010; rejected_before_reads(-ENODATA);
	reset(); response.response.status=0x48064455; rejected_before_reads(-ENODATA);
	reset(); response.response.has_detail=1; rejected_before_reads(-ENODATA);
	reset(); response.response.fence=2; rejected_before_reads(-ENODATA);
	reset(); response.message.length=16; rejected_before_reads(-ENODATA);
	reset(); response.message.read_index=1; rejected_before_reads(-ENODATA);
	reset(); response.message.write_index=8; rejected_before_reads(-ENODATA);
	/* Fail each ring observation; no retries, partial snapshots never publish. */
	for(unsigned int at=1;at<=3;at++) {
		reset(); f.bad_ring_at=at; assert(run()==-ESTALE);
		assert(!result.snapshot_ready && copies==at-1 && ring_reads==at);
		reset(); f.error_ring_at=at; assert(run()==-EIO);
		assert(!result.snapshot_ready && copies==at-1 && ring_reads==at);
	}
	reset(); f.ring.h2m_read_index=9; assert(run()==-ESTALE && copies==0);
	reset(); f.ring.m2h_read_index=1; assert(run()==-ESTALE && copies==0);
	reset(); f.ring.m2h_descriptor^=4; assert(run()==-ESTALE && copies==0);
	reset(); f.ring.h2m_start+=4; assert(run()==-ESTALE && copies==0);
	reset(); backing[3072]=0; assert(run()==-EOVERFLOW && copies==1 && waits==0);
	assert(result.guard_checked && !result.guard_intact && !result.snapshot_ready);
	reset(); f.change_guard=true; assert(run()==-EOVERFLOW && copies==2);
	assert(!result.guard_intact && !result.snapshot_ready);
	reset(); f.change_pixels=true; assert(run()==-ESTALE && copies==2);
	assert(result.stability_checked && !result.stable && !result.snapshot_ready);
	puts("bounded coherent snapshot: PASS (normal/nonzero flags, gates, late message/error, guard mutation, data mutation, exact read limits)");
}
