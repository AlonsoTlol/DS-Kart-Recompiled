"""Como error_intermedias.py pero solo en la zona del kart que sigue la cámara (abajo en el centro de la pantalla
de arriba): ahí se ve si el personaje se deshace en las imágenes intermedias. Guarda la peor (con sus vecinas).
    python tools/error_kart.py carpeta
"""
import glob, struct, sys
import numpy as np

def carga(f):
    d = open(f, "rb").read()
    off, w, h = struct.unpack_from("<I", d, 10)[0], struct.unpack_from("<i", d, 18)[0], struct.unpack_from("<i", d, 22)[0]
    return np.frombuffer(d, dtype=np.uint8, offset=off, count=w * abs(h) * 4).reshape(abs(h), w, 4)[:, :, :3][: abs(h) // 2].astype(np.float32)

fs = sorted(glob.glob(sys.argv[1] + "/t_*.bmp"))
imgs = [carga(f) for f in fs]
mejor = None
for desfase in range(3):
    r = []
    for k in range(desfase + 3, len(imgs) - 1, 3):
        a, b = imgs[k - 3], imgs[k]
        for j, t in ((k - 2, 1 / 3), (k - 1, 2 / 3)):
            zona = np.abs(imgs[j] - (a * (1 - t) + b * t))[110:190, 88:168].mean()
            r.append((zona, j))
    if mejor is None or np.median([x[0] for x in r]) < np.median([x[0] for x in mejor]):
        mejor = r
v = np.array([x[0] for x in mejor])
print(f"zona del kart: mediana {np.median(v):.1f}  p95 {np.percentile(v, 95):.1f}  máx {v.max():.1f}  (>25: {int((v > 25).sum())})")
peor = max(mejor)[1]
print("peor:", fs[peor])
