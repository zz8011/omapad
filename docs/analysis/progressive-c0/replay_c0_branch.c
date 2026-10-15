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
__attribute__((noreturn)) static void error(void){out(2,"APPLE ERROR return-PC=",22);hex((U)__builtin_return_address(0));done(90);}

#define WAT(p,o) (*(W*)((B*)(p)+(o)))
static void log_noop(void) {}
static void patch(U pc,void*fn){long off=(long)fn-(long)pc;*(W*)pc=0x14000000|((off>>2)&0x3ffffff);}
void replay(B*,W*);
__asm__("replay:\nsub sp,sp,#0xc0\nstp x19,x20,[sp,#0x80]\nstp x24,x30,[sp,#0x90]\nmov x20,x0\nstr x1,[sp,#0x40]\nstr wzr,[sp,#0x4c]\nmovz x16,#0x17b4\nmovk x16,#0x868,lsl #16\nadr x17,1f\nstr x17,[sp,#0x78]\nblr x16\n1:\nldp x19,x20,[sp,#0x80]\nldp x24,x30,[sp,#0x90]\nadd sp,sp,#0xc0\nret\n");
void run(U*args){
 if(args[0]!=2)done(94);
 long m=sc(222,0x7004000,0x2000000,7,0x32,-1,0);if(m!=0x7004000)done(95);
 if(readfile((char*)args[2],(void*)m,0x2000000)!=27192152)done(96);
 *(W*)0x8681864=0xf9403ffe;*(W*)0x8681868=0xd65f03c0;patch(0x8677de4,log_noop);
 long page=sc(222,0,8192,3,0x22,-1,0);if(page<0)done(97);
 if(sc(226,page+4096,4096,0,0,0,0))done(98);
 W values[]={0x600644d5,0x400644d5,0x200644d5,0x000644d5,0x60064455,0,0x48064455};
 for(U i=0;i<sizeof values/sizeof values[0];i++){
  U len=(values[i]&(1u<<27))?16:12;
  B *rec=(B*)(page+4096-len-16);zero(rec,len+16);
  rec[16]=(B)(0x80|len);rec[17]=0xc0;rec[18]=1;rec[20]=3;WAT(rec,24)=values[i];
  if(len==16)WAT(rec,28)=0x12345678;
  W cls=0;replay(rec,&cls);
  out(2,"status=",7);hex(values[i]);out(2,"class=",6);hex(cls);
  if(cls!=((values[i]&(1u<<27))?0x8010:0x10))done(99);
 }
 static const char ok[]="PASS: 7 raw C0 dispatch paths; 12-byte paths end at an inaccessible guard page.\n";out(2,ok,sizeof ok-1);done(0);
}
__asm__(".global _start\n_start:\nmov x0,sp\nbl run\n");
