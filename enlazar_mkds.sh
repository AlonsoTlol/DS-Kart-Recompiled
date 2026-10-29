#!/bin/sh
# Une el código traducido de los dos procesadores con el runtime en mkds.exe.
set -e
cd "$(dirname "$0")"
CLANG="/c/Program Files/LLVM/bin/clang.exe"
"$CLANG" -O2 -w -Iruntime build/mkds*/*.o \
    runtime/cpu.c runtime/memoria.c runtime/bios.c runtime/hilos.c runtime/ds.c runtime/video.c runtime/gpu3d.c runtime/ventana.c runtime/sonido.c runtime/repeticion.c runtime/microfono.c runtime/wifi.c runtime/main_mkds.c \
    -lwinmm -lgdi32 -lopengl32 -luser32 -lxinput -lhid -lsetupapi -lws2_32 -liphlpapi -g -Wl,/STACK:268435456 -Wl,-debug -o build/mkds.exe
echo "ok: build/mkds.exe"
