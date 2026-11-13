#!/bin/sh
# Crea los zips de una versión para GitHub Releases, uno por sistema, en publicar/:
#   DS-Kart-Recompiled-Launcher-Windows-vX.Y.Z.zip   Launcher.exe y el vídeo de la portada (lo usa también el botón
#                                                    de actualizar del launcher)
#   DS-Kart-Recompiled-Linux-vX.Y.Z.zip              sh linux/jugar.sh
#   DS-Kart-Recompiled-Mac-vX.Y.Z.zip                doble clic en mac/jugar.command
# El de Windows tiene que salir el primero en la lista de GitHub, que va por orden alfabético: los launcher hasta la
# 0.2.2 se quedan con el primer .zip y con otro no podrían actualizarse. Por eso "Launcher-Windows", antes que "Linux".
# Solo llevan nuestro código (el juego se genera desde la ROM de cada uno). Nunca rom/, extraido/, generado/, build/
# ni partida.sav.
# La versión es VERSION_NUM de launcher/Preparar.cs: súbela antes de crear una nueva.
set -e
cd "$(dirname "$0")/.."
V=$(grep -o 'VERSION_NUM = "[0-9.]*"' launcher/Preparar.cs | grep -o '[0-9.]*')
PY=/c/Users/Administrator/AppData/Local/Programs/Python/Python312/python.exe
sh launcher/compilar.sh

# lo que hace falta en los tres para generar el juego desde la ROM
comun() {
    mkdir -p "$1/tools" "$1/runtime"
    cp LICENSE faltan.txt faltan7.txt "$1/"
    cp tools/nds_rom.py tools/recomp.py tools/traducir_mkds.py "$1/tools/"
    cp runtime/*.c runtime/*.cpp runtime/*.h "$1/runtime/" && cp -r runtime/imgui "$1/runtime/"  # el menú (Dear ImGui)
}
empaqueta() {  # sistema
    "$PY" publicar/zip_version.py publicar/tmp/DS-Kart-Recompiled "publicar/DS-Kart-Recompiled-$1-v$V.zip"
    rm -rf publicar/tmp
}

D=publicar/tmp/DS-Kart-Recompiled
rm -rf publicar/tmp
rm -f publicar/*.zip publicar/portada.mp4

comun $D
cp Launcher.exe publicar/README.txt portada.mp4 $D/
empaqueta Launcher-Windows

comun $D
mkdir -p $D/linux
cp linux/preparar.sh linux/jugar.sh linux/lanzador.sh linux/actualiza.py linux/logo.bmp linux/LEEME.txt $D/linux/
mkdir -p $D/launcher
cp launcher/lanzador.cpp launcher/portada.c launcher/pl_mpeg.h $D/launcher/
cp portada.mpg $D/  # el vídeo del launcher (portada.mp4 pasado a MPEG-1)
echo "$V" > $D/linux/version.txt
cp linux/LEEME.txt $D/README.txt
empaqueta Linux

comun $D
mkdir -p $D/linux $D/mac
cp linux/preparar.sh linux/jugar.sh linux/lanzador.sh linux/actualiza.py linux/logo.bmp $D/linux/  # lo mismo que en Linux
mkdir -p $D/launcher
cp launcher/lanzador.cpp launcher/portada.c launcher/pl_mpeg.h $D/launcher/
cp portada.mpg $D/
echo "$V" > $D/linux/version.txt
cp mac/jugar.command mac/LEEME.txt $D/mac/
cp mac/LEEME.txt $D/README.txt
empaqueta Mac

ls -l publicar/*.zip
