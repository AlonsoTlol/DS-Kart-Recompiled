#!/bin/sh
# DS Kart Recompiled para macOS (beta): doble clic en el Finder (o "sh mac/jugar.command" en la Terminal).
# Prepara el juego la primera vez y lo arranca; es lo mismo que en Linux (linux/jugar.sh sabe que está en un Mac).
cd "$(dirname "$0")/.." || exit 1
sh linux/jugar.sh
echo
echo "(Puedes cerrar esta ventana / You can close this window)"
