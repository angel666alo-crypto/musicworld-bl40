#!/usr/bin/env python3
"""Genera despacho C++ tipado desde exports REALES, para el núcleo convertido con WABT.
Sin WAMR. Se verifican las firmas: se rechazan i64/float/returns multiples y los imports.
"""
from pathlib import Path
import sys

def read_u32(b,p):
    val = shift = 0
    while True:
        x = b[p];p += 1
        val |= (x & 127) << shift
        if x < 128: return val,p
        shift += 7
        if shift > 35: raise ValueError('LEB demasiado grande')

def parse(path):
    b=Path(path).read_bytes()
    if b[:8]!=b'\0asm\x01\0\0\0': raise ValueError('El core no es WebAssembly 1.0')
    p=8; sections={}
    while p<len(b):
        sec=b[p];p+=1;size,p=read_u32(b,p);sections[sec]=b[p:p+size];p+=size
    if 2 in sections: raise ValueError('El core importa funciones: hay que implementar host imports')
    buf=sections[1];p=0;n,p=read_u32(buf,p);types=[]
    for _ in range(n):
        if buf[p] != 96: raise ValueError('Firma no soportada')
        p+=1;num,p=read_u32(buf,p);params=buf[p:p+num];p+=num
        num,p=read_u32(buf,p);results=buf[p:p+num];p+=num
        types.append((list(params),list(results)))
    buf=sections[3];p=0;n,p=read_u32(buf,p);indexes=[]
    for _ in range(n):
        idx,p=read_u32(buf,p);indexes.append(idx)
    buf=sections[7];p=0;n,p=read_u32(buf,p);exports={}
    for _ in range(n):
        size,p=read_u32(buf,p);name=buf[p:p+size].decode('utf-8');p+=size
        kind=buf[p];p+=1;idx,p=read_u32(buf,p)
        if kind==0: exports[name]=types[indexes[idx]]
    return exports

NEEDED=['__wasm_call_ctors','web_alloc','web_free','bl40_register_asset','bl40_boot','bl40_tick',
  'bl40_send_touch','bl40_frame_ptr','bl40_presentation_count',
  'bl40_pc_midi_pop','bl40_pc_audio_trace_n','bl40_pc_sound_service_n',
  'bl40_pc_unknown_count','bl40_pc_fast_pair_spans','bl40_pc_open_count',
  'bl40_pc_midi_captured','bl40_pc_audio_trace_count','bl40_pc_sound_service_count',
  'bl40_pc_midi_pending','bl40_pc_midi_dropped']

def generate(exports,dst,header_path=None):
    # No adivinar mangling C: extraer el nombre del export de la cabecera real de WABT.
    symbol_map={}
    if header_path:
        import re
        hdr=Path(header_path).read_text()
        for match in re.finditer(r"/\*\s*export:\s*'([^']+)'\s*\*/\s*(?:[\w* ]+?)\s+(\w+)\s*\(", hdr):
            symbol_map[match.group(1)]=match.group(2)
        if not symbol_map:
            raise ValueError('WABT no incluyo anotaciones de exports en su .h; revisa la version')

    out=['// Generado a partir de los exports REALES de bl40_core.wasm. NO EDITAR.',
         '#include "bl40_core_native.h"', '#include <cstring>', '#include <string>',
         'extern "C" uint32_t bl40_wasm2c_dispatch(w2c_bl40core* m, const char* name, const uint32_t* a, uint32_t argc, bool* ok) {',
         ' if (ok) *ok=false;', ' if (!m || !name) return 0;']
    for name in NEEDED:
        if name not in exports: raise ValueError('Falta export: '+name)
        params,results=exports[name]
        if any(x!=0x7f for x in params+results) or len(results)>1:
            raise ValueError(f'Firma no soportada: {name} {params} -> {results}')
        par=','.join('a[%d]'%i for i in range(len(params)))
        symbol=symbol_map.get(name,f'w2c_bl40core_{name}')
        if header_path and name not in symbol_map:
            raise ValueError('La cabecera real de WABT no exporta '+name)
        call=f'{symbol}(m'+(', '+par if par else '')+')'
        line='  if (ok) *ok=true; '
        line+=('return '+call+';' if results else call+'; return 0;')
        out.extend((f' if(std::strcmp(name,"{name}")==0) {{',
                    f'  if(argc!={len(params)})return 0;',line,' }'))
    out.extend((' return 0;', '}'))
    Path(dst).write_text('\n'.join(out)+'\n')
    print('Puente generado:',dst,'funciones:',len(NEEDED))

if __name__=='__main__':
    generate(parse(sys.argv[1]),sys.argv[2],sys.argv[3] if len(sys.argv)>3 else None)
