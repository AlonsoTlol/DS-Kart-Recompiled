#!/bin/sh
# DS Kart Recompiled para Linux: abre el launcher con ventana (launcher/lanzador.cpp).
#   sh linux/jugar.sh              el launcher
#   sh linux/jugar.sh --terminal   sin launcher: prepara el juego si hace falta y lo arranca (también si no hay ventana)
# Los ajustes van en ajustes.ini (se crea la primera vez): NOMBRE, ESCALA (1-4), FPS (60/120/180/240),
# PANTALLA_COMPLETA (0/1), IDIOMA (1 inglés, 2 francés, 3 alemán, 4 italiano, 5 español), MICRO (0/1),
# DISPOSICION (0 las dos iguales, 1 la de arriba grande y la táctil pequeña al lado, 16:9, 2 superpuesta).
cd "$(dirname "$0")/.."
if [ ! -f ajustes.ini ]; then
    cat > ajustes.ini <<'FIN'
# Ajustes del juego (mkds los lee al arrancar)
NOMBRE=Jugador
ESCALA=2
FPS=60
PANTALLA_COMPLETA=0
IDIOMA=1
MICRO=1
DISPOSICION=0
FIN
    echo "Creado ajustes.ini: cambia ahí tu NOMBRE y lo demás."
fi
if [ "$1" != --terminal ] && sh linux/lanzador.sh; then
    while :; do
        ./build/lanzador
        c=$?
        [ "$c" = 42 ] || break  # 42: se ha actualizado, a compilarlo otra vez
        sh linux/lanzador.sh || exit 1
    done
    [ "$c" = 43 ] || exit "$c"  # 43: no se pudo abrir la ventana: sigue por la terminal
fi
sh linux/preparar.sh || exit 1
exec ./build/mkds
