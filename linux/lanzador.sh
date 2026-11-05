#!/bin/sh
# Compila el launcher con ventana (launcher/lanzador.cpp) si hace falta -> build/lanzador. Lo usa linux/jugar.sh.
# Hace falta lo mismo que para el juego: clang y SDL2.
cd "$(dirname "$0")/.." || exit 1
if [ "$(uname -s)" = Darwin ]; then
    MAC=1
    [ -d /opt/homebrew/bin ] && PATH="/opt/homebrew/bin:$PATH"
    [ -d /usr/local/bin ] && PATH="$PATH:/usr/local/bin"
else
    MAC=
fi
command -v clang > /dev/null && command -v sdl2-config > /dev/null || exit 1
mkdir -p build/lanzador_obj
OPC="-O2 -w -Iruntime -Iruntime/imgui -D_GNU_SOURCE -fno-exceptions -fno-rtti -fno-threadsafe-statics"
[ -n "$MAC" ] && OPC="$OPC -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -DGL_SILENCE_DEPRECATION"
SDL="$(sdl2-config --cflags)"
[ -n "$MAC" ] && SDL="$SDL -I$(sdl2-config --prefix)/include"
hecho=
for f in launcher/lanzador.cpp runtime/imgui/imgui.cpp runtime/imgui/imgui_draw.cpp runtime/imgui/imgui_tables.cpp \
         runtime/imgui/imgui_widgets.cpp runtime/imgui/imgui_impl_opengl2.cpp; do
    o=build/lanzador_obj/$(basename "$f" .cpp).o
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
        [ -z "$hecho" ] && echo "Preparando el launcher..."
        hecho=1
        clang $OPC $SDL -c "$f" -o "$o" || exit 1
    fi
done
if [ -n "$hecho" ] || [ ! -f build/lanzador ]; then
    if [ -n "$MAC" ]; then GL="-framework OpenGL"; else GL="-lGL"; fi
    clang -o build/lanzador build/lanzador_obj/*.o $(sdl2-config --libs) $GL -lpthread -lm || exit 1
fi
