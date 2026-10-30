#!/bin/sh
# Crea el zip de una versión para GitHub Releases: publicar/DS-Kart-Recompiled-vX.Y.Z.zip (con el vídeo de la
# portada). Es el único: se descarga la primera vez y lo usa también el botón de actualizar del launcher.
# Solo lleva nuestro código (el launcher genera el juego desde la ROM de cada uno). Nunca rom/, extraido/,
# generado/, build/ ni partida.sav.
# La versión es VERSION_NUM de launcher/Preparar.cs: súbela antes de crear una nueva.
set -e
cd "$(dirname "$0")/.."
V=$(grep -o 'VERSION_NUM = "[0-9.]*"' launcher/Preparar.cs | grep -o '[0-9.]*')
sh launcher/compilar.sh
D=publicar/tmp/DS-Kart-Recompiled
rm -rf publicar/tmp && mkdir -p $D/tools $D/runtime $D/linux
cp Launcher.exe LICENSE faltan.txt faltan7.txt publicar/README.txt $D/
cp tools/nds_rom.py tools/recomp.py tools/traducir_mkds.py $D/tools/
cp runtime/*.c runtime/*.cpp runtime/*.h $D/runtime/ && cp -r runtime/imgui $D/runtime/  # el menú (Dear ImGui)
cp linux/preparar.sh linux/jugar.sh linux/LEEME.txt $D/linux/  # Linux (beta): sh linux/jugar.sh
rm -f publicar/*.zip publicar/portada.mp4
cp portada.mp4 $D/
(cd publicar/tmp && powershell -NoProfile -Command "Compress-Archive -Path DS-Kart-Recompiled -DestinationPath ../DS-Kart-Recompiled-v$V.zip")
rm -rf publicar/tmp
ls -l publicar/*.zip
