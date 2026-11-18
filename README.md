# Music World BL40 — Android

Proyecto Android ARM64 para ejecutar una reconstrucción del juego Music World del LG BL40.

## Compilación en Ubuntu (WSL de Windows)

Se necesitan Ubuntu WSL, Git, Python 3, CMake, Ninja, compiladores C/C++, Java 17 y Android SDK con plataforma 35, NDK `25.2.9519653` y CMake `3.22.1`. Se utiliza Gradle 8.11.1. La primera compilación descarga WABT y TinySoundFont y genera C nativo desde WebAssembly; necesita acceso a Internet.

```bash
sudo apt update
sudo apt install -y git python3 build-essential clang cmake ninja-build unzip curl openjdk-17-jdk nodejs
mkdir -p ~/proyectos
# Copia MusicWorld_BL40_Proyecto.zip desde Descargas de Windows a ~/proyectos
cd ~/proyectos
unzip -o MusicWorld_BL40_Proyecto.zip
cd MusicWorld_BL40
bash compilar_ubuntu.sh
```

Para copiar el ZIP desde Descargas de Windows sin modificarlo, antes del `unzip` puedes usar, cambiando `TU_USUARIO_WINDOWS` por tu usuario real:

```bash
cp "/mnt/c/Users/TU_USUARIO_WINDOWS/Downloads/MusicWorld_BL40_Proyecto.zip" ~/proyectos/
```

El APK se genera en `salida/MusicWorld_BL40.apk`. En WSL, el script intenta copiarlo también a la carpeta Descargas de Windows. No hace falta ADB para compilar ni transferir el APK por USB.

## Pruebas del proyecto

```bash
bash probar.sh
```

Las pruebas comprueban integridad de recursos, planificador de reloj, cola PCM, escenario de carga sintética, orden de inicialización, integración del backend nativo y arranque básico del núcleo en Node.js. No equivalen a una prueba funcional en Android.

## Organización

- `app/src/main/cpp/`: runtime, sincronización, audio, render y enlace con el núcleo convertido a C.
- `app/src/main/java/`: actividad Android y entrada táctil.
- `app/src/main/assets/`: núcleo, recursos, ROM de solo lectura y banco musical.
- `tools/generar_puente_wasm2c.py`: generación de enlaces entre el núcleo convertido y el runtime.
- `tests/`: pruebas reproducibles en Linux.
- `DESARROLLO.md`: arquitectura, limitaciones y tareas pendientes.

## Dependencias y recursos

WABT (Apache-2.0) y TinySoundFont (licencia zlib) se obtienen de sus repositorios públicos durante la compilación. El banco SF2 incluido deriva de TimGM6mb; se conserva su licencia en `SOUNDFONT_LICENCIAS.txt`. Los recursos de Music World y LG se incluyen para esta labor de investigación y compatibilidad; su presencia no implica cesión de derechos ni autorización general de redistribución.
