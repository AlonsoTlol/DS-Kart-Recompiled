"""Traductor estático ARM/Thumb -> C para el ARM9 de la DS (ARM946E-S, ARMv5TE).

Igual que WiiCompiled con el PowerPC de la Wii: lee el código máquina, lo
divide en funciones y escribe cada función como una función de C que hace lo
mismo sobre una estructura Cpu (registros y banderas) y la memoria emulada.

Cómo encuentra el código:
  1. Empieza por los puntos de entrada que le dan.
  2. Sigue los saltos dentro de cada función; cada BL/BLX es otra función.
  3. Un B a la entrada de otra función se traduce como "llamada de cola".
  4. Los saltos calculados (switch, punteros a función) se resuelven en tiempo
     de ejecución: primero se mira si es un "return", luego un switch local
     con los destinos conocidos, y si no, la tabla global de funciones.
     Tras un salto calculado se recorre el código que sigue (hasta el primer
     dato de la función) para no perder los "case" de los switch.

Uso suelto (binario plano):
    python tools/recomp.py --seg 02000000:prueba.bin --entry 02000000 --out generado/x
"""

import argparse
import os
import struct

MASK = 0xFFFFFFFF

# Para depurar: cambios de la traducción que se pueden desactivar ("ret", "barrido", "todos",
# "debil", "fronteras"); traducir_mkds.py los rellena desde la variable VIEJO7 solo para el ARM7.
VIEJO = set()
NUEVO = set()


def sx(v, bits):
    m = 1 << (bits - 1)
    return (v & ((1 << bits) - 1) ^ m) - m


def ror(v, s):
    s &= 31
    return ((v >> s) | (v << (32 - s))) & MASK if s else v


def h(v):
    return f"0x{v & MASK:08X}u"


COND = ["c->z", "!c->z", "c->c", "!c->c", "c->n", "!c->n", "c->v", "!c->v",
        "(c->c && !c->z)", "(!c->c || c->z)", "(c->n == c->v)", "(c->n != c->v)",
        "(!c->z && c->n == c->v)", "(c->z || c->n != c->v)", None, None]


TICK = "if (--c->ciclos < 0) cpu_tick(c);"


def fname(addr, thumb, prefix=""):
    return f"{'T' if thumb else 'A'}{prefix}_{addr:08X}"


class Image:
    """El código tal y como queda en la memoria de la DS."""

    def __init__(self):
        self.segs = []

    def add(self, start, data):
        self.segs.append((start, bytes(data)))

    def _find(self, a, n):
        for s, d in self.segs:
            if s <= a and a + n <= s + len(d):
                return s, d
        return None

    def has(self, a, n):
        return self._find(a, n) is not None

    def u16(self, a):
        s, d = self._find(a, 2)
        return struct.unpack_from("<H", d, a - s)[0]

    def u32(self, a):
        s, d = self._find(a, 4)
        return struct.unpack_from("<I", d, a - s)[0]


# ---------------------------------------------------------------------------
# Análisis del flujo: qué hace cada instrucción con el contador de programa
# ---------------------------------------------------------------------------

class Flow:
    __slots__ = ("size", "kind", "cond", "target", "tthumb", "literal", "tail")

    def __init__(self, size):
        self.size = size
        self.kind = "seq"      # seq, branch, call, callind, ret, jumpind, tail, undef
        self.cond = False      # True si la instrucción es condicional
        self.target = None
        self.tthumb = False
        self.literal = None    # dirección de un dato leído con [pc, #x]
        self.tail = False      # branch traducido como llamada de cola

    def falls(self):
        if self.kind in ("seq", "call", "callind"):
            return True
        if self.kind == "undef":
            return False
        return self.cond


def arm_class(i):
    op = (i >> 25) & 7
    if op == 0:
        if (i & 0x0FFFFFF0) == 0x012FFF10: return "bx"
        if (i & 0x0FFFFFF0) == 0x012FFF30: return "blxr"
        if (i & 0x0FFF0FF0) == 0x016F0F10: return "clz"
        if (i & 0x0F900FF0) == 0x01000050: return "qarith"
        if (i & 0x0F900090) == 0x01000080: return "smulxy"
        if (i & 0x0FB00FF0) == 0x01000090: return "swp"
        if (i & 0x0FC000F0) == 0x00000090: return "mul"
        if (i & 0x0F8000F0) == 0x00800090: return "mull"
        if (i & 0x90) == 0x90 and (i & 0x60): return "hw"
        if (i & 0x0FBF0FFF) == 0x010F0000: return "mrs"
        if (i & 0x0FB0FFF0) == 0x0120F000: return "msr"
        if (i & 0x01900000) == 0x01000000: return "undef"
        return "dp"
    if op == 1:
        if (i & 0x0FB0F000) == 0x0320F000: return "msr"
        if (i & 0x01900000) == 0x01000000: return "undef"
        return "dp"
    if op == 2: return "sdt"
    if op == 3: return "undef" if i & 0x10 else "sdt"
    if op == 4: return "ldm"
    if op == 5: return "b"
    if op == 6: return "cop"
    if ((i >> 24) & 15) == 0xF: return "swi"
    return "mcr" if i & 0x10 else "cop"


MOV_LR_PC = 0xE1A0E00F


def arm_flow(img, a):
    i = img.u32(a)
    cond = i >> 28
    f = Flow(4)
    f.cond = cond < 0xE
    if cond == 0xF:
        if ((i >> 25) & 7) == 5:  # BLX inmediato: llama a Thumb
            f.kind = "call"
            f.target = (a + 8 + (sx(i, 24) << 2) + (((i >> 24) & 1) << 1)) & MASK
            f.tthumb = True
        elif (i & 0xFD70F000) != 0xF550F000:  # todo menos PLD
            f.kind = "undef"
        return f
    cls = arm_class(i)
    prev_mov_lr = img.has(a - 4, 4) and img.u32(a - 4) == MOV_LR_PC
    if cls == "bx":
        rm = i & 15
        if rm == 15:
            f.kind, f.target, f.tthumb = "tail", a + 8, False
        elif rm == 14:
            f.kind = "ret"
        else:
            f.kind = "callind" if prev_mov_lr else "jumpind"
    elif cls == "blxr":
        f.kind = "callind"
    elif cls == "b":
        f.target = (a + 8 + (sx(i, 24) << 2)) & MASK
        f.kind = "call" if (i >> 24) & 1 else "branch"
    elif cls == "ldm":
        if (i >> 20) & 1 and (i >> 15) & 1:
            f.kind = "ret" if ((i >> 16) & 15) == 13 else ("callind" if prev_mov_lr else "jumpind")
    elif cls == "sdt":
        rn, rd, ld = (i >> 16) & 15, (i >> 12) & 15, (i >> 20) & 1
        if ld and rn == 15 and not (i >> 25) & 1:
            off = i & 0xFFF
            f.literal = a + 8 + (off if (i >> 23) & 1 else -off)
        if ld and rd == 15:
            if f.literal is not None and img.has(f.literal, 4):
                t = img.u32(f.literal)
                f.kind = "call" if prev_mov_lr else "tail"
                f.target, f.tthumb = t & ~1 if t & 1 else t & ~3, bool(t & 1)
            elif rn == 13 and not (i >> 24) & 1:
                f.kind = "ret"  # ldr pc, [sp], #4
            else:
                f.kind = "callind" if prev_mov_lr else "jumpind"
    elif cls == "dp":
        opc, rd = (i >> 21) & 15, (i >> 12) & 15
        if rd == 15 and not (8 <= opc <= 11):
            if opc == 13 and not (i >> 25) & 1 and (i & 0xFFF) == 14:
                f.kind = "ret"  # mov pc, lr
            else:
                f.kind = "callind" if prev_mov_lr else "jumpind"
    elif cls == "undef":
        f.kind = "undef"
    return f


def thumb_flow(img, a):
    i = img.u16(a)
    f = Flow(2)
    if (i & 0xFF00) == 0x4700:  # BX / BLX registro
        rm = (i >> 3) & 15
        if i & 0x80:
            f.kind = "callind"
        elif rm == 15:
            f.kind, f.target, f.tthumb = "tail", (a + 4) & ~3, False
        elif rm == 14:
            f.kind = "ret"
        elif "ret" not in VIEJO and rm < 8 and img.has(a - 2, 2) and (img.u16(a - 2) & 0xFF00) == 0xBC00 and (img.u16(a - 2) >> rm) & 1:
            f.kind = "ret"  # pop {rX} + bx rX: la vuelta típica del compilador de Nintendo en Thumb
        else:
            f.kind = "jumpind"
    elif (i & 0xFC00) == 0x4400 and ((i >> 8) & 3) in (0, 2) and ((i & 7) | ((i >> 4) & 8)) == 15:
        rm = (i >> 3) & 15
        f.kind = "ret" if ((i >> 8) & 3) == 2 and rm == 14 else "jumpind"
    elif (i & 0xFF00) == 0xBD00:  # pop {.., pc}
        f.kind = "ret"
    elif (i & 0xF800) == 0x4800:
        f.literal = ((a + 4) & ~3) + (i & 0xFF) * 4
    elif (i & 0xF000) == 0xD000:
        cond = (i >> 8) & 15
        if cond == 0xE:
            f.kind = "undef"
        elif cond != 0xF:
            f.kind, f.cond = "branch", True
            f.target = (a + 4 + sx(i, 8) * 2) & MASK
    elif (i & 0xF800) == 0xE000:
        f.kind = "branch"
        f.target = (a + 4 + sx(i, 11) * 2) & MASK
    elif (i & 0xF800) == 0xF000:
        if img.has(a + 2, 2):
            j = img.u16(a + 2)
            off = sx(i, 11) << 12
            if (j & 0xF800) == 0xF800:
                f.size, f.kind, f.tthumb = 4, "call", True
                f.target = (a + 4 + off + ((j & 0x7FF) << 1)) & MASK
            elif (j & 0xF801) == 0xE800:
                f.size, f.kind, f.tthumb = 4, "call", False
                f.target = (a + 4 + off + ((j & 0x7FF) << 1)) & ~3 & MASK
            else:
                f.kind = "undef"
        else:
            f.kind = "undef"
    elif (i & 0xF000) == 0xF000 or (i & 0xF800) == 0xE800 or (i & 0xFF00) == 0xBE00:
        f.kind = "undef"
    return f


# ---------------------------------------------------------------------------
# Funciones
# ---------------------------------------------------------------------------

class Func:
    def __init__(self, addr, thumb):
        self.addr = addr
        self.thumb = thumb
        self.ins = {}            # dirección -> Flow
        self.calls = set()       # (dirección, thumb) de funciones a las que llama
        self.literals = set()
        self.dynsites = []       # saltos calculados
        self.dyn_targets = set() # destinos posibles de esos saltos dentro de la función
        self.missing = set()


class Recomp:
    def __init__(self, img, entries, hooks=None, prefix="", armv4=False, disp=None, debiles=None,
                 external=None, firmas=False):
        self.img = img
        self.prefix = prefix   # "7" para el ARM7: A7_..., g_funcs7, dispatch7; "_o0" overlay 0
        self.disp = disp or "dispatch" + prefix
        # Funciones de fuera de esta imagen que ya están traducidas (el código fijo,
        # para un overlay): se llaman directamente con su nombre sin prefijo.
        self.external = external or set()
        self.firmas = firmas   # tabla con los primeros bytes de cada función (overlays)
        # parches condicionales: dirección -> (condición en C, código en C que sustituye a la instrucción)
        self.parches = {}
        self.fuera = set()     # destinos fuera de la imagen: se resuelven al ejecutar
        self.v5 = not armv4    # ARMv5: ldr/ldm/pop a pc cambian ARM/Thumb según el bit 0
        self.entries = set(entries)
        self.funcs = {}
        # (dirección, thumb) -> (función C del runtime, ¿necesita setjmp en la llamada?)
        self.hooks = hooks or {}
        self.entries |= set(self.hooks)
        # entradas no seguras (las apuntadas en faltan.txt): se traducen, pero no cortan barridos
        # ni convierten saltos en llamadas de cola, por si caen en mitad de una función
        self.debiles = set(debiles or []) - self.entries

    def explore(self, key, strong, known):
        """Recorre una función. `strong`: entradas seguras (BL, arranque, hooks); un B
        a una de ellas es llamada de cola. `known`: todas, para limitar los barridos."""
        addr, thumb = key
        fn = Func(addr, thumb)
        step = 2 if thumb else 4
        todo = [addr]
        swept = set()
        while True:
            while todo:
                a = todo.pop()
                if a in fn.ins:
                    continue
                if not self.img.has(a, step):
                    fn.missing.add(a)
                    continue
                f = thumb_flow(self.img, a) if thumb else arm_flow(self.img, a)
                fn.ins[a] = f
                if f.literal is not None:
                    fn.literals.add(f.literal)
                k = f.kind
                if k == "branch":
                    if (f.target, thumb) in strong and f.target != addr:
                        f.tail = True
                        fn.calls.add((f.target, thumb))
                    else:
                        todo.append(f.target)
                elif k in ("call", "tail"):
                    fn.calls.add((f.target, f.tthumb))
                elif k == "jumpind":
                    fn.dynsites.append(a)
                if f.falls():
                    todo.append(a + f.size)
            # barrido tras cada salto calculado nuevo
            new = [d for d in fn.dynsites if d not in swept]
            if not new:
                break
            for d in new:
                swept.add(d)
                # en ARM un "bx rN" es un salto de cola por registro (funciones puente), nunca un
                # switch (esos son add/ldr pc): barrer detrás se tragaba el código Thumb de al lado
                if not thumb and "bx" not in VIEJO and (self.img.u32(d) & 0x0FFFFFF0) == 0x012FFF10:
                    continue
                # switch ARM con su tamaño a la vista (cmp rX,#n ... addls pc,pc,rX,lsl #2): solo se
                # miran sus n+1 casos. Barrer 4 KB detrás se metía en código Thumb, encontraba "saltos"
                # falsos y barría más, en cadena: 0x0211127C tapaba medio ARM9 (Contrarreloj con fantasma)
                if not thumb and "tabla" not in VIEJO:
                    casos = self.casos_switch_arm(d)
                    if casos:
                        for a in casos:
                            if self.img.has(a, 4):
                                fn.dyn_targets.add(a)
                                todo.append(a)
                        continue
                # el barrido salta los datos (el compilador de Nintendo pone bloques de
                # literales en mitad de funciones grandes) y solo lo corta una función segura
                limits = [x for (x, t) in (known if "barrido" in VIEJO else strong) if x > d]
                if "barrido" in VIEJO:
                    limits += [x for x in fn.literals if x > d]
                end = min(limits + [d + 0x1000])
                datos = set()
                for l in fn.literals:
                    datos.update((l, l + 2))
                # ...y donde empieza la función siguiente (push {..., lr}): un "case" nunca empieza así
                a = d + step
                while "barrido" not in VIEJO and a < end and self.img.has(a, 4):
                    if a not in datos:
                        if thumb and (self.img.u16(a) & 0xFF00) == 0xB500:
                            end = a
                            break
                        if not thumb and (self.img.u32(a) & 0xFFFF4000) == 0xE92D4000:
                            end = a
                            break
                    a += step
                a = d + step
                while a < end and self.img.has(a, step):
                    if a not in datos:
                        fn.dyn_targets.add(a)
                        todo.append(a)
                    a += step
        # "todos" (cualquier instrucción de la función como destino de un salto calculado) va desactivado:
        # junto con el barrido nuevo colgaba el ARM7 (27/10). Se activa poniendo "todos" en NUEVO.
        if fn.dynsites and "todos" in NUEVO:
            fn.dyn_targets |= set(fn.ins)
        return fn

    def casos_switch_arm(self, d):
        """Destinos de un switch ARM 'addls pc, pc, rX, lsl #2' (tabla de saltos detrás) o 'ldrls pc,
        [pc, rX, lsl #2]' (tabla de direcciones) si unas instrucciones antes hay 'cmp rX, #n'. Si no se
        reconoce, None (y se barre como siempre)."""
        img = self.img
        w = img.u32(d)
        rx = w & 0xF
        es_add = (w & 0xFFFFFFF0) == 0x908FF100  # addls pc, pc, rX, lsl #2
        es_ldr = (w & 0xFFFFFFF0) == 0x979FF100  # ldrls pc, [pc, rX, lsl #2]
        if not (es_add or es_ldr):
            return None
        n = None
        for k in range(1, 5):
            if not img.has(d - 4 * k, 4):
                break
            p = img.u32(d - 4 * k)
            if (p & 0xFFF0F000) == 0xE3500000 and ((p >> 16) & 0xF) == rx:  # cmp rX, #imm
                rot = ((p >> 8) & 0xF) * 2
                imm = p & 0xFF
                n = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                break
            if (p >> 12) & 0xF == rx and (p & 0x0C000000) == 0:  # algo cambia rX antes del cmp: no fiarse
                break
        if n is None or n > 1024:
            return None
        casos = [d + 4]  # sin el caso (rX > n): la instrucción siguiente
        if es_add:
            casos += [d + 8 + 4 * i for i in range(n + 1)]
        else:
            for i in range(n + 1):
                v = img.u32(d + 8 + 4 * i)
                if not v & 1 and img.has(v, 4):
                    casos.append(v)
        return casos

    def plausible(self, a, thumb):
        """¿Parece que en `a` empieza una función? (para punteros encontrados en datos)"""
        step = 2 if thumb else 4
        if not thumb and self.img.has(a, 4) and (self.img.u32(a) >> 28) != 0xE:
            return False
        for _ in range(64):
            if not self.img.has(a, step):
                return False
            f = thumb_flow(self.img, a) if thumb else arm_flow(self.img, a)
            if f.kind == "undef":
                return False
            if f.kind in ("ret", "jumpind", "tail", "branch") and not f.cond:
                return True
            a += f.size
        return True

    def pointer_scan(self):
        """Busca en todo el binario palabras que apunten a código (punteros a función)."""
        lits = set()
        for fn in self.funcs.values():
            lits |= fn.literals
        found = set()
        for s, d in self.img.segs:
            for off in range(0, len(d) - 3, 4):
                v = struct.unpack_from("<I", d, off)[0]
                key = (v & ~1, True) if v & 1 else (v, False)
                if not (v & 1) and v & 3:
                    continue
                if key in self.funcs or key[0] in lits or not self.img.has(key[0], 2):
                    continue
                if self.plausible(*key):
                    found.add(key)
        return found

    def prologue_scan(self):
        """Busca comienzos típicos de función (push {..., lr}) que aún no sean función:
        funciones a las que se llega de formas que no se ven. Se miran también los que
        quedaron dentro de otra función Thumb (un barrido tras un salto calculado puede
        "tragarse" las funciones de detrás); solo se saltan los que son código ARM."""
        dentro_arm = set()
        for (fa, ft), fn in self.funcs.items():
            if ft:
                continue
            for a, f in fn.ins.items():
                dentro_arm.add(a)
                dentro_arm.add(a + 2)
        found = set()
        for s, d in self.img.segs:
            for off in range(0, len(d) - 3, 2):
                a = s + off
                if a in dentro_arm:
                    continue
                hw = d[off] | d[off + 1] << 8
                if (hw & 0xFF00) == 0xB500:  # Thumb: push {..., lr}
                    if (a, True) not in self.funcs and self.plausible(a, True):
                        found.add((a, True))
                elif off % 4 == 0:
                    w = struct.unpack_from("<I", d, off)[0]
                    if (w & 0xFFFF4000) == 0xE92D4000 and (a, False) not in self.funcs \
                            and self.plausible(a, False):  # ARM: stmdb sp!, {..., lr}
                        found.add((a, False))
        return found

    def gap_scan(self):
        """Rellena huecos: tras cada trozo del binario que no es de ninguna función ni
        dato conocido, si lo que viene se decodifica bien, es una función (pequeñas
        funciones sin push lr a las que se llega por punteros calculados)."""
        cubierto = {}  # dirección -> thumb de la función que la cubre
        for (fa, ft), fn in self.funcs.items():
            for a, f in fn.ins.items():
                cubierto[a] = ft
                if f.size == 4:
                    cubierto[a + 2] = ft
            for l in fn.literals:
                cubierto[l] = ft
                cubierto[l + 2] = ft
        found = set()
        for s, d in self.img.segs:
            a, fin = s, s + len(d)
            modo = False
            while a < fin - 2:
                if a in cubierto:
                    modo = cubierto[a]
                    a += 2
                    continue
                # inicio de hueco: saltar relleno (ceros y nop)
                hw = self.img.u16(a)
                if hw in (0x0000, 0x46C0) or (not modo and a % 4):
                    a += 2
                    continue
                key = (a, modo)
                if key not in self.funcs and self.plausible(a, modo):
                    found.add(key)
                # avanzar hasta el final del hueco
                while a < fin - 2 and a not in cubierto:
                    a += 2
        return found

    def boundary_scan(self):
        """Tras cada instrucción que cierra un bloque (vuelta, salto sin condición), si lo
        que sigue no es destino de ningún salto de esa función, ahí empieza otra función."""
        found = set()
        for (fa, ft), fn in list(self.funcs.items()):
            destinos = {f.target for f in fn.ins.values() if f.kind == "branch" and not f.tail}
            for a, f in fn.ins.items():
                if f.falls() or f.kind == "undef":
                    continue
                n = a + f.size
                step = 2 if ft else 4
                for _ in range(4):  # saltar relleno (nop, ceros)
                    if not self.img.has(n, step):
                        break
                    v = self.img.u16(n) if ft else self.img.u32(n)
                    if v in ((0x46C0, 0x0000) if ft else (0xE1A00000, 0)):
                        n += step
                        continue
                    break
                if n in destinos or n in fn.literals or (n, ft) in self.funcs or not self.img.has(n, step):
                    continue
                if self.plausible(n, ft):
                    found.add((n, ft))
        return found

    def discover(self, log=print, scan=True, prologos=False):
        # Los punteros encontrados en datos pueden ser falsos (caer en mitad de una
        # función): se traducen, pero solo las entradas seguras (strong) convierten
        # un B en llamada de cola.
        strong = set(self.entries)
        known = set(self.entries) | self.debiles
        pending = list(known)
        scanned = not scan
        huecos = 0
        while True:  # primera pasada: encontrar todas las funciones
            while pending:
                key = pending.pop()
                if key in self.funcs:
                    continue
                if not self.img.has(key[0], 2):
                    self.fuera.add(key)
                    continue
                fn = self.explore(key, strong, known)
                self.funcs[key] = fn
                for k in fn.calls:
                    if key in strong or "debil" in VIEJO:  # lo que llama una función encontrada buscando no cuenta como seguro
                        strong.add(k)
                    if k not in known:
                        known.add(k)
                        pending.append(k)
            if scanned:
                if prologos:  # una vez acabado lo demás: comienzos de función sueltos
                    prologos = False
                    huecos = 3
                    extra = self.prologue_scan() - known
                    log(f"funciones encontradas por su comienzo (push lr): {len(extra)}")
                    known |= extra
                    pending = list(extra)
                    continue
                if huecos > 0:  # y después, rellenar los huecos que queden (varias veces)
                    huecos -= 1
                    extra = (self.gap_scan() | (set() if "fronteras" in VIEJO else self.boundary_scan())) - known
                    log(f"funciones encontradas en huecos y fronteras: {len(extra)}")
                    if extra:
                        known |= extra
                        pending = list(extra)
                        continue
                break
            scanned = True
            extra = self.pointer_scan() - known
            log(f"punteros a función encontrados en datos: {len(extra)}")
            known |= extra
            pending = list(extra)
        # segunda pasada: ahora que se conocen todas, los B a otra función son llamadas de cola
        for key in list(self.funcs):
            self.funcs[key] = self.explore(key, strong, known)
        n_ins = sum(len(f.ins) for f in self.funcs.values())
        log(f"{len(self.funcs)} funciones, {n_ins} instrucciones")

    # -- generación de C ----------------------------------------------------
    @staticmethod
    def _save(path, text):
        """Solo escribe si cambia, para que la compilación sea incremental."""
        if os.path.exists(path):
            with open(path) as f:
                if f.read() == text:
                    return
        with open(path, "w") as f:
            f.write(text)

    def write(self, outdir, per_file=400):
        os.makedirs(outdir, exist_ok=True)
        keys = sorted(self.funcs, key=lambda k: (k[0], k[1]))
        hdr = ['#pragma once\n#include "cpu.h"\n'] + (['#include "../mkds/funcs.h"\n'] if self.external else []) + [f"void {fname(*k, self.prefix)}(Cpu *c);\n" for k in keys]
        self._save(os.path.join(outdir, "funcs.h"), "".join(hdr))
        files = []
        for n in range(0, len(keys), per_file):
            name = f"funcs_{n // per_file:03d}.c"
            files.append(name)
            parts = ['#include "funcs.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n\n']
            for k in keys[n:n + per_file]:
                parts.append(Gen(self, self.funcs[k]).function() + "\n")
            self._save(os.path.join(outdir, name), "".join(parts))
        tipo = "FuncEntryOv" if self.firmas else "FuncEntry"
        tabla = [f'#include "funcs.h"\n\nconst {tipo} g_funcs{self.prefix}[] = {{\n']
        for k in sorted(keys, key=lambda k: k[0] | int(k[1])):
            firma = ""
            if self.firmas:  # los 4 primeros bytes, para saber si el overlay está cargado
                a = k[0]
                firma = ", " + h(self.img.u16(a) | self.img.u16(a + 2) << 16 if self.img.has(a, 4) else 0)
            tabla.append(f"    {{{h(k[0] | int(k[1]))}, {fname(*k, self.prefix)}{firma}}},\n")
        tabla.append(f"}};\nconst size_t g_nfuncs{self.prefix} = sizeof g_funcs{self.prefix} / sizeof g_funcs{self.prefix}[0];\n")
        self._save(os.path.join(outdir, "tabla.c"), "".join(tabla))
        for viejo in os.listdir(outdir):  # restos de traducciones anteriores con más archivos
            if viejo.startswith("funcs_") and viejo.endswith(".c") and viejo not in files:
                os.remove(os.path.join(outdir, viejo))
        return files + ["tabla.c"]


class Gen:
    """Escribe una función en C."""

    def __init__(self, rc, fn):
        self.rc = rc
        self.disp = rc.disp
        self.img = rc.img
        self.fn = fn
        self.thumb = fn.thumb

    def function(self):
        fn = self.fn
        hook = self.rc.hooks.get((fn.addr, fn.thumb))
        # "antes": el runtime mira antes y sigue el original; "si": si el runtime devuelve 1, se vuelve ya
        tipo_hook = hook[2] if hook and len(hook) > 2 else None
        if hook and not tipo_hook:  # sustituida por una función del runtime
            return (f"void {fname(fn.addr, fn.thumb, self.rc.prefix)}(Cpu *c) {{\n"
                    f"    c->func = {h(fn.addr | int(fn.thumb))};\n    {hook[0]}(c);\n}}\n")
        out = [f"void {fname(fn.addr, fn.thumb, self.rc.prefix)}(Cpu *c) {{",
               "    const uint32_t entry_lr = R[14];",
               "    (void)entry_lr;",
               "    uint32_t t_dyn; (void)t_dyn;",
               f"    c->func = {h(fn.addr | int(fn.thumb))};",
               "    " + TICK]
        if tipo_hook == "antes":
            out.append(f"    {hook[0]}(c);")
        elif tipo_hook == "si":
            out.append(f"    if ({hook[0]}(c)) return;")
        addrs = sorted(fn.ins)
        if addrs and addrs[0] != fn.addr:
            out.append(f"    goto L_{fn.addr:08X};")
        if fn.addr in fn.missing:
            out.append(f"    cpu_missing(c, {h(fn.addr)});")
        for idx, a in enumerate(addrs):
            f = fn.ins[a]
            out.append(f"L_{a:08X}:;")
            body = self.thumb_ins(a, f) if self.thumb else self.arm_ins(a, f)
            out += ["    " + l for l in body]
            if f.falls():
                nxt = a + f.size
                if idx + 1 >= len(addrs) or addrs[idx + 1] != nxt:
                    if nxt in fn.ins:
                        out.append(f"    goto L_{nxt:08X};")
                    else:
                        out.append(f"    cpu_missing(c, {h(nxt)});")
        targets = sorted(fn.dyn_targets)
        if targets:  # saltos calculados dentro de la función (switch): una sola tabla
            bit = 1 if self.thumb else 0
            out.append("L_dyn:")
            out.append("    switch (t_dyn) {")
            for a in targets:
                out.append(f"    case {h(a | bit)}: goto L_{a:08X};")
            out.append("    }")
            out.append(f"    {self.disp}(c, t_dyn); return;")
        out.append("}")
        return "\n".join(out) + "\n"

    # -- saltos -------------------------------------------------------------
    def nombre(self, addr, thumb):
        """Función C para un destino conocido; None si hay que resolverlo al ejecutar
        (fuera de la imagen: un overlay, o el código fijo desde un overlay sin traducir)."""
        if (addr, thumb) in self.rc.funcs:
            return fname(addr, thumb, self.rc.prefix)
        if (addr, thumb) in self.rc.external:
            return fname(addr, thumb)
        return None

    def branch(self, f, a):
        if not f.tail and f.target in self.fn.ins:
            if f.target <= a:  # bucle: dar ocasión a interrupciones y al otro procesador
                return [TICK, f"goto L_{f.target:08X};"]
            return [f"goto L_{f.target:08X};"]
        n = self.nombre(f.target, self.thumb)
        if n:
            return [f"{n}(c); return;"]
        return [f"{self.disp}(c, {h(f.target | int(self.thumb))}); return;"]

    def call(self, f, ret):
        name = self.nombre(f.target, f.tthumb)
        if not name:
            return [f"R[14] = {h(ret)};", f"{self.disp}(c, {h(f.target | int(f.tthumb))});",
                    f"c->func = {h(self.fn.addr | int(self.thumb))};"]
        hook = self.rc.hooks.get((f.target, f.tthumb))
        if hook and hook[1]:
            # guardar contexto: aquí se volverá (longjmp) cuando se cargue ese contexto
            return [f"R[14] = {h(ret)};",
                    f"if (__builtin_setjmp(hle_ctx_buf(c, R[0])) == 0) {name}(c);",
                    f"c->func = {h(self.fn.addr | int(self.thumb))};"]
        return [f"R[14] = {h(ret)};", f"{name}(c);",
                f"c->func = {h(self.fn.addr | int(self.thumb))};"]

    def tail(self, f):
        n = self.nombre(f.target, f.tthumb)
        if not n:
            return [f"{self.disp}(c, {h(f.target | int(f.tthumb))}); return;"]
        return [f"{n}(c); return;"]

    def dyn_jump(self, expr, interwork):
        """Salto a una dirección calculada."""
        if interwork:
            key = "t"
        elif self.thumb:
            key = "(t | 1u)"
        else:
            key = "(t & ~3u)"
        lines = ["{", f"    uint32_t t = {expr};",
                 "    if (((t ^ entry_lr) & ~1u) == 0) return;"]
        if self.fn.dyn_targets:  # la tabla de destinos está una sola vez, en L_dyn
            lines += [f"    t_dyn = {key}; goto L_dyn;", "}"]
            return lines
        lines += [f"    {self.disp}(c, {key}); return;", "}"]
        return lines

    def dyn_call(self, expr, ret, interwork=True):
        key = "t" if interwork else ("(t | 1u)" if self.thumb else "(t & ~3u)")
        return ["{", f"    uint32_t t = {expr};", f"    R[14] = {h(ret)};",
                f"    {self.disp}(c, {key});", "}"]

    def pc_write(self, a, f, expr, interwork):
        """Escritura en pc de una instrucción normal (mov pc, ldr pc, pop pc...)."""
        if f.kind == "callind":  # mov lr, pc ya puso la vuelta
            return ["{", f"    uint32_t t = {expr};",
                    f"    {self.disp}(c, {'t' if interwork else '(t & ~3u)'});", "}"]
        if f.kind == "tail":
            return [f"(void)({expr});"] + self.tail(f)
        if f.kind == "call":  # mov lr, pc + ldr pc, =función
            n = self.nombre(f.target, f.tthumb)
            return [f"(void)({expr});", f"{n}(c);" if n else f"{self.disp}(c, {h(f.target | int(f.tthumb))});"]
        return self.dyn_jump(expr, interwork)

    # -- ARM ----------------------------------------------------------------
    def con_parche(self, a, lineas):
        p = self.rc.parches.get(a)
        if not p:
            return lineas
        cond, codigo = p
        return [f"if ({cond}) {{", f"    {codigo}", "} else {"] + ["    " + l for l in lineas] + ["}"]

    def arm_ins(self, a, f):
        return self.con_parche(a, self.arm_ins_(a, f))

    def thumb_ins(self, a, f):
        return self.con_parche(a, self.thumb_ins_(a, f))

    def arm_ins_(self, a, f):
        i = self.img.u32(a)
        cond = i >> 28
        if cond == 0xF:
            if f.kind == "call":
                return self.call(f, a + 4)
            if f.kind == "undef":
                return [f"cpu_undefined(c, {h(a)}, {h(i)});"]
            return ["/* pld */"]
        body = self.arm_body(a, i, f)
        if cond == 0xE:
            return body
        return [f"if ({COND[cond]}) {{"] + ["    " + l for l in body] + ["}"]

    def reg(self, r, pc):
        return h(pc) if r == 15 else f"R[{r}]"

    def shift_imm(self, i, a, pcv):
        """Operando "registro desplazado por inmediato" -> (expresión, carry o None)."""
        rm, st, n = i & 15, (i >> 5) & 3, (i >> 7) & 31
        m = self.reg(rm, pcv)
        if st == 0:
            return (m, None) if n == 0 else (f"({m} << {n})", f"(({m} >> {32 - n}) & 1)")
        if st == 1:
            return ("0u", f"({m} >> 31)") if n == 0 else (f"({m} >> {n})", f"(({m} >> {n - 1}) & 1)")
        if st == 2:
            if n == 0:
                return f"(uint32_t)((int32_t){m} >> 31)", f"({m} >> 31)"
            return f"(uint32_t)((int32_t){m} >> {n})", f"(({m} >> {n - 1}) & 1)"
        if n == 0:
            return f"((c->c << 31) | ({m} >> 1))", f"({m} & 1)"
        return f"ror32({m}, {n})", f"(({m} >> {n - 1}) & 1)"

    def arm_body(self, a, i, f):
        cls = arm_class(i)
        return getattr(self, "arm_" + cls)(a, i, f)

    def arm_dp(self, a, i, f):
        opc, s = (i >> 21) & 15, (i >> 20) & 1
        rn, rd = (i >> 16) & 15, (i >> 12) & 15
        regshift = not (i >> 25) & 1 and (i >> 4) & 1
        pcv = a + 12 if regshift else a + 8
        logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
        L = ["{"]
        cy = None
        if (i >> 25) & 1:
            rot = ((i >> 8) & 15) * 2
            imm = ror(i & 0xFF, rot)
            op2 = h(imm)
            if rot:
                cy = str(imm >> 31)
        elif regshift:
            fnm = ["lsl_c", "lsr_c", "asr_c", "ror_c"][(i >> 5) & 3]
            L.append("    uint32_t cy = c->c;")
            L.append(f"    uint32_t op2 = {fnm}({self.reg(i & 15, pcv)}, "
                     f"{self.reg((i >> 8) & 15, pcv)} & 0xFF, &cy);")
            op2, cy = "op2", "cy"
        else:
            e, cye = self.shift_imm(i, a, pcv)
            if s and logical and cye is not None:
                L.append(f"    uint32_t cy = {cye};")
                cy = "cy"
            L.append(f"    uint32_t op2 = {e};")
            op2 = "op2"
        n = self.reg(rn, pcv)
        plain = {0: f"{n} & {op2}", 1: f"{n} ^ {op2}", 2: f"{n} - {op2}", 3: f"{op2} - {n}",
                 4: f"{n} + {op2}", 5: f"{n} + {op2} + c->c", 6: f"{n} - {op2} - (c->c ^ 1)",
                 7: f"{op2} - {n} - (c->c ^ 1)", 8: f"{n} & {op2}", 9: f"{n} ^ {op2}",
                 12: f"{n} | {op2}", 13: op2, 14: f"{n} & ~{op2}", 15: f"~{op2}"}
        flag = {2: f"sub_f(c, {n}, {op2})", 3: f"sub_f(c, {op2}, {n})", 4: f"add_f(c, {n}, {op2})",
                5: f"adc_f(c, {n}, {op2})", 6: f"sbc_f(c, {n}, {op2})", 7: f"sbc_f(c, {op2}, {n})",
                10: f"sub_f(c, {n}, {op2})", 11: f"add_f(c, {n}, {op2})"}
        if 8 <= opc <= 11:  # TST TEQ CMP CMN: solo banderas
            if logical:
                L.append(f"    nz(c, {plain[opc]});")
                if cy is not None:
                    L.append(f"    c->c = {cy};")
            else:
                L.append(f"    {flag[opc]};")
            return L + ["}"]
        if s and logical:
            L.append(f"    uint32_t res = nz(c, {plain[opc]});")
            if cy is not None:
                L.append(f"    c->c = {cy};")
        elif s:
            L.append(f"    uint32_t res = {flag[opc]};")
        else:
            L.append(f"    uint32_t res = {plain[opc]};")
        if rd == 15:
            if s:
                L.append("    cpu_restore_spsr(c);")
            L += ["    " + l for l in self.pc_write(a, f, "res", False)]
        else:
            L.append(f"    R[{rd}] = res;")
        return L + ["}"]

    def arm_sdt(self, a, i, f):
        I, P, U = (i >> 25) & 1, (i >> 24) & 1, (i >> 23) & 1
        B, W, ld = (i >> 22) & 1, (i >> 21) & 1, (i >> 20) & 1
        rn, rd = (i >> 16) & 15, (i >> 12) & 15
        off = self.shift_imm(i, a, a + 8)[0] if I else h(i & 0xFFF)
        sign = "+" if U else "-"
        L = ["{", f"    uint32_t base = {self.reg(rn, a + 8)};", f"    uint32_t off = {off};",
             f"    uint32_t ad = {'base ' + sign + ' off' if P else 'base'};"]
        wb = (not P) or W
        if ld:
            L.append(f"    uint32_t v = {'rd8(ad)' if B else 'ldr32(ad)'};")
            if wb and rn != rd:
                L.append(f"    R[{rn}] = base {sign} off;")
            if rd == 15:
                L += ["    " + l for l in self.pc_write(a, f, "v", self.rc.v5)]
            else:
                L.append(f"    R[{rd}] = v;")
        else:
            L.append(f"    {'wr8' if B else 'wr32'}(ad, {self.reg(rd, a + 12)});")
            if wb:
                L.append(f"    R[{rn}] = base {sign} off;")
        return L + ["}"]

    def arm_hw(self, a, i, f):
        P, U, imm = (i >> 24) & 1, (i >> 23) & 1, (i >> 22) & 1
        W, ld = (i >> 21) & 1, (i >> 20) & 1
        rn, rd, sh = (i >> 16) & 15, (i >> 12) & 15, (i >> 5) & 3
        off = h(((i >> 4) & 0xF0) | (i & 0xF)) if imm else self.reg(i & 15, a + 8)
        sign = "+" if U else "-"
        L = ["{", f"    uint32_t base = {self.reg(rn, a + 8)};", f"    uint32_t off = {off};",
             f"    uint32_t ad = {'base ' + sign + ' off' if P else 'base'};"]
        wb = (not P) or W
        wbl = f"    R[{rn}] = base {sign} off;"
        if ld:
            load = {1: "rd16(ad)", 2: "(uint32_t)(int32_t)(int8_t)rd8(ad)",
                    3: "(uint32_t)(int32_t)(int16_t)rd16(ad)"}[sh]
            L.append(f"    uint32_t v = {load};")
            if wb and rn != rd:
                L.append(wbl)
            L.append(f"    R[{rd}] = v;")
        elif sh == 1:
            L.append(f"    wr16(ad, {self.reg(rd, a + 12)});")
            if wb:
                L.append(wbl)
        elif sh == 2:  # LDRD
            L += ["    uint32_t v0 = rd32(ad), v1 = rd32(ad + 4);"]
            if wb:
                L.append(wbl)
            L += [f"    R[{rd}] = v0;", f"    R[{rd + 1}] = v1;"]
        else:  # STRD
            L += [f"    wr32(ad, R[{rd}]);", f"    wr32(ad + 4, R[{rd + 1}]);"]
            if wb:
                L.append(wbl)
        return L + ["}"]

    def arm_ldm(self, a, i, f):
        P, U, S, W, ld = (i >> 24) & 1, (i >> 23) & 1, (i >> 22) & 1, (i >> 21) & 1, (i >> 20) & 1
        rn = (i >> 16) & 15
        regs = [r for r in range(16) if (i >> r) & 1]
        n = len(regs)
        if n == 0:
            return [f"cpu_undefined(c, {h(a)}, {h(i)});"]
        start = {(0, 1): "base", (1, 1): "base + 4", (0, 0): f"base - {4 * n - 4}",
                 (1, 0): f"base - {4 * n}"}[(P, U)]
        newb = f"base + {4 * n}" if U else f"base - {4 * n}"
        L = ["{", f"    uint32_t base = R[{rn}];", f"    uint32_t ad = {start};"]
        usr = S and not (ld and 15 in regs)  # "^": registros del modo usuario

        def rg(r):
            return f"*cpu_usr(c, {r})" if usr and r >= 8 else f"R[{r}]"
        if ld:
            for k, r in enumerate(regs):
                adr = "ad" if k == 0 else f"ad + {4 * k}"
                L.append(f"    uint32_t npc = rd32({adr});" if r == 15 else f"    {rg(r)} = rd32({adr});")
            if W and not (rn in regs and regs[-1] == rn and n > 1):
                L.append(f"    R[{rn}] = {newb};")
            if 15 in regs:
                if S:
                    L.append("    cpu_restore_spsr(c);")
                L += ["    " + l for l in self.pc_write(a, f, "npc", self.rc.v5)]
        else:
            for k, r in enumerate(regs):
                adr = "ad" if k == 0 else f"ad + {4 * k}"
                if r == 15:
                    val = h(a + 12)
                elif r == rn:
                    val = "base" if k == 0 else f"({newb})"
                else:
                    val = rg(r)
                L.append(f"    wr32({adr}, {val});")
            if W:
                L.append(f"    R[{rn}] = {newb};")
        return L + ["}"]

    def arm_b(self, a, i, f):
        if f.kind == "call":
            return self.call(f, a + 4)
        return self.branch(f, a)

    def arm_bx(self, a, i, f):
        rm = i & 15
        if f.kind == "tail":
            return self.tail(f)
        if f.kind == "callind":  # mov lr, pc ya puso la vuelta
            return ["{", f"    uint32_t t = R[{rm}];", f"    {self.disp}(c, t);", "}"]
        return self.dyn_jump(f"R[{rm}]", True)

    def arm_blxr(self, a, i, f):
        return self.dyn_call(f"R[{i & 15}]", a + 4)

    def arm_clz(self, a, i, f):
        return [f"R[{(i >> 12) & 15}] = clz32(R[{i & 15}]);"]

    def arm_qarith(self, a, i, f):
        op = (i >> 21) & 3
        rn, rd, rm = (i >> 16) & 15, (i >> 12) & 15, i & 15
        e = {0: f"qadd(c, R[{rm}], R[{rn}])", 1: f"qsub(c, R[{rm}], R[{rn}])",
             2: f"qadd(c, R[{rm}], qadd(c, R[{rn}], R[{rn}]))",
             3: f"qsub(c, R[{rm}], qadd(c, R[{rn}], R[{rn}]))"}[op]
        return [f"R[{rd}] = {e};"]

    def arm_smulxy(self, a, i, f):
        op = (i >> 21) & 3
        rd, rn, rs, rm = (i >> 16) & 15, (i >> 12) & 15, (i >> 8) & 15, i & 15
        x, y = (i >> 5) & 1, (i >> 6) & 1

        def half(r, top):
            return f"(int32_t)(int16_t)(R[{r}] >> 16)" if top else f"(int32_t)(int16_t)R[{r}]"
        if op == 0:  # SMLAxy (la bandera Q no se usa en juegos)
            return [f"R[{rd}] = (uint32_t)({half(rm, x)} * {half(rs, y)}) + R[{rn}];"]
        if op == 1:
            p = f"(uint32_t)(((int64_t)(int32_t)R[{rm}] * {half(rs, y)}) >> 16)"
            return [f"R[{rd}] = {p};"] if x else [f"R[{rd}] = {p} + R[{rn}];"]
        if op == 2:  # SMLALxy: RdHi = rd, RdLo = rn
            return ["{", f"    uint64_t acc = ((uint64_t)R[{rd}] << 32) | R[{rn}];",
                    f"    acc += (uint64_t)(int64_t)({half(rm, x)} * {half(rs, y)});",
                    f"    R[{rn}] = (uint32_t)acc; R[{rd}] = (uint32_t)(acc >> 32);", "}"]
        return [f"R[{rd}] = (uint32_t)({half(rm, x)} * {half(rs, y)});"]

    def arm_swp(self, a, i, f):
        rn, rd, rm, B = (i >> 16) & 15, (i >> 12) & 15, i & 15, (i >> 22) & 1
        return ["{", f"    uint32_t ad = R[{rn}];",
                f"    uint32_t v = {'rd8(ad)' if B else 'ldr32(ad)'};",
                f"    {'wr8' if B else 'wr32'}(ad, R[{rm}]);", f"    R[{rd}] = v;", "}"]

    def arm_mul(self, a, i, f):
        A, S = (i >> 21) & 1, (i >> 20) & 1
        rd, rn, rs, rm = (i >> 16) & 15, (i >> 12) & 15, (i >> 8) & 15, i & 15
        e = f"R[{rm}] * R[{rs}]" + (f" + R[{rn}]" if A else "")
        return [f"R[{rd}] = nz(c, {e});" if S else f"R[{rd}] = {e};"]

    def arm_mull(self, a, i, f):
        sgn, A, S = (i >> 22) & 1, (i >> 21) & 1, (i >> 20) & 1
        hi, lo, rs, rm = (i >> 16) & 15, (i >> 12) & 15, (i >> 8) & 15, i & 15
        L = ["{"]
        if sgn:
            L.append(f"    uint64_t p = (uint64_t)((int64_t)(int32_t)R[{rm}] * (int64_t)(int32_t)R[{rs}]);")
        else:
            L.append(f"    uint64_t p = (uint64_t)R[{rm}] * R[{rs}];")
        if A:
            L.append(f"    p += ((uint64_t)R[{hi}] << 32) | R[{lo}];")
        L.append(f"    R[{lo}] = (uint32_t)p; R[{hi}] = (uint32_t)(p >> 32);")
        if S:
            L.append("    c->n = (uint32_t)(p >> 63); c->z = p == 0;")
        return L + ["}"]

    def arm_mrs(self, a, i, f):
        rd = (i >> 12) & 15
        return [f"R[{rd}] = {'cpu_get_spsr(c)' if (i >> 22) & 1 else 'cpu_get_cpsr(c)'};"]

    def arm_msr(self, a, i, f):
        val = h(ror(i & 0xFF, ((i >> 8) & 15) * 2)) if (i >> 25) & 1 else f"R[{i & 15}]"
        return [f"cpu_msr(c, {(i >> 22) & 1}, {(i >> 16) & 15}, {val});"]

    def arm_mcr(self, a, i, f):
        if ((i >> 8) & 15) != 15:
            return [f"/* coprocesador {(i >> 8) & 15} */"]
        op1, crn, rd = (i >> 21) & 7, (i >> 16) & 15, (i >> 12) & 15
        crm, op2 = i & 15, (i >> 5) & 7
        if (i >> 20) & 1:
            if rd == 15:
                return [f"(void)cp15_read(c, {crn}, {crm}, {op1}, {op2});"]
            return [f"R[{rd}] = cp15_read(c, {crn}, {crm}, {op1}, {op2});"]
        return [f"cp15_write(c, {crn}, {crm}, {op1}, {op2}, {self.reg(rd, a + 12)});"]

    def arm_cop(self, a, i, f):
        return ["/* coprocesador */"]

    def arm_swi(self, a, i, f):
        return [f"bios_swi(c, {h((i >> 16) & 0xFF)});"]

    def arm_undef(self, a, i, f):
        return [f"cpu_undefined(c, {h(a)}, {h(i)});"]

    # -- Thumb --------------------------------------------------------------
    def thumb_ins_(self, a, f):
        i = self.img.u16(a)
        if f.kind == "undef":
            return [f"cpu_undefined(c, {h(a)}, {h(i)});"]
        pc = a + 4
        top = i >> 13
        if i < 0x1800:  # desplazamiento inmediato
            op, off, rs, rd = (i >> 11) & 3, (i >> 6) & 31, (i >> 3) & 7, i & 7
            if op == 0:
                if off == 0:
                    return [f"R[{rd}] = nz(c, R[{rs}]);"]
                return ["{", f"    uint32_t v = R[{rs}];", f"    c->c = (v >> {32 - off}) & 1;",
                        f"    R[{rd}] = nz(c, v << {off});", "}"]
            if op == 1:
                if off == 0:
                    return ["{", f"    c->c = R[{rs}] >> 31;", f"    R[{rd}] = nz(c, 0);", "}"]
                return ["{", f"    uint32_t v = R[{rs}];", f"    c->c = (v >> {off - 1}) & 1;",
                        f"    R[{rd}] = nz(c, v >> {off});", "}"]
            n = 32 if off == 0 else off
            return ["{", f"    uint32_t v = R[{rs}];",
                    f"    c->c = ((uint32_t)((int32_t)v >> {n - 1})) & 1;",
                    f"    R[{rd}] = nz(c, (uint32_t)((int32_t)v >> {min(n, 31)}));", "}"]
        if i < 0x2000:  # add/sub
            I, sub, rn, rs, rd = (i >> 10) & 1, (i >> 9) & 1, (i >> 6) & 7, (i >> 3) & 7, i & 7
            opnd = h(rn) if I else f"R[{rn}]"
            return [f"R[{rd}] = {'sub_f' if sub else 'add_f'}(c, R[{rs}], {opnd});"]
        if top == 1:  # mov/cmp/add/sub inmediato
            op, rd, imm = (i >> 11) & 3, (i >> 8) & 7, i & 0xFF
            return [[f"R[{rd}] = nz(c, {h(imm)});", f"sub_f(c, R[{rd}], {h(imm)});",
                     f"R[{rd}] = add_f(c, R[{rd}], {h(imm)});",
                     f"R[{rd}] = sub_f(c, R[{rd}], {h(imm)});"][op]]
        if i < 0x4400:  # ALU
            op, rs, rd = (i >> 6) & 15, (i >> 3) & 7, i & 7
            d, s = f"R[{rd}]", f"R[{rs}]"
            if op in (2, 3, 4, 7):
                fnm = {2: "lsl_c", 3: "lsr_c", 4: "asr_c", 7: "ror_c"}[op]
                return ["{", "    uint32_t cy = c->c;", f"    {d} = nz(c, {fnm}({d}, {s} & 0xFF, &cy));",
                        "    c->c = cy;", "}"]
            return [{0: f"{d} = nz(c, {d} & {s});", 1: f"{d} = nz(c, {d} ^ {s});",
                     5: f"{d} = adc_f(c, {d}, {s});", 6: f"{d} = sbc_f(c, {d}, {s});",
                     8: f"nz(c, {d} & {s});", 9: f"{d} = sub_f(c, 0, {s});",
                     10: f"sub_f(c, {d}, {s});", 11: f"add_f(c, {d}, {s});",
                     12: f"{d} = nz(c, {d} | {s});", 13: f"{d} = nz(c, {d} * {s});",
                     14: f"{d} = nz(c, {d} & ~{s});", 15: f"{d} = nz(c, ~{s});"}[op]]
        if i < 0x4800:  # registros altos, BX
            op = (i >> 8) & 3
            rs = ((i >> 3) & 7) | ((i >> 3) & 8)
            rd = (i & 7) | ((i >> 4) & 8)
            S = self.reg(rs, pc)
            if op == 0:
                if rd == 15:
                    return self.dyn_jump(f"{h(pc)} + {S}", False)
                return [f"R[{rd}] = R[{rd}] + {S};"]
            if op == 1:
                return [f"sub_f(c, {self.reg(rd, pc)}, {S});"]
            if op == 2:
                if rd == 15:
                    return self.dyn_jump(S, False)
                return [f"R[{rd}] = {S};"]
            if i & 0x80:
                return self.dyn_call(S, (a + 2) | 1)
            if f.kind == "tail":
                return self.tail(f)
            return self.dyn_jump(S, True)
        if i < 0x5000:  # ldr rd, [pc, #x]
            return [f"R[{(i >> 8) & 7}] = rd32({h(f.literal)});"]
        if i < 0x6000:  # carga/guarda con registro
            op, ro, rb, rd = (i >> 9) & 7, (i >> 6) & 7, (i >> 3) & 7, i & 7
            ad = f"R[{rb}] + R[{ro}]"
            return [{0: f"wr32({ad}, R[{rd}]);", 1: f"wr16({ad}, R[{rd}]);", 2: f"wr8({ad}, R[{rd}]);",
                     3: f"R[{rd}] = (uint32_t)(int32_t)(int8_t)rd8({ad});",
                     4: f"R[{rd}] = ldr32({ad});", 5: f"R[{rd}] = rd16({ad});",
                     6: f"R[{rd}] = rd8({ad});",
                     7: f"R[{rd}] = (uint32_t)(int32_t)(int16_t)rd16({ad});"}[op]]
        if i < 0x8000:  # carga/guarda inmediato
            B, ld, off, rb, rd = (i >> 12) & 1, (i >> 11) & 1, (i >> 6) & 31, (i >> 3) & 7, i & 7
            ad = f"R[{rb}] + {off if B else off * 4}"
            if ld:
                return [f"R[{rd}] = {'rd8' if B else 'ldr32'}({ad});"]
            return [f"{'wr8' if B else 'wr32'}({ad}, R[{rd}]);"]
        if i < 0x9000:  # media palabra inmediato
            ld, off, rb, rd = (i >> 11) & 1, (i >> 6) & 31, (i >> 3) & 7, i & 7
            ad = f"R[{rb}] + {off * 2}"
            return [f"R[{rd}] = rd16({ad});"] if ld else [f"wr16({ad}, R[{rd}]);"]
        if i < 0xA000:  # relativo a sp
            ld, rd, imm = (i >> 11) & 1, (i >> 8) & 7, (i & 0xFF) * 4
            return [f"R[{rd}] = ldr32(R[13] + {imm});"] if ld else [f"wr32(R[13] + {imm}, R[{rd}]);"]
        if i < 0xB000:  # add rd, pc/sp, #x
            rd, imm = (i >> 8) & 7, (i & 0xFF) * 4
            if (i >> 11) & 1:
                return [f"R[{rd}] = R[13] + {imm};"]
            return [f"R[{rd}] = {h(((a + 4) & ~3) + imm)};"]
        if i < 0xC000:
            if (i & 0xFF00) == 0xB000:
                imm = (i & 0x7F) * 4
                return [f"R[13] {'-' if i & 0x80 else '+'}= {imm};"]
            if (i & 0xF600) == 0xB400:
                ld, extra = (i >> 11) & 1, (i >> 8) & 1
                regs = [r for r in range(8) if (i >> r) & 1]
                if extra:
                    regs.append(15 if ld else 14)
                n = len(regs)
                if ld:
                    L = ["{", "    uint32_t ad = R[13];"]
                    for k, r in enumerate(regs):
                        L.append(f"    uint32_t npc = rd32(ad + {4 * k});" if r == 15
                                 else f"    R[{r}] = rd32(ad + {4 * k});")
                    L.append(f"    R[13] = ad + {4 * n};")
                    if 15 in regs:
                        L += ["    " + l for l in self.dyn_jump("npc", self.rc.v5)]
                    return L + ["}"]
                L = ["{", f"    uint32_t ad = R[13] - {4 * n};"]
                for k, r in enumerate(regs):
                    L.append(f"    wr32(ad + {4 * k}, R[{r}]);")
                return L + ["    R[13] = ad;", "}"]
            return [f"cpu_undefined(c, {h(a)}, {h(i)});"]
        if i < 0xD000:  # ldmia/stmia
            ld, rb = (i >> 11) & 1, (i >> 8) & 7
            regs = [r for r in range(8) if (i >> r) & 1]
            n = len(regs)
            L = ["{", f"    uint32_t ad = R[{rb}];"]
            if ld:
                for k, r in enumerate(regs):
                    L.append(f"    R[{r}] = rd32(ad + {4 * k});")
                if rb not in regs:
                    L.append(f"    R[{rb}] = ad + {4 * n};")
            else:
                for k, r in enumerate(regs):
                    val = ("ad" if k == 0 else f"(ad + {4 * n})") if r == rb else f"R[{r}]"
                    L.append(f"    wr32(ad + {4 * k}, {val});")
                L.append(f"    R[{rb}] = ad + {4 * n};")
            return L + ["}"]
        if i < 0xE000:
            cond = (i >> 8) & 15
            if cond == 0xF:
                return [f"bios_swi(c, {h(i & 0xFF)});"]
            return [f"if ({COND[cond]}) {{"] + ["    " + l for l in self.branch(f, a)] + ["}"]
        if i < 0xE800:
            return self.branch(f, a)
        if f.kind == "call":
            return self.call(f, (a + 4) | 1)
        return [f"cpu_undefined(c, {h(a)}, {h(i)});"]


def parse_hex(s):
    return int(s, 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seg", action="append", required=True,
                    help="DIRECCION:archivo  (código cargado en esa dirección)")
    ap.add_argument("--entry", action="append", required=True,
                    help="dirección de entrada en hex; T:dirección para Thumb")
    ap.add_argument("--out", required=True)
    ap.add_argument("--extra", help="archivo faltan.txt con más entradas (una por línea)")
    args = ap.parse_args()
    if args.extra and os.path.exists(args.extra):
        with open(args.extra) as f:
            args.entry += [l.strip() for l in f if l.strip()]
    img = Image()
    for s in args.seg:
        addr, path = s.split(":", 1)
        with open(path, "rb") as f:
            img.add(parse_hex(addr), f.read())
    entries = []
    for e in args.entry:
        thumb = e.startswith("T:")
        entries.append((parse_hex(e[2:] if thumb else e), thumb))
    rc = Recomp(img, entries)
    rc.discover()
    files = rc.write(args.out)
    print(f"escrito en {args.out}: {len(files)} archivos .c")


if __name__ == "__main__":
    main()
