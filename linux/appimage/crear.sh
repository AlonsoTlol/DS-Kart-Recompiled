#!/bin/bash
# Crea el AppImage (se lanza en Linux; aquí, en WSL). Uso: crear.sh carpeta-del-proyecto carpeta-de-trabajo salida.AppImage
# En la carpeta de trabajo tiene que estar lo descargado y descomprimido en x/ (zig 0.13.0, python-build-standalone 3.12
# "install_only_stripped" y el wheel pysdl2-dll 2.32.10 manylinux_2_28) y el arrancador en dl/runtime-x86_64
# (AppImage/type2-runtime). Las cabeceras de SDL2 salen de libsdl2-dev. Hace falta mksquashfs.
set -e
R="$(readlink -f "$1")"; T="$(readlink -f "$2")"; SAL="$(readlink -f "$(dirname "$3")")/$(basename "$3")"
A="$T/AppDir"; rm -rf "$A"; mkdir -p "$A/usr/bin" "$A/usr/lib" "$A/usr/share/dskart" "$A/usr/share/licencias"
V=$(grep -o 'VERSION_NUM = "[0-9.]*"' "$R/launcher/Preparar.cs" | grep -o '[0-9.]*')
D="$A/usr/share/dskart"

# 1. nuestro código: lo mismo que el zip de Linux
mkdir -p "$D/tools" "$D/runtime" "$D/linux" "$D/lib/include"
cp "$R/LICENSE" "$R/faltan.txt" "$R/faltan7.txt" "$D/"
cp "$R/tools/nds_rom.py" "$R/tools/recomp.py" "$R/tools/traducir_mkds.py" "$D/tools/"
cp "$R"/runtime/*.c "$R"/runtime/*.cpp "$R"/runtime/*.h "$D/runtime/" && cp -r "$R/runtime/imgui" "$D/runtime/"
cp "$R"/linux/{preparar.sh,jugar.sh,lanzador.sh,actualiza.py,logo.bmp} "$D/linux/"
cp "$R/linux/LEEME.txt" "$D/README.txt"
cp "$R/portada.mpg" "$D/"
echo "$V" > "$D/linux/version.txt"

# 2. SDL2 (biblioteca y cabeceras) y una libGL de mentira solo para enlazar (la de verdad es la de cada PC)
cp "$T"/x/sdl/sdl2dll/dll/libSDL2-2.0*.so.0* "$T/" 2>/dev/null || true
S=$(ls "$T"/x/sdl/sdl2dll/dll/libSDL2-2.0*.so* | head -1)
cp "$(readlink -f "$S")" "$D/lib/libSDL2-2.0.so.0"; chmod 755 "$D/lib/libSDL2-2.0.so.0"
cp -r /usr/include/SDL2 "$D/lib/include/"
cp /usr/include/x86_64-linux-gnu/SDL2/_real_SDL_config.h "$D/lib/include/SDL2/"
mkdir -p "$D/lib/include/GL"; echo "#include <SDL2/SDL_opengl.h>" > "$D/lib/include/GL/gl.h"  # lleva las declaraciones de OpenGL
rm -f "$T"/libSDL2-2.0*

# 3. Zig sin lo que no es Linux de 64 bits con glibc, y Python sin lo que no se usa
mkdir -p "$A/usr/zig" "$A/usr/python"
cp "$T/x/zig/zig" "$T/x/zig/LICENSE" "$A/usr/zig/"
cp -r "$T/x/zig/lib" "$A/usr/zig/lib"
L="$A/usr/zig/lib"
rm -rf "$L"/docs "$L"/tsan "$L"/libc/{mingw,musl,wasi,darwin} "$L"/init
find "$L/libc/include" -mindepth 1 -maxdepth 1 -type d ! -name 'generic-glibc' ! -name 'any-linux-any' ! -name 'x86-linux-any' \
    ! -name 'x86_64-linux-gnu' ! -name 'x86-linux-gnu' ! -name 'x86_64-linux-any' -exec rm -rf {} +
cp -r "$T/x/py/bin" "$T/x/py/lib" "$A/usr/python/"
P="$A/usr/python"
rm -rf "$P"/lib/python3.12/{test,idlelib,tkinter,ensurepip,site-packages,turtledemo,lib2to3,pydoc_data,config-3.12-x86_64-linux-gnu} \
    "$P"/lib/{tcl*,tk*,itcl*,thread*,pkgconfig,Tix*,libtcl*,libtk*} "$P"/share "$P"/include
find "$P/bin" -mindepth 1 ! -name 'python3.12' -delete
find "$P" -name __pycache__ -type d -exec rm -rf {} +

# 4. herramientas con los nombres que esperan los guiones, y el launcher ya compilado
cp -r "$R/linux/appimage/herramientas" "$A/usr/herramientas"; chmod 755 "$A"/usr/herramientas/*
sed -i 's/\r$//' "$A"/usr/herramientas/* "$D"/linux/*.sh "$D"/linux/*.py
cd "$D"
export DSKART_APPDIR="$A" ZIG_GLOBAL_CACHE_DIR="$T/cache" ZIG_LOCAL_CACHE_DIR="$T/cache"
export PATH="$A/usr/herramientas:$PATH"
W="$T/obj"; rm -rf "$W"; mkdir -p "$W"
OPC="-O2 -w -Iruntime -Iruntime/imgui -D_GNU_SOURCE -fno-exceptions -fno-rtti -fno-threadsafe-statics"
# la libGL de mentira: todas las funciones de OpenGL 1.x que declara SDL_opengl.h
grep -o 'GLAPIENTRY gl[A-Za-z0-9]*' lib/include/SDL2/SDL_opengl.h | awk '{print $2}' | sort -u | sed 's/.*/void &(void) {}/' > "$W/gl.c"
clang -shared -fPIC -O2 -Wl,-soname,libGL.so.1 "$W/gl.c" -o lib/libGL.so
for f in "$R/launcher/lanzador.cpp" "$R/launcher/portada.c" runtime/imgui/imgui.cpp runtime/imgui/imgui_draw.cpp \
         runtime/imgui/imgui_tables.cpp runtime/imgui/imgui_widgets.cpp runtime/imgui/imgui_impl_opengl2.cpp; do
    clang $OPC -Ilib/include -I"$R/launcher" -c "$f" -o "$W/$(basename "${f%.*}").o"
done
clang -s -o "$A/usr/bin/lanzador" "$W"/*.o $(sdl2-config --libs) -lGL -lpthread -lm
cp lib/libSDL2-2.0.so.0 "$A/usr/lib/"

# 5. lo demás del AppImage
cp "$R/linux/appimage/AppRun" "$A/AppRun"; sed -i 's/\r$//' "$A/AppRun"; chmod 755 "$A/AppRun"
sed 's/\r$//' "$R/linux/appimage/ds-kart-recompiled.desktop" > "$A/ds-kart-recompiled.desktop"
cp "$R/logo/icono_256.png" "$A/ds-kart-recompiled.png" 2>/dev/null || cp "$R/linux/appimage/icono.png" "$A/ds-kart-recompiled.png"
cp "$A/ds-kart-recompiled.png" "$A/.DirIcon"
cp "$T/x/zig/LICENSE" "$A/usr/share/licencias/zig.txt"
cp "$T/x/py/lib/python3.12/LICENSE.txt" "$A/usr/share/licencias/python.txt" 2>/dev/null || true
cp "$T"/x/sdl/pysdl2_dll-*.dist-info/licenses/libs/SDL2/README-SDL.txt "$A/usr/share/licencias/SDL2.txt"
cp "$R/LICENSE" "$A/usr/share/licencias/ds-kart-recompiled.txt"
(cd "$D" && find . -type f ! -name .id | sort | xargs sha256sum | sha256sum | cut -c1-16) > "$D/.id"
rm -f "$T/x.squashfs"
mksquashfs "$A" "$T/x.squashfs" -root-owned -noappend -comp zstd -Xcompression-level 22 -b 1M -quiet -no-progress
cat "$T/dl/runtime-x86_64" "$T/x.squashfs" > "$SAL"; chmod 755 "$SAL"; rm -f "$T/x.squashfs"
du -sh "$A/usr"/* | sort -h; ls -la "$SAL"
