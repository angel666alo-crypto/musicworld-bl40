#include "bl40_wasm2c_backend.hpp"
#include "bl40_core_native.h"
#include "wasm-rt.h"
#include "wasm-rt-impl.h"
#include <memory>
#include <new>
#include <cstring>

extern "C" uint32_t bl40_wasm2c_dispatch(w2c_bl40core*, const char*, const uint32_t*, uint32_t, bool*);
namespace {
 w2c_bl40core* module_instance=nullptr;
 bool initialized=false;
}
bool bl40_native_initialize(std::string& error) {
 if(module_instance){ error="El modulo C ya esta iniciado"; return false; }
 wasm_rt_init();
 initialized=true;
 module_instance=new(std::nothrow) w2c_bl40core{};
 if(!module_instance) {error="Sin memoria para la instancia C nativa";wasm_rt_free();initialized=false; return false;}
 WASM_RT_SAVE_STACK_DEPTH();
 const int bootTrap=WASM_RT_SETJMP(g_wasm_rt_jmp_buf);
 if(bootTrap!=0){
  error=std::string("Trap en init wasm2c: ")+wasm_rt_strerror((wasm_rt_trap_t)bootTrap);
  bl40_native_release();return false;
 }
 wasm2c_bl40core_instantiate(module_instance);
 // El WASM no tiene start section; inicializar explicita y una sola vez los
 // constructores C++ del nucleo tras instalar memoria y globales del modulo.
 bool ctorsOk=false;
 bl40_wasm2c_dispatch(module_instance,"__wasm_call_ctors",nullptr,0,&ctorsOk);
 if(!ctorsOk){error="No se llamaron los constructores reales __wasm_call_ctors";bl40_native_release();return false;}
 const wasm_rt_memory_t* mem=w2c_bl40core_memory(module_instance);
 if(!mem || !mem->data || mem->size==0){error="No hay memoria guest en wasm2c";bl40_native_release();return false;}
 g_wasm_rt_jmp_buf.initialized=false;
 return true;
}
void bl40_native_release(){
 if(module_instance){wasm2c_bl40core_free(module_instance);delete module_instance;module_instance=nullptr;}
 if(initialized){wasm_rt_free();initialized=false;}
}
void bl40_native_enter_guest_thread(){wasm_rt_init_thread();}
void bl40_native_leave_guest_thread(){wasm_rt_free_thread();}
bool bl40_native_call(const char* name,std::initializer_list<uint32_t> args,
                      uint32_t& result,std::string& error){
 if(!module_instance){error="Nucleo C nativo sin inicializar";return false;}
 bool ok=false;
 WASM_RT_SAVE_STACK_DEPTH();
 const int trap=WASM_RT_SETJMP(g_wasm_rt_jmp_buf);
 if(trap!=0){
  error=std::string("Trap de codigo C nativo en ")+name+": "+wasm_rt_strerror((wasm_rt_trap_t)trap);
  g_wasm_rt_jmp_buf.initialized=false;
  return false;
 }
 result=bl40_wasm2c_dispatch(module_instance,name,args.begin(),(uint32_t)args.size(),&ok);
 g_wasm_rt_jmp_buf.initialized=false;
 if(!ok){error=std::string("No existe export/firma nativa: ")+(name?name:"(null)");return false;}
 return true;
}
bool bl40_native_validate_address(uint32_t offset,uint32_t size){
 if(!module_instance || offset==0) return false;
 const auto* mem=w2c_bl40core_memory(module_instance);
 return mem && mem->data && (uint64_t)offset+size<=mem->size;
}
uint8_t* bl40_native_pointer(uint32_t offset){
 if(!bl40_native_validate_address(offset,1))return nullptr;
 return w2c_bl40core_memory(module_instance)->data+offset;
}
