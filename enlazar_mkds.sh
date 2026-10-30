#!/bin/sh
# Une el código traducido de los dos procesadores con el runtime en mkds.exe.
# El menú de ajustes (menu.cpp y Dear ImGui) es C++: se compila sin excepciones ni RTTI y se enlaza como C.
set -e
cd "$(dirname "$0")"
CLANG="/c/Program Files/LLVM/bin/clang.exe"
mkdir -p build/menu
CPP="-O2 -w -Iruntime -fno-exceptions -fno-rtti -fno-threadsafe-statics -g"
for f in runtime/menu.cpp runtime/imgui/imgui.cpp runtime/imgui/imgui_draw.cpp runtime/imgui/imgui_tables.cpp \
         runtime/imgui/imgui_widgets.cpp runtime/imgui/imgui_impl_opengl2.cpp; do
    o=build/menu/$(basename "$f" .cpp).o
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ runtime/menu.h -nt "$o" ] || [ runtime/imgui/imconfig.h -nt "$o" ]; then
        "$CLANG" $CPP -c "$f" -o "$o"
    fi
done
"$CLANG" -O2 -w -Iruntime build/mkds*/*.o build/menu/*.o \
    runtime/cpu.c runtime/memoria.c runtime/bios.c runtime/hilos.c runtime/ds.c runtime/video.c runtime/gpu3d.c runtime/ventana.c runtime/sonido.c runtime/repeticion.c runtime/microfono.c runtime/wifi.c runtime/main_mkds.c \
    -lwinmm -lgdi32 -lopengl32 -luser32 -lxinput -lhid -lsetupapi -lws2_32 -liphlpapi -g -Wl,/STACK:268435456 -Wl,-debug -o build/mkds.exe
echo "ok: build/mkds.exe"
