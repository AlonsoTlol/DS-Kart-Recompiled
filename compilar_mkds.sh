#!/bin/sh
# Compila el código traducido de MKDS (cada carpeta de generado/) a objetos en build/.
# Solo recompila lo que ha cambiado (el .c o los encabezados son más nuevos que el .o).
# NUCLEOS=4 por defecto para no molestar si estás usando el PC.
cd "$(dirname "$0")"
CLANG="/c/Program Files/LLVM/bin/clang.exe"
for d in $(ls generado); do
    mkdir -p build/$d
    for o in build/$d/*.o; do  # objetos de archivos que ya no existen
        [ -f "$o" ] && [ ! -f "generado/$d/$(basename "$o" .o).c" ] && rm -f "$o"
    done
    for f in generado/$d/*.c; do
        o="build/$d/$(basename "$f" .c).o"
        if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ runtime/cpu.h -nt "$o" ] || [ generado/$d/funcs.h -nt "$o" ] \
           || [ generado/mkds/funcs.h -nt "$o" ]; then
            echo "$f"
        fi
    done
done | xargs -r -P "${NUCLEOS:-4}" -I{} sh -c '
    f="{}"; d=$(basename "$(dirname "$f")"); o="build/$d/$(basename "$f" .c).o"
    extra=""; [ "$d" = mkds7 ] && extra="-DCPU7"
    if "'"$CLANG"'" -O2 -w $extra -Iruntime -Igenerado/$d -c "$f" -o "$o" 2> "$o.err"; then
        echo "ok    $f"; rm -f "$o.err"
    else
        echo "ERROR $f"
    fi'
echo "terminado"
