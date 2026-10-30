#!/bin/sh
# DS Kart Recompiled para Linux: prepara el juego si hace falta y lo arranca.
#   sh linux/jugar.sh
# Los ajustes van en ajustes.ini (se crea la primera vez): NOMBRE, ESCALA (1-4), FPS (60/120/180/240),
# PANTALLA_COMPLETA (0/1), IDIOMA (1 inglés, 2 francés, 3 alemán, 4 italiano, 5 español), MICRO (0/1).
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
FIN
    echo "Creado ajustes.ini: cambia ahí tu NOMBRE y lo demás."
fi
sh linux/preparar.sh || exit 1
exec ./build/mkds
