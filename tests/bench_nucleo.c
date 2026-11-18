// Banco de pruebas del nucleo en PC: arranca bl40_core, ejecuta N ticks con toques pseudoaleatorios
// (semilla fija) y reporta tiempos + hash de los fotogramas. Uso: ver tools/medir_nucleo.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <setjmp.h>
#include "bl40_core_native.h"
#include "wasm-rt-impl.h"
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
static uint16_t rd16(const uint8_t*p){return p[0]|(p[1]<<8);}
static uint32_t rd32(const uint8_t*p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}
static int cmp(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return x<y?-1:x>y;}
int main(int argc,char**argv){
 int N=argc>2?atoi(argv[2]):3000;
 FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
 uint8_t*pack=malloc(sz);fread(pack,1,sz,f);fclose(f);
 wasm_rt_init();
 static w2c_bl40core m;
 WASM_RT_SAVE_STACK_DEPTH();
 if(WASM_RT_SETJMP(g_wasm_rt_jmp_buf)){printf("TRAP\n");return 1;}
 wasm2c_bl40core_instantiate(&m);
 w2c_bl40core_0x5F_wasm_call_ctors(&m);
 uint8_t*mem=m.w2c_memory.data;
 uint32_t n=rd32(pack);size_t off=4;
 for(uint32_t i=0;i<n;i++){
  uint16_t nl=rd16(pack+off);off+=2;const uint8_t*name=pack+off;off+=nl;
  uint32_t len=rd32(pack+off);off+=4;const uint8_t*data=pack+off;off+=len;
  uint32_t np=w2c_bl40core_web_alloc(&m,nl+1);memcpy(mem+np,name,nl);mem[np+nl]=0;
  uint32_t dp=w2c_bl40core_web_alloc(&m,len);memcpy(mem+dp,data,len);
  if(w2c_bl40core_bl40_register_asset(&m,np,dp,len)!=1){printf("reg fail %u\n",i);return 1;}
 }
 double t0=now();
 if(w2c_bl40core_bl40_boot(&m)!=1){printf("boot fail\n");return 1;}
 printf("boot %.1f ms\n",now()-t0);
 uint64_t hh=1469598103934665603ULL;uint32_t lastpc=0;double heavy=0;int nh=0;
 double*ts=malloc(sizeof(double)*N);double tot=0;
 int over10=0,over20=0,over50=0;
 for(int i=0;i<N;i++){
  /* toques sinteticos: un tap cada 1.5 s */
  static uint32_t rs=12345;
  int ph=i%90;
  if(ph==10){rs=rs*1664525u+1013904223u;}
  uint32_t tx=(rs>>8)%346,ty=(rs>>16)%776;
  if(ph==10)w2c_bl40core_bl40_send_touch(&m,0,tx,ty);
  if(ph==14)w2c_bl40core_bl40_send_touch(&m,2,tx,ty);
  double a=now();w2c_bl40core_bl40_tick(&m,10);double d=now()-a;
  if((i&1)==0){ (void)w2c_bl40core_bl40_presentation_count(&m);}
  if(d>1.0){heavy+=d;nh++;}
  {uint32_t pc=w2c_bl40core_bl40_presentation_count(&m);if(pc!=lastpc){lastpc=pc;uint32_t p=w2c_bl40core_bl40_frame_ptr(&m);const uint8_t*fb=mem+p;for(int k=0;k<346*776*2;k+=61){hh^=fb[k];hh*=1099511628211ULL;}}}
  ts[i]=d;tot+=d;if(d>10)over10++;if(d>20)over20++;if(d>50)over50++;
 }
 double mx=0;for(int i=0;i<N;i++)if(ts[i]>mx)mx=ts[i];
 qsort(ts,N,sizeof(double),cmp);
 printf("ticks=%d media=%.3f ms p50=%.3f p90=%.3f p99=%.3f max=%.3f  >10ms=%d >20ms=%d >50ms=%d  speed=%.2fx\n",N,tot/N,ts[N/2],ts[N*9/10],ts[N*99/100],mx,over10,over20,over50,10.0/(tot/N));
 printf("heavy_ticks=%d heavy_avg=%.3f ms frames=%u hash=%016llx\n",nh,nh?heavy/nh:0,lastpc,(unsigned long long)hh);
 return 0;}
