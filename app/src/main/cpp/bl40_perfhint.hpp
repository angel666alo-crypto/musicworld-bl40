#pragma once
// Sugerencia de rendimiento de Android (ADPF, API 33+) cargada con dlopen:
// compila con cualquier minSdk y no hace nada si el sistema no la ofrece.
#include <dlfcn.h>
#include <cstddef>
#include <cstdint>
namespace bl40perf {
struct Hint {
  using GetMgr=void*(*)();
  using Create=void*(*)(void*,const int32_t*,size_t,int64_t);
  using Report=int(*)(void*,int64_t);
  using Close=void(*)(void*);
  void* session=nullptr; Report report=nullptr; Close close=nullptr;
  bool start(int32_t tid,int64_t targetNs){
    void* lib=dlopen("libandroid.so",RTLD_NOW);
    if(!lib)return false;
    auto getMgr=(GetMgr)dlsym(lib,"APerformanceHint_getManager");
    auto create=(Create)dlsym(lib,"APerformanceHint_createSession");
    report=(Report)dlsym(lib,"APerformanceHint_reportActualWorkDuration");
    close=(Close)dlsym(lib,"APerformanceHint_closeSession");
    if(!getMgr||!create||!report)return false;
    void* mgr=getMgr(); if(!mgr)return false;
    session=create(mgr,&tid,1,targetNs);
    return session!=nullptr;
  }
  void work(int64_t actualNs){ if(session&&report&&actualNs>0)report(session,actualNs); }
  void stop(){ if(session&&close)close(session); session=nullptr; }
};
}
