#!/usr/bin/env python3
from pathlib import Path
import tempfile, subprocess, importlib.util, re, sys, os
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('generator',ROOT/'tools/generar_puente_wasm2c.py')
gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
exp=gen.parse(ROOT/'app/src/main/assets/bl40_core.wasm')
assert len(exp)>350, len(exp)
cpp=ROOT/'app/src/main/cpp'
s=(cpp/'native_engine.cpp').read_text()
cm=(cpp/'CMakeLists.txt').read_text()
assert 'wasm_runtime_' not in s and '<wasm_export.h>' not in s
assert 'wamr_vm' not in cm and 'WAMR_ROOT_DIR' not in cm
assert '"wasm2cNativeEnabled"' in s.replace('\\','')
assert 'juego=wasm2c C ARM64' in s
with tempfile.TemporaryDirectory() as d:
    p=Path(d)
    hdr=['#pragma once','#include <stdint.h>','typedef uint32_t u32;',
         'typedef struct {uint8_t bytes[8];} w2c_bl40core;',
         'typedef struct {uint8_t* data;uint64_t size;} wasm_rt_memory_t;',
         'extern "C" {','void wasm2c_bl40core_instantiate(w2c_bl40core*);',
         'void wasm2c_bl40core_free(w2c_bl40core*);',
         'wasm_rt_memory_t* w2c_bl40core_memory(w2c_bl40core*);']
    funcs=['#include "bl40_core_native.h"','#include "wasm-rt.h"','#include "wasm-rt-impl.h"','#include <stdlib.h>',
        'static uint8_t guest[65536];static wasm_rt_memory_t memory={guest,65536};',
        'extern "C" {',
        'void wasm2c_bl40core_instantiate(w2c_bl40core*){}',
        'void wasm2c_bl40core_free(w2c_bl40core*){}',
        'wasm_rt_memory_t* w2c_bl40core_memory(w2c_bl40core*){return &memory;}']
    for name in gen.NEEDED:
        args,outs=exp[name]
        prot=('u32' if outs else 'void')+f' w2c_bl40core_{name}(w2c_bl40core*'+(',u32'*len(args))+')'
        hdr.append(prot+';')
        if name=='bl40_tick':
            funcs.append('u32 w2c_bl40core_bl40_tick(w2c_bl40core*,u32 ms){if(ms==99){longjmp(g_wasm_rt_jmp_buf.buffer,1);}return 1;}')
        else:
            funcs.append(prot+'{'+('return 1;' if outs else '')+'}')
    hdr.append('}')
    funcs.extend(['void wasm_rt_init(void){}','void wasm_rt_free(void){}',
       'void wasm_rt_init_thread(void){}','void wasm_rt_free_thread(void){}',
       'thread_local wasm_rt_jmp_buf g_wasm_rt_jmp_buf={};',
       'const char* wasm_rt_strerror(wasm_rt_trap_t){return "trap";}','}'])
    (p/'bl40_core_native.h').write_text('\n'.join(hdr)+'\n')
    (p/'wasm-rt.h').write_text('#pragma once\n#include <setjmp.h>\n#include <stdbool.h>\ntypedef int wasm_rt_trap_t;typedef struct{bool initialized;jmp_buf buffer;} wasm_rt_jmp_buf;\n\n#ifdef __cplusplus\nextern "C" {\n#endif\nconst char* wasm_rt_strerror(wasm_rt_trap_t);\nvoid wasm_rt_init(void);void wasm_rt_free(void);void wasm_rt_init_thread(void);void wasm_rt_free_thread(void);\n#ifdef __cplusplus\n}\n#endif\n')
    (p/'wasm-rt-impl.h').write_text('#pragma once\n#include "wasm-rt.h"\nextern "C" { extern thread_local wasm_rt_jmp_buf g_wasm_rt_jmp_buf; }\n#define WASM_RT_SAVE_STACK_DEPTH() ((void)0)\n#define WASM_RT_SETJMP(v) ((v).initialized=true,setjmp((v).buffer))\n')
    (p/'fake_core.cpp').write_text('\n'.join(funcs)+'\n')
    gen.generate(exp,p/'bl40_core_dispatch.cpp')
    (p/'main.cpp').write_text('''#include "bl40_wasm2c_backend.hpp"
#include <string>
#include <cassert>
int main(){std::string e; uint32_t r=0;
assert(bl40_native_initialize(e));
assert(bl40_native_validate_address(1,65535));
assert(!bl40_native_validate_address(64000,2000));
assert(bl40_native_pointer(15));
assert(bl40_native_call("bl40_tick",{10},r,e) && r==1);
assert(!bl40_native_call("bl40_tick",{99},r,e));
assert(e.find("Trap")!=std::string::npos);
assert(!bl40_native_call("bl40_tick",{10,10},r,e));
assert(!bl40_native_call("incorrecta",{},r,e));
bl40_native_release();}
''')
    subprocess.run(['clang++','-std=c++20','-O1','-I',str(p),'-I',str(cpp),str(cpp/'bl40_wasm2c_backend.cpp'),str(p/'bl40_core_dispatch.cpp'),str(p/'fake_core.cpp'),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: 369+ exports, sin WAMR, dispatcher tipado, memoria, boot y fallo de firma')
