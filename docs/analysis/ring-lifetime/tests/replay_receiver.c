/* Offline original C0 basic-block replay only; no MMIO/device access. */
typedef unsigned long U;
typedef unsigned int W;
typedef unsigned char B;
static long sc(long n,long a,long b,long c,long d,long e,long f) {
 register long x8 __asm__("x8")=n,x0 __asm__("x0")=a,x1 __asm__("x1")=b,
 x2 __asm__("x2")=c,x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");return x0;
}
static void out(long fd,const void*p,U n){sc(64,fd,(long)p,n,0,0,0);}
__attribute__((noreturn)) static void done(long n){sc(93,n,0,0,0,0,0);__builtin_unreachable();}
static void hex(U n){char s[17];for(int i=0;i<16;i++)s[i]="0123456789abcdef"[(n>>(60-4*i))&15];s[16]='\n';out(2,s,17);}
static U readfile(char *p,void *buf,U size){long fd=sc(56,-100,(long)p,0,0,0,0);if(fd<0)done(91);U n=0;while(n<size){long k=sc(63,fd,(long)((B*)buf+n),size-n,0,0,0);if(k<0)done(92);if(!k)break;n+=k;}sc(57,fd,0,0,0,0,0);return n;}
void *memset(void*p,int v,U n){B*b=p;for(U i=0;i<n;i++)b[i]=v;return p;}
void *memcpy(void*d,const void*s,U n){B*a=d;const B*b=s;for(U i=0;i<n;i++)a[i]=b[i];return d;}
static void zero(void*p,U n){memset(p,0,n);}


#define WAT(p,o) (*(W*)((B*)(p)+(o)))

/* Boundary doubles replace only MMIO read/write and logging. Receiver
 * instructions, dispatch table, copy/advance order, and wrap branches are raw. */
static W ring[51], rd, wr, core, reads, acks, nwords, irq_ack, irq;
static B context[128], record[1024];
static W raw_read(void *unused,W off) {
 (void)unused;
 if ((off>>16)!=core) done(71);
 off &= 0xffff;
 if(off==0x608)return irq;
 if(off==0x2fe8)return rd;
 if(off==0x2fec)return wr;
 if(off>=0x8000 && off<0x80cc && !(off&3)){
  if(reads>=nwords)done(72);
  reads++;return ring[(off-0x8000)/4];
 }
 done(73);
}
static void raw_write(void *unused,W off,W value){
 (void)unused;
 if((off>>16)!=core)done(74);
 off &= 0xffff;
 if(off==0x60c){if(value!=0x4000)done(75);irq_ack++;return;}
 if(off!=0x2fe8 || value!=wr || acks || reads!=nwords)done(76);
 /* Raw Apple record has not been classified yet, but full bytes are copied. */
 if(WAT(record,12)!=0 || WAT(record,4)!=0)done(77);
 for(W i=0;i<nwords;i++)if(WAT(record,16+4*i)!=ring[(rd+i)%51])done(78);
 rd=value;acks++;
}
static void log_noop(void){}
static void patch(U pc,void *fn){long off=(long)fn-(long)pc;if(off<-(1L<<27)||off>=(1L<<27))done(79);*(W*)pc=0x14000000|((off>>2)&0x3ffffff);}
static B driver[0x4000],queue[128],surface[256],surface_shared[32];
static U vtable[80],clients[32];
static W event,unlock_event,notify_event,zero_event,pop_event,submit_event;
static W pending;
#define UAT(p,o) (*(U*)((B*)(p)+(o)))
static W registered(void){return 1;}
static W queue_counter(void){return 7;}
static W surface_unlock(void *p,W option){
 if(p!=surface||option)done(81);unlock_event=++event;
 return ((W(*)(void*,W))0x7f3ee90)(p,option);
}
static void notify_client(void *p,W frame,W ctx,W unique,W err){
 if(p!=(void*)0x12345678||frame!=9||ctx!=2||unique!=3||err)done(82);
 notify_event=++event;
}
static void clear_record(void *p,U n){if(p!=driver+0xe18||n!=0xb18)done(83);zero_event=++event;zero(p,n);}
static void dequeue(void *q,U *client,W *unique,W *ctx,W *frame,W *unused,void *job,B *flag,W *err){
 if(q!=queue||!pending||!zero_event)done(84);
 *client=0x87654321;*unique=3;*ctx=2;*frame=10;*unused=0;*flag=0;*err=0;zero(job,0xb00);pop_event=++event;
}
static W submit_next(void *d,void *job,W ctx,W unique,W frame,W engine){
 (void)job;if(d!=driver||ctx!=2||unique!=3||frame!=10||engine!=0)done(85);
 if(driver[0x2453]||WAT(driver,0xdfc)||UAT(driver,0xe38)||UAT(driver,0xe08)!=0x87654321)done(86);
 submit_event=++event;return 0;
}
static void replay_completion(void){
 patch(0x869c66c,queue_counter);patch(0x869c5f8,registered);
 patch(0x7770abc,log_noop);patch(0x77718d0,log_noop);patch(0x7a6ff04,log_noop);
 patch(0x8680534,notify_client);patch(0x75b56c0,clear_record);
 patch(0x869bc34,dequeue);patch(0x867a044,submit_next);
 patch(0x7c40c34,log_noop);patch(0x7c40d64,log_noop);
 /* Actual IOSurface base vtable +1b8, with software locks doubled. */
 vtable[0x1b8/8]=0x7f3cb70;
 for(pending=0;pending<2;pending++){
  zero(driver,sizeof driver);zero(queue,sizeof queue);zero(surface,sizeof surface);zero(surface_shared,sizeof surface_shared);
  event=unlock_event=notify_event=zero_event=pop_event=submit_event=0;
  UAT(driver,0x2ae8)=(U)queue;WAT(queue,0x14)=pending;
  UAT(driver,0xe08)=0x12345678;WAT(driver,0xe18)=2;WAT(driver,0xe1c)=3;WAT(driver,0xe20)=9;
  UAT(driver,0xe38)=(U)surface;UAT(surface,0)=(U)vtable;vtable[0x228/8]=(U)surface_unlock;
  UAT(surface,0xc0)=(U)surface_shared;
  UAT(driver,0x2648)=(U)clients;clients[2]=0x12345678;
  driver[0x2650]=1;WAT(driver,0x2658)=1;WAT(driver,0x2694)=0x10;
  WAT(driver,0xdfc)=1;WAT(driver,0xe00)=pending;driver[0x2453]=1;
  /* With no queued job, skip the separate idle PM policy. */
  driver[0x2c10]=pending?1:0;
  W result=((W(*)(void*,void*,W))0x867c8e8)(driver,0,0);
  if(result||unlock_event!=1||notify_event!=2||zero_event!=3)done(87);
  if(WAT(surface_shared,12)!=1||WAT(surface_shared,16)!=1||surface[0x22]!=1||surface[0x23]!=1||surface[0x27]!=1)done(90);
  if(pending && (pop_event!=4||submit_event!=5))done(88);
  if(!pending && (pop_event||submit_event))done(89);
 }
 static const char ok[]="PASS: raw completion -> raw IOSurface base unlock/+1b8 -> notify -> record clear -> raw dequeue path -> next submit boundary (2 cases); locks/client/submit boundaries modeled, no DMA proof.\n";
 out(2,ok,sizeof ok-1);
}
void run(U *args){
 if(args[0]!=2)done(94);
 long m=sc(222,0x7004000,0x2000000,7,0x32,-1,0);if(m!=0x7004000)done(95);
 if(readfile((char*)args[2],(void*)m,0x2000000)!=27192152)done(96);
 patch(0x8677e84,raw_read);patch(0x8677ef0,raw_write);patch(0x8677de4,log_noop);
 int (*receiver)(void*,void*,W)=(void*)0x8681618;
 W lengths[]={12,13,16,127}; W cases=0;
 for(core=0;core<2;core++)for(W start=0;start<51;start++)for(W l=0;l<4;l++){
  W len=lengths[l];nwords=(len+3)/4;rd=start;wr=(start+nwords)%51;
  zero(context,sizeof context);zero(record,sizeof record);zero(ring,sizeof ring);
  WAT(context,0x60)=0x6000;WAT(context,0x64)=51;
  reads=acks=irq_ack=0;irq=(start&1)?0x4000:0;
  for(W i=0;i<nwords;i++)ring[(start+i)%51]=0x11110000+i;
  ring[start]=(len|0x80)|(0xc0<<8)|(1<<16);
  ring[(start+1)%51]=3;ring[(start+2)%51]=0x600644d5;
  if(receiver(context,record,core)!=0 || acks!=1 || rd!=wr || reads!=nwords || irq_ack!=!!irq || WAT(record,4)!=1 || WAT(record,8)!=1 || WAT(record,12)!=0x10)done(80);
  cases++;
 }
 out(2,"raw receiver cases=",19);hex(cases);
 static const char ok[]="PASS: actual Apple copy, modulo advance, consumer write before dispatch, both core offsets; no DMA modeled.\n";
 out(2,ok,sizeof ok-1);
 replay_completion();
 done(0);
}
__asm__(".global _start\n_start:\nmov x0,sp\nbl run\n");
