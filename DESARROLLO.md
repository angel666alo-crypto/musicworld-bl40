# Arquitectura y pendientes

## Estado del núcleo

La ruta activa ejecuta `bl40_core.wasm` convertido a C mediante WABT `wasm2c`, compilado a ARM64 dentro de `libmusicworld.so`. El núcleo reconstruye servicios de juego, pero no arranca el sistema operativo OKL4 del LG BL40 ni ejecuta íntegramente la versión PXO como aplicación nativa del firmware. El puente `bl40_dynarmic_bridge.cpp` es una interfaz de comprobación no integrada como motor de ejecución.

## Procesamiento

- Tiempo virtual: cada llamada completada a `bl40_tick(10)` avanza 10 ms de huésped. El planificador usa tiempo monotónico y mantiene la fase absoluta; puede reajustar el plazo del anfitrión ante suspensiones o bloqueos largos sin saltar pasos virtuales.
- Vídeo: framebuffer RGB565 del núcleo reconstruido presentado con OpenGL ES 2.0.
- Audio: eventos MIDI convertidos por TinySoundFont al banco TimGM6mb y salida PCM por AAudio; no es el sintetizador Qualcomm CMX original.
- Entrada: eventos táctiles Android traducidos al formato del huésped, con tratamiento de pulsaciones y movimientos.
- Recursos: `bl40_assets.pack` contiene 145 entradas; `INTEGRIDAD_ASSETS.json` registra hashes de los tres archivos mayores.

## Limitaciones conocidas

La fidelidad del firmware y del audio no está terminada. El motor del huésped conserva costos altos en determinados ticks; el planificador no puede compensar una CPU insuficiente sin perder fluidez. La sincronización entre escenas, audio y touch requiere validación en partidas largas y repetidas. Los diagnósticos no prueban jugabilidad completa. La prueba en Linux no sustituye una ejecución en Android.

## Trabajo para continuar

1. Crear un conjunto de pruebas de escenas reproducibles con entradas táctiles, CRC de framebuffer y eventos musicales, comparando cada paso y sus tiempos virtuales.
2. Medir tiempos de `bl40_tick` en Android y perfilar las rutinas ARM/Thumb y los accesos de memoria del núcleo, sin alterar el reloj para ocultar los costos.
3. Investigar el cargador PXO, los mapas MMU/CP15, estados de CPU, colas ProcMngr/MsgHandler y scheduler OKL4 antes de conectar una ruta Dynarmic real.
4. Recuperar los bancos CMX/MIDI originales, con su estructura de eventos y sincronización, antes de sustituir TimGM6mb.
5. Probar gestos, toques, pausa/reanudación y varias partidas consecutivas en un dispositivo Android físico.

## Compilación y mantenimiento

El script `compilar_ubuntu.sh` comprueba dependencias, ejecuta las pruebas disponibles, obtiene WABT/TinySoundFont y genera los fuentes C necesarios. WABT se fija a la etiqueta `1.0.39`; `TinySoundFont` se descarga desde el repositorio principal. Los fuentes `generated/` son artefactos de compilación, no deben editarse directamente. Los recursos originales y sus hashes deben mantenerse sin modificaciones accidentales.

Las pruebas de carga basadas en distribuciones anteriores son sintéticas y no deben presentarse como una medición de rendimiento del teléfono. Mantener el diagnóstico del runtime y el aviso de limitaciones al modificar el motor.


## Rendimiento (v2)

- Perfil medido en PC: el nucleo emula la CPU ARM del firmware (`step`, accesos R8/R16/R32/W8/W16/W32, MMU) y dibuja cada fotograma de 346x776 por software. Un tick normal cuesta ~0,001 ms, pero un tick con fotograma (cada 4 ticks, 25 fps) cuesta 7-10 ms en un PC rapido; en gama baja eso es varias veces el presupuesto de 10 ms.
- `tools/optimizar_c.py` (lo ejecuta `compilar_ubuntu.sh`) integra los accesos a memoria y el decodificador en `step`: ~25 % menos instrucciones, fotogramas identicos.
- `bash tools/medir_nucleo.sh` compara en PC antes/despues y falla si el hash de fotogramas difiere. Ejecutalo tras cambiar de version de WABT o del `bl40_core.wasm`.
- El informe (`nativeStats` / LOG JSON) incluye `tickHistogram`, `heavyTickAvgMs`, `coreCpuLoadPercent` y el bloque `device` (modelo, SoC, hilos, estado termico).
- Siguiente paso de mayor impacto: sustituir el interprete ARM por un JIT (Dynarmic) o reducir el trabajo de dibujado del huesped; ambos requieren el codigo fuente del nucleo.
