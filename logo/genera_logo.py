"""Genera la geometría del logo de DS Kart Recompiled: una rueda de kart vista de frente cuyo neumático son dos
flechas en círculo (como el icono de "volver a cargar": el juego, vuelto a montar).
Escribe en esta carpeta logo.svg, logo_xaml.txt (el mismo dibujo para WPF) y logo_xaml_pequeno.txt (versión
sin detalles para iconos de 32 píxeles o menos).
    python logo/genera_logo.py
"""
import math
import os

C = 256.0          # centro (lienzo de 512)
R = 182.0          # radio medio del neumático
GROSOR = 58.0      # grosor del neumático
ROJO, AMARILLO, BLANCO, NOCHE, NOCHE2 = "#FF4D3D", "#FFC940", "#F3F4FF", "#12152A", "#232A57"
ROJO_OSC, AMARILLO_OSC = "#C9322A", "#D99A22"  # muescas del neumático


def p(ang, r):
    a = math.radians(ang)
    return C + r * math.cos(a), C + r * math.sin(a)


def f(x):
    return f"{x:.1f}"


def flecha(a0, a1):
    """Arco grueso de a0 a a1 grados (sentido horario en pantalla) acabado en punta de flecha."""
    ri, re = R - GROSOR / 2, R + GROSOR / 2
    punta = a1 + 24                      # la punta sobresale del arco
    ancho = 30                           # cuánto sale la punta por cada lado del neumático
    x0e, y0e = p(a0, re)
    x1e, y1e = p(a1, re)
    x1i, y1i = p(a1, ri)
    x0i, y0i = p(a0, ri)
    bxe, bye = p(a1, re + ancho)
    bxi, byi = p(a1, ri - ancho)
    tx, ty = p(punta, R)
    grande = 1 if (a1 - a0) > 180 else 0
    return (f"M{f(x0e)},{f(y0e)} A{f(re)},{f(re)} 0 {grande} 1 {f(x1e)},{f(y1e)} "
            f"L{f(bxe)},{f(bye)} L{f(tx)},{f(ty)} L{f(bxi)},{f(byi)} L{f(x1i)},{f(y1i)} "
            f"A{f(ri)},{f(ri)} 0 {grande} 0 {f(x0i)},{f(y0i)} Z")


def muescas(a0, a1, n):
    """Dibujo de la banda de rodadura: rayas cortas a lo ancho del neumático."""
    d = []
    for k in range(n):
        a = a0 + (a1 - a0) * (k + 0.5) / n
        x0, y0 = p(a - 3.5, R - GROSOR * 0.30)
        x1, y1 = p(a + 3.5, R - GROSOR * 0.30)
        x2, y2 = p(a + 3.5, R + GROSOR * 0.30)
        x3, y3 = p(a - 3.5, R + GROSOR * 0.30)
        d.append(f"M{f(x0)},{f(y0)} L{f(x1)},{f(y1)} L{f(x2)},{f(y2)} L{f(x3)},{f(y3)} Z")
    return " ".join(d)


def radios(n, r0, r1, ancho_grados):
    """Radios anchos de llanta de kart: se ensanchan hacia fuera."""
    d = []
    for k in range(n):
        a = -90 + 360 * k / n
        x0, y0 = p(a - ancho_grados * 1.4, r0)
        x1, y1 = p(a + ancho_grados * 1.4, r0)
        x2, y2 = p(a + ancho_grados, r1)
        x3, y3 = p(a - ancho_grados, r1)
        d.append(f"M{f(x0)},{f(y0)} L{f(x1)},{f(y1)} L{f(x2)},{f(y2)} L{f(x3)},{f(y3)} Z")
    return " ".join(d)


def cuadros(r):
    """Tapacubos a cuadros: dos cuartos oscuros de un círculo blanco."""
    x0, y0 = p(-90, r)
    x1, y1 = p(0, r)
    x2, y2 = p(90, r)
    x3, y3 = p(180, r)
    return (f"M{f(C)},{f(C)} L{f(x0)},{f(y0)} A{f(r)},{f(r)} 0 0 1 {f(x1)},{f(y1)} Z "
            f"M{f(C)},{f(C)} L{f(x2)},{f(y2)} A{f(r)},{f(r)} 0 0 1 {f(x3)},{f(y3)} Z")


def circulo(r):
    return f"M{f(C - r)},{f(C)} A{f(r)},{f(r)} 0 1 1 {f(C + r)},{f(C)} A{f(r)},{f(r)} 0 1 1 {f(C - r)},{f(C)} Z"


# dos flechas de 130 grados con su hueco: arriba la roja, abajo la amarilla
capas = [
    (NOCHE2, circulo(R + GROSOR / 2 + 6)),               # fondo detrás del neumático
    (ROJO, flecha(-160, -30)),
    (AMARILLO, flecha(20, 150)),
    (ROJO_OSC, muescas(-150, -40, 5)),
    (AMARILLO_OSC, muescas(30, 140, 5)),
    (BLANCO, circulo(122)),                              # llanta: aro grueso y cinco radios anchos
    (NOCHE2, circulo(92)),
    (BLANCO, radios(5, 34, 96, 13)),
    (ROJO, circulo(48)),                                 # buje a cuadros
    (BLANCO, circulo(35)),
    (NOCHE, cuadros(35)),
]
# para iconos de 32 píxeles o menos: sin muescas ni radios (si no, se emborrona)
capas_pequeno = [
    (NOCHE2, circulo(R + GROSOR / 2 + 6)),
    (ROJO, flecha(-160, -30)),
    (AMARILLO, flecha(20, 150)),
    (BLANCO, circulo(118)),
    (NOCHE2, circulo(78)),
    (BLANCO, circulo(52)),
    (NOCHE, cuadros(52)),
]

aqui = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(aqui, "logo.svg"), "w", encoding="utf-8") as fsvg:
    fsvg.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">\n'
               '  <!-- DS Kart Recompiled: rueda de kart con dos flechas de "volver a montar" (genera_logo.py) -->\n')
    for color, d in capas:
        fsvg.write(f'  <path fill="{color}" d="{d}"/>\n')
    fsvg.write("</svg>\n")
for nombre, lista in (("logo_xaml.txt", capas), ("logo_xaml_pequeno.txt", capas_pequeno)):
    with open(os.path.join(aqui, nombre), "w", encoding="utf-8") as fx:
        fx.write("<DrawingGroup>\n")
        fx.write("  <GeometryDrawing Brush='#00000000' Geometry='M0,0 L512,512'/>\n")  # fija el lienzo a 512x512
        for color, d in lista:
            fx.write(f"  <GeometryDrawing Brush='{color}' Geometry='F1 {d}'/>\n")
        fx.write("</DrawingGroup>\n")
print("ok")
