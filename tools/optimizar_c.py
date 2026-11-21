#!/usr/bin/env python3
"""Optimiza el C generado por wasm2c (sin cambiar su comportamiento).

El nucleo del BL40 emula la CPU ARM del firmware: cada instruccion pasa por
los accesos a memoria R8/R16/R32/W8/W16/W32 y por el decodificador de
desplazamientos/condiciones. clang no integra esas funciones en `step` (son
grandes por su ruta lenta con MMU) y el coste de llamada domina. Se marcan como
always_inline; el resultado es identico bit a bit (se comprobo el hash de miles
de fotogramas con toques aleatorios) y consume ~25 % menos instrucciones.

Uso: optimizar_c.py bl40_core_native.c   (idempotente; edita el archivo en sitio)
"""
import re, sys
from pathlib import Path

MARCA_HLE = '/* BL40-OPT: atajo de la copia ARM'
HLE_C = r'''
/* BL40-OPT: atajo de la copia ARM de la direccion 0x1fd50 (ldmcs/stmcs x2 + subcss #32 + bcs).
   Copia por bloques los 32*K bytes de K iteraciones completas dejando SIEMPRE la ultima iteracion al
   interprete (flags y salida del bucle exactos). Solo con MMU y ventanas desactivadas, ARM, alineado,
   sin solape y dentro de la RAM de 64 MB. Replica los contadores de escritura de W32 y el de instrucciones. */
static int bl40_hle_copy32(w2c_bl40core* instance, u32 p0) {
  u8* M = instance->w2c_memory.data;
  u32 v, base, dst, src, n, k, i;
  static const u32 code[6] = {0x28b15018u, 0x28a05018u, 0x28b15018u, 0x28a05018u, 0x22522020u, 0x2afffff9u};
  if (M[p0 + 100u] || M[p0 + 103u] != 1u) return 0; /* ARM y C=1 (las instrucciones son condicionales CS) */
  if ((M[808480u] | M[27300u]) & 1u) return 0;
  memcpy(&base, M + p0, 4);
  if (!base) return 0;
  for (i = 0; i < 6; i++) { memcpy(&v, M + base + 0x1fd50u + 4u * i, 4); if (v != code[i]) return 0; }
  memcpy(&dst, M + p0 + 4u, 4); memcpy(&src, M + p0 + 8u, 4); memcpy(&n, M + p0 + 12u, 4);
  if ((dst | src) & 3u) return 0;
  if (n < 64u || n >= 0x80000000u) return 0;
  k = (n >> 5) - 1u; if (k > 1024u) k = 1024u;
  { u64 db = (u64)dst + 32ull * k, sb = (u64)src + 32ull * k;
    if (db > 67108800ull || sb > 67108800ull) return 0;
    if (!((db <= src) || (sb <= dst))) return 0;
    if (!((db <= 0x1fd00ull) || (dst >= 0x1fe00u))) return 0; }
  memcpy(M + base + dst, M + base + src, 32u * k);
  { u32 r0 = dst + 32u * k, r1 = src + 32u * k, r2 = n - 32u * k, t;
    memcpy(M + p0 + 4u, &r0, 4); memcpy(M + p0 + 8u, &r1, 4); memcpy(M + p0 + 12u, &r2, 4);
    memcpy(&t, M + base + r1 - 16u, 4); memcpy(M + p0 + 16u, &t, 4);
    memcpy(&t, M + base + r1 - 12u, 4); memcpy(M + p0 + 20u, &t, 4);
    memcpy(&t, M + base + r1 - 8u, 4);  memcpy(M + p0 + 52u, &t, 4);
    memcpy(&t, M + base + r1 - 4u, 4);  memcpy(M + p0 + 60u, &t, 4); }
  M[p0 + 101u] = 0; M[p0 + 102u] = 0; M[p0 + 103u] = 1; M[p0 + 104u] = 0;
  { u64 c; memcpy(&c, M + p0 + 128u, 8); c += 6ull * k; memcpy(M + p0 + 128u, &c, 8); }
  { u64 st = 8ull * k, c; u32 flag, fb, w, h, fbbytes, cnt = 0;
    memcpy(&c, M + p0 + 5776u, 8); c += st; memcpy(M + p0 + 5776u, &c, 8);
    memcpy(&flag, M + p0 + 5820u, 4);
    if (flag) { memcpy(&c, M + p0 + 5800u, 8); c += st; memcpy(M + p0 + 5800u, &c, 8); }
    memcpy(&fb, M + p0 + 84u, 4);
    if (fb) {
      memcpy(&w, M + p0 + 92u, 4); memcpy(&h, M + p0 + 96u, 4); fbbytes = (w * h) << 1;
      for (i = 0; i < 8u * k; i++) if ((dst + 4u * i) - fb < fbbytes) cnt++;
      if (cnt) {
        memcpy(&c, M + p0 + 5784u, 8); c += cnt; memcpy(M + p0 + 5784u, &c, 8);
        if (flag) { memcpy(&c, M + p0 + 5808u, 8); c += cnt; memcpy(M + p0 + 5808u, &c, 8); }
      }
    } }
  return 1;
}
'''

MARCA = '/* BL40-OPT: accesores integrados */'
FUNCIONES = ['R8','R16','R32','W8','W16','W32',
             'shift','shiftImm','cond','addCarry',
             'v12PhysRead8','v12Write8','v12PageFor','v12Find']

def aplicar_hle(s):
    """Inserta el atajo de copia ARM antes de `step` y su llamada tras leer la direccion de la instruccion."""
    if MARCA_HLE in s:
        return s, False
    d = re.search(r'^void w2c_bl40core_step\(w2c_bl40core\* instance, u32 var_p0\) \{', s, re.M)
    if not d:
        return s, False
    call = re.compile(r'(  var_i0 = i32_load_default32\(&instance->w2c_memory, \(u64\)\(var_i0\) \+ 68u\);\n  var_l2 = var_i0;\n)')
    cuerpo = s[d.end():]
    m = call.search(cuerpo)
    if not m or len(call.findall(cuerpo)) < 1:
        return s, False
    cuerpo = cuerpo[:m.end()] + '  if (var_l2 == 130384u && bl40_hle_copy32(instance, var_p0)) {goto var_B0;}\n' + cuerpo[m.end():]
    return s[:d.start()] + HLE_C + '\n' + s[d.start():d.end()] + cuerpo, True

def main(path):
    p = Path(path)
    s = p.read_text()
    if MARCA in s:
        print('optimizar_c: ya aplicado'); return 0
    total = 0
    for f in FUNCIONES:
        pat = re.compile(r'^(u32|void) (w2c_bl40core_%s)\((w2c_bl40core\* instance)' % re.escape(f), re.M)
        s, n = pat.subn(r'__attribute__((always_inline)) static inline \1 \2(\3', s)
        if n != 1:
            print(f'optimizar_c: AVISO {f}: {n} definiciones (se esperaba 1); sin cambios para esa funcion')
        total += n
    if total == 0:
        print('optimizar_c: no se aplico ninguna optimizacion (nucleo distinto?); se compila sin ella')
        return 0
    s, hle = aplicar_hle(s)
    p.write_text(MARCA + '\n' + s)
    print(f'optimizar_c: {total} funciones integradas; atajo de copia ARM: {"si" if hle else "no (patron no encontrado)"}')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
