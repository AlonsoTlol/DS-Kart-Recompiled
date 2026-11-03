#!/bin/sh
# DS Kart Recompiled para Linux y macOS: prepara el juego a partir de tu ROM (lo que en Windows hace el launcher).
#   sh linux/preparar.sh        -> build/mkds
# Hace falta: python3, clang y SDL2 (en Ubuntu/Debian: sudo apt install python3 clang libsdl2-dev;
# en macOS: xcode-select --install y brew install sdl2).
# La ROM va en rom/mkds.nds (Mario Kart DS europeo, AMCP). Solo vuelve a compilar lo que haya cambiado.
set -e
cd "$(dirname "$0")/.."
if [ "$(uname -s)" = Darwin ]; then
    MAC=1
    PIDE="en macOS: xcode-select --install y brew install sdl2 (https://brew.sh)"
    [ -d /opt/homebrew/bin ] && PATH="/opt/homebrew/bin:$PATH"  # Homebrew en los Mac con chip de Apple
    [ -d /usr/local/bin ] && PATH="$PATH:/usr/local/bin"
else
    MAC=
    PIDE="en Ubuntu/Debian: sudo apt install python3 clang libsdl2-dev"
fi
for p in python3 clang sdl2-config; do
    command -v $p > /dev/null || { echo "Falta $p ($PIDE)"; exit 1; }
done
mkdir -p rom
[ -f rom/mkds.nds ] || { echo "Falta la ROM: copia tu Mario Kart DS europeo a rom/mkds.nds (Missing ROM: copy your European Mario Kart DS to rom/mkds.nds)"; exit 1; }

# 1. sacar el código de la ROM y traducirlo a C (solo si cambian las herramientas o la ROM)
# (compat.h y menu.h no cuentan: solo los usa el runtime, no el código traducido)
huella=$(cat tools/*.py faltan.txt faltan7.txt $(ls runtime/*.h | grep -v -e compat.h -e menu.h) rom/mkds.nds | cksum | cut -d' ' -f1)
if [ "$(cat build/traduccion.txt 2>/dev/null)" != "$huella" ]; then
    echo "Leyendo tu ROM y traduciendo el código del juego a C..."
    rm -rf extraido generado build/obj build/traduccion.txt
    python3 tools/nds_rom.py rom/mkds.nds extraido > /dev/null
    python3 tools/traducir_mkds.py > /dev/null
    mkdir -p build
    echo "$huella" > build/traduccion.txt
fi

# 2. compilar (en paralelo; un núcleo libre)
mkdir -p build/obj/runtime
if [ -n "$MAC" ]; then n=$(sysctl -n hw.ncpu); else n=$(nproc); fi
[ "$n" -gt 1 ] && n=$((n - 1))
OPC="-O2 -w -Iruntime -D_GNU_SOURCE"
[ -n "$MAC" ] && OPC="$OPC -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -DGL_SILENCE_DEPRECATION"  # ucontext y OpenGL de Apple
SDL="$(sdl2-config --cflags)"
[ -n "$MAC" ] && SDL="$SDL -I$(sdl2-config --prefix)/include"  # Homebrew solo da .../include/SDL2 y el código pide <SDL2/SDL.h>
lista() {
    [ -d generado ] && for d in generado/*/; do
        d=$(basename "$d"); mkdir -p build/obj/$d
        for f in generado/$d/*.c; do
            o=build/obj/$d/$(basename "$f" .c).o
            [ -f "$o" ] && [ "$o" -nt "$f" ] || echo "$f $o $d"
        done
    done
    for f in runtime/*.c; do
        case "$f" in runtime/ventana.c) continue;; esac  # la de Windows; en Linux y macOS va ventana_sdl.c
        o=build/obj/runtime/$(basename "$f" .c).o
        [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt runtime/compat.h ] && [ "$o" -nt runtime/menu.h ] || echo "$f $o runtime"
    done
    for f in runtime/menu.cpp runtime/imgui/imgui.cpp runtime/imgui/imgui_draw.cpp runtime/imgui/imgui_tables.cpp              runtime/imgui/imgui_widgets.cpp runtime/imgui/imgui_impl_opengl2.cpp; do  # el menú de ajustes (C++)
        o=build/obj/runtime/$(basename "$f" .cpp).o
        [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt runtime/menu.h ] && [ "$o" -nt runtime/imgui/imconfig.h ] || echo "$f $o menu"
    done
}
total=$(lista | wc -l)
if [ "$total" -gt 0 ]; then
    echo "Compilando $total archivos con $n núcleos (la primera vez tarda unos minutos)..."
    lista | SDL="$SDL" xargs -P "$n" -L 1 sh -c '
        extra=""; [ "$3" = mkds7 ] && extra="-DCPU7"
        [ "$3" != runtime ] && [ "$3" != menu ] && extra="$extra -Igenerado/$3"
        [ "$3" = runtime ] && extra="$SDL"
        [ "$3" = menu ] && extra="-fno-exceptions -fno-rtti -fno-threadsafe-statics"
        clang '"$OPC"' $extra -c "$1" -o "$2" || { echo "ERROR en $1"; exit 255; }' sh
fi

# 3. unir
if [ -n "$MAC" ]; then GL="-framework OpenGL"; else GL="-lGL"; fi
clang -o build/mkds build/obj/*/*.o $(sdl2-config --libs) $GL -lpthread -lm
rm -rf generado extraido   # ya compilado: cientos de MB que no hacen falta
echo "Listo: sh linux/jugar.sh"
