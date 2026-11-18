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

MARCA = '/* BL40-OPT: accesores integrados */'
FUNCIONES = ['R8','R16','R32','W8','W16','W32',
             'shift','shiftImm','cond','addCarry',
             'v12PhysRead8','v12Write8','v12PageFor','v12Find']

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
    p.write_text(MARCA + '\n' + s)
    print(f'optimizar_c: {total} funciones integradas')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
