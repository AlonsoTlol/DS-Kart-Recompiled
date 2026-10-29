"""Mide lo "raras" que salen las imágenes intermedias: cada una se compara con la mezcla de la real de antes y la de
después (en bloques de 16x16 de la pantalla de arriba). Un polígono estirado da un bloque con error enorme.
Las imágenes vienen en grupos de 3 (intermedia 1/3, intermedia 2/3, real).
    python error_intermedias.py carpeta
"""
import glob
import struct
import sys

import numpy as np


def carga(f):
    d = open(f, "rb").read()
    off, w, h = struct.unpack_from("<I", d, 10)[0], struct.unpack_from("<i", d, 18)[0], struct.unpack_from("<i", d, 22)[0]
    a = np.frombuffer(d, dtype=np.uint8, offset=off, count=w * abs(h) * 4).reshape(abs(h), w, 4)[:, :, :3]
    return a[: abs(h) // 2].astype(np.float32)  # pantalla de arriba


fs = sorted(glob.glob(sys.argv[1] + "/t_*.bmp"))
imgs = [carga(f) for f in fs]
# encontrar las reales: en cada grupo de 3, la tercera. Se prueba el desfase que da menos error.
mejor = None
for desfase in range(3):
    peores = []
    for k in range(desfase + 3, len(imgs) - 1, 3):
        a, b = imgs[k - 3], imgs[k]  # reales
        for j, t in ((k - 2, 1 / 3), (k - 1, 2 / 3)):
            esperado = a * (1 - t) + b * t
            e = np.abs(imgs[j] - esperado).mean(axis=2)
            h, w = e.shape
            bloques = e[: h // 16 * 16, : w // 16 * 16].reshape(h // 16, 16, w // 16, 16).mean(axis=(1, 3))
            peores.append(bloques.max())
    if peores and (mejor is None or np.median(peores) < mejor[0]):
        mejor = (np.median(peores), desfase, peores)
med, desfase, peores = mejor
peores = np.array(peores)
print(f"intermedias: {len(peores)}  peor bloque: mediana {np.median(peores):.1f}  p95 {np.percentile(peores, 95):.1f}"
      f"  máx {peores.max():.1f}  (>60: {int((peores > 60).sum())})")
