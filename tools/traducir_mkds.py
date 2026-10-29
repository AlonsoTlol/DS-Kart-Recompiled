"""Traduce el código de Mario Kart DS (lo extraído con nds_rom.py) a C.

    python tools/traducir_mkds.py   -> generado/mkds/*.c (ARM9) y generado/mkds7/*.c (ARM7)

De momento solo el código fijo. Los overlays del ARM9 van después:
comparten direcciones entre sí y necesitan su propio nombre por overlay.
"""

import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import recomp  # noqa: E402
from recomp import Image, Recomp  # noqa: E402

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def entradas_extra(nombre):
    out = []
    ruta = os.path.join(RAIZ, nombre)
    if os.path.exists(ruta):
        for l in open(ruta):
            l = l.strip()
            if l:
                out.append((int(l[2:], 16), True) if l.startswith("T:") else (int(l, 16), False))
    return out


def traduce(nombre, img, entries, hooks, carpeta, escribir=True, **kw):
    t0 = time.time()
    print(f"{nombre}:")
    parches = kw.pop("parches", None)
    rc = Recomp(img, entries, hooks, **kw)
    if parches:
        rc.parches = parches
    rc.discover(log=lambda m: print("  " + m), prologos=True)
    thumb = sum(1 for k in rc.funcs if k[1])
    print(f"  ARM: {len(rc.funcs) - thumb} funciones, Thumb: {thumb}")
    if escribir:
        files = rc.write(os.path.join(RAIZ, "generado", carpeta))
        print(f"  {len(files)} archivos C en generado/{carpeta} ({time.time() - t0:.0f} s)")
    return rc


def puentes(d, ram):
    """Funciones puente ARM sin push lr (ldr ip,=destino ... bx ip) que solo se llaman por puntero.
    Las de las Misiones (overlay 1) no las encontraba ninguna búsqueda. Solo cuentan si van justo
    detrás de datos o de otro salto, para no partir una función más grande."""
    res = set()
    for o in range(4, len(d) - 16, 4):
        if struct.unpack_from("<I", d, o)[0] & 0xFFFFF000 != 0xE59FC000:
            continue
        antes = struct.unpack_from("<I", d, o - 4)[0]
        if antes >> 28 == 0xE and antes & 0x0FFFFFF0 != 0x012FFF10:
            continue
        sig = struct.unpack_from("<3I", d, o + 4)
        if 0xE12FFF1C in sig and all(w >> 28 == 0xE for w in sig[:sig.index(0xE12FFF1C)]):
            res.add((ram + o, False))
    return res


def overlays(ext, info, fijas):
    """Cada overlay se traduce aparte (nombres A_o0_..., tabla con firmas)."""
    ovs = [ov for ov in info["overlays9"]]
    datos = {}
    for ov in ovs:
        with open(os.path.join(ext, f"overlay9_{ov['id']}.bin"), "rb") as f:
            datos[ov["id"]] = f.read()
    faltan = entradas_extra("faltan.txt")
    cruzadas = {ov["id"]: set() for ov in ovs}
    for pasada in range(2):
        rcs = {}
        for ov in ovs:
            n, ram, d = ov["id"], ov["ram"], datos[ov["id"]]
            img = Image()
            img.add(ram, d)
            entries = set(cruzadas[n])
            # tabla de inicializadores estáticos (punteros a función)
            for a in range(ov["sinit_start"], ov["sinit_end"], 4):
                if ram <= a < ram + len(d) - 3:
                    v = struct.unpack_from("<I", d, a - ram)[0]
                    entries.add((v & ~1, True) if v & 1 else (v, False))
            entries |= puentes(d, ram)
            debiles = {k for k in faltan if ram <= k[0] < ram + len(d) and img.has(k[0], 2)}
            entries = {k for k in entries if img.has(k[0], 2)}
            # Misiones (overlay 1): 0219F3A4 calcula hasta qué nivel está abierto (9 misiones superadas abren el
            # siguiente); con todo desbloqueado, todos
            # y 021A0E34 si el jefe del nivel está abierto (las 8 misiones superadas)
            ganchos = {(0x0219F3A4, True): ("truco_niveles_mision", False, "si"),
                       (0x021A0E34, True): ("truco_jefe_mision", False, "si")} if n == 1 else None
            rcs[n] = traduce(f"overlay {n}", img, sorted(entries), ganchos, f"mkds_o{n}", escribir=False,
                             prefix=f"_o{n}", disp="dispatch", external=fijas, firmas=True,
                             debiles=debiles)
        # llamadas de un overlay a otro: entradas para el destino en la segunda pasada
        nuevas = False
        for n, rc in rcs.items():
            for k in rc.fuera:
                for ov in ovs:
                    if ov["id"] != n and ov["ram"] <= k[0] < ov["ram"] + len(datos[ov["id"]]):
                        if k not in cruzadas[ov["id"]]:
                            cruzadas[ov["id"]].add(k)
                            nuevas = True
        if not nuevas:
            break
    usados = []
    for n, rc in rcs.items():
        if not rc.funcs:
            continue
        rc.write(os.path.join(RAIZ, "generado", f"mkds_o{n}"))
        usados.append(n)
    # registro para el runtime (cpu.c: buscar_ov)
    lineas = ['#include "cpu.h"\n']
    for n in usados:
        lineas.append(f"extern const FuncEntryOv g_funcs_o{n}[];\nextern const size_t g_nfuncs_o{n};\n")
    lineas.append("\nconst TablaOv g_overlays[] = {\n")
    for n in usados:
        lineas.append(f"    {{g_funcs_o{n}, &g_nfuncs_o{n}}},\n")
    if not usados:
        lineas.append("    {0, 0},\n")
    lineas.append(f"}};\nconst size_t g_noverlays = {len(usados)};\n")
    Recomp._save(os.path.join(RAIZ, "generado", "mkds", "overlays.c"), "".join(lineas))
    print(f"overlays traducidos: {usados}")


def arm9(ext, info):
    with open(os.path.join(ext, "arm9.bin"), "rb") as f:
        arm9 = f.read()
    ram = info["arm9_ram"]
    img = Image()
    img.add(ram, arm9[:info["module_params"]["autoload_start"] - ram])
    for a in info["autoloads"]:  # ITCM y DTCM, en la dirección donde se ejecutan
        img.add(a["ram"], arm9[a["offset"]:a["offset"] + a["size"]])
    hooks = {  # funciones del SDK que se sustituyen por versiones del runtime
        (0x0200E6EC, False): ("hle_save_context", False),  # OS_SaveContext
        (0x0200E738, False): ("hle_load_context", False),  # OS_LoadContext
        (0x0200E688, False): ("hle_init_context", False, "antes"),  # OS_InitContext
        # truco TODO_DESBLOQUEADO (lo mismo que los códigos Action Replay de desbloqueo)
        (0x02056DEC, True): ("truco_desbloqueado", False, "si"),   # ¿está desbloqueado X?
        (0x02056DAC, True): ("truco_cilindradas", False, "si"),    # cilindrada más alta (4 = espejo)
    }
    entries = [(info["arm9_entry"], False)]
    parches = {0x021189D6: ("g_trucos & 1", "R[0] = 1;")}  # estado de cada copa: desbloqueada
    return traduce("ARM9", img, entries, hooks, "mkds", parches=parches, debiles=entradas_extra("faltan.txt"))


def arm7(ext, info):
    with open(os.path.join(ext, "arm7.bin"), "rb") as f:
        arm7 = f.read()
    ram = info["arm7_ram"]
    img = Image()
    img.add(ram, arm7)
    # El arranque del ARM7 copia trozos a su WRAM, a la VRAM que le presta el
    # ARM9 y a 0x027E0000. La tabla (inicio lista, fin lista, datos) está en
    # el literal que usa la rutina de copia (0x02380198 en MKDS).
    lista, fin, datos = struct.unpack_from("<3I", arm7, 0x198)
    while lista < fin:
        dest, tam, bss = struct.unpack_from("<3I", arm7, lista - ram)
        img.add(dest, arm7[datos - ram:datos - ram + tam])
        print(f"  ARM7: {tam} bytes de código/datos en {dest:08X}")
        datos += tam
        lista += 12
    hooks = {
        (0x037F96DC, False): ("hle_save_context", False),  # OS_SaveContext
        (0x037F9710, False): ("hle_load_context", False),  # OS_LoadContext
        (0x037F9678, False): ("hle_init_context", False, "antes"),  # OS_InitContext
    }
    entries = [(info["arm7_entry"], False)]
    recomp.VIEJO = set(x for x in os.environ.get("VIEJO7", "").split(",") if x)
    if recomp.VIEJO:
        print(f"  (ARM7 con lo de antes en: {sorted(recomp.VIEJO)})")
    traduce("ARM7", img, entries, hooks, "mkds7", prefix="7", armv4=True, debiles=entradas_extra("faltan7.txt"))


def main():
    ext = os.path.join(RAIZ, "extraido")
    with open(os.path.join(ext, "info.json")) as f:
        info = json.load(f)
    rc9 = arm9(ext, info)
    overlays(ext, info, set(rc9.funcs))
    arm7(ext, info)


if __name__ == "__main__":
    main()
