#include <stdio.h>
#include <stdint.h>
/* Host-test the two pieces of PURE LOGIC in hal_arm64.c that can be wrong
   independent of hardware: the CF8 address -> ECAM address decode, and the
   PL031 epoch -> CMOS field conversion. Same arithmetic, copied verbatim. */
#define ARM_ECAM_BASE 0x4010000000UL
static unsigned long ecam_addr(uint32_t cf8){
    uint8_t bus=(cf8>>16)&0xFF, dev=(cf8>>11)&0x1F, fn=(cf8>>8)&0x07;
    uint16_t off=cf8&0xFC;
    return ARM_ECAM_BASE+((unsigned long)bus<<20)+((unsigned long)dev<<15)+((unsigned long)fn<<12)+off;
}
static void rtc_fields(uint32_t t,int*yr,int*mo,int*dy,int*hh,int*mm,int*ss){
    *ss=t%60;t/=60;*mm=t%60;t/=60;*hh=t%24;t/=24;
    int y=1970; for(;;){int leap=((y%4==0&&y%100!=0)||y%400==0);uint32_t len=leap?366:365;if(t<len)break;t-=len;y++;}
    int leap=((y%4==0&&y%100!=0)||y%400==0);
    static const int md[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    int m=0; for(;m<12;m++){uint32_t len=(uint32_t)md[m]+((m==1&&leap)?1u:0u);if(t<len)break;t-=len;}
    *yr=y;*mo=m+1;*dy=(int)t+1;
}
int main(void){
    int fail=0;
    /* ECAM: bus0 dev0 fn0 off0 (enable bit 31 set, as pci.c sets it) */
    struct{uint32_t cf8; unsigned long want; const char*n;} v[]={
      {0x80000000, 0x4010000000UL, "b0 d0 f0 off0"},
      {0x80000800, 0x4010008000UL, "b0 d1 f0 off0"},
      {0x80001000, 0x4010010000UL, "b0 d2 f0 off0"},
      {0x80010000, 0x4010100000UL, "b1 d0 f0 off0"},   /* bus1 -> +0x100000 */
      {0x80000004, 0x4010000004UL, "b0 d0 f0 off4"},
      {0x80000100, 0x4010001000UL, "b0 d0 f1 off0"},
    };
    for(unsigned i=0;i<sizeof v/sizeof*v;i++){
        unsigned long got=ecam_addr(v[i].cf8);
        printf("  ECAM %-16s cf8=%08x -> %#lx %s\n",v[i].n,v[i].cf8,got,got==v[i].want?"OK":"FAIL");
        if(got!=v[i].want)fail=1;
    }
    /* RTC: known epochs */
    struct{uint32_t t;int Y,M,D,h,m,s;} r[]={
      {0,1970,1,1,0,0,0},
      {951782400,2000,2,29,0,0,0},      /* leap day 2000-02-29 */
      {1767225600,2026,1,1,0,0,0},      /* 2026-01-01 */
      {1786500000,2026,8,12,2,0,0},
    };
    for(unsigned i=0;i<sizeof r/sizeof*r;i++){
        int Y,M,D,h,m,s; rtc_fields(r[i].t,&Y,&M,&D,&h,&m,&s);
        int ok=(Y==r[i].Y&&M==r[i].M&&D==r[i].D&&h==r[i].h&&m==r[i].m&&s==r[i].s);
        printf("  RTC  t=%-11u -> %04d-%02d-%02d %02d:%02d:%02d (want %04d-%02d-%02d %02d:%02d:%02d) %s\n",
               r[i].t,Y,M,D,h,m,s,r[i].Y,r[i].M,r[i].D,r[i].h,r[i].m,r[i].s,ok?"OK":"FAIL");
        if(!ok)fail=1;
    }
    printf(fail?"RESULT: FAIL\n":"RESULT: ALL PASS\n"); return fail;
}
