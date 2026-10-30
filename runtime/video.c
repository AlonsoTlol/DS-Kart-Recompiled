// Vídeo de la DS: los 9 bancos de VRAM y los dos motores 2D (A y B).
//
// Cada banco (A..I) se asigna con VRAMCNT a un uso: fondos o sprites de un
// motor, paletas extendidas, texturas 3D o acceso directo (LCDC). Aquí se
// mantienen tablas de páginas de 16 KB para cada uso.
//
// Los motores dibujan línea a línea: 4 fondos + sprites, se ordenan por
// prioridad, se aplican ventanas, transparencias y el brillo general.
// De momento se dibuja el fotograma entero al llegar la VBlank.
#include "compat.h"
#include "video.h"
#include "gpu3d.h"
#include <string.h>
#include <stdlib.h>

uint8_t g_paleta[0x800], g_oam[0x800];

static uint8_t bA[0x20000], bB[0x20000], bC[0x20000], bD[0x20000], bE[0x10000];
static uint8_t bF[0x4000], bG[0x4000], bH[0x8000], bI[0x4000];
static uint8_t *const banco[9] = {bA, bB, bC, bD, bE, bF, bG, bH, bI};
static const uint32_t tam_banco[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
                                      0x4000, 0x4000, 0x8000, 0x4000};
// Lo que lee el montaje de la imagen: la VRAM y el 3D de verdad, o la foto (montaje en otro hilo)
static uint8_t *const *banco_act = banco;
static const uint16_t *c3_act, *c3hd_act;
static const uint8_t *a3_act, *a3hd_act;
static uint8_t vramcnt[9];

// Páginas de 16 KB de cada zona
static uint8_t *bg_a[32], *obj_a[16], *bg_b[8], *obj_b[8], *lcdc[41];
static uint8_t *ext_bg_a[4], *ext_obj_a, *ext_bg_b[4], *ext_obj_b;  // paletas extendidas (8 KB)
uint8_t *g_tex[4], *g_texpal[6];  // para el 3D (más adelante)

static void mapea(uint8_t **tabla, int n, uint32_t off, uint8_t *mem, uint32_t tam) {
    for (uint32_t k = 0; k < tam; k += 0x4000) {
        int p = (int)((off + k) / 0x4000);
        if (p < n) tabla[p] = mem + k;
    }
}

static void recalcula(void) {
    memset(bg_a, 0, sizeof bg_a); memset(obj_a, 0, sizeof obj_a);
    memset(bg_b, 0, sizeof bg_b); memset(obj_b, 0, sizeof obj_b);
    memset(lcdc, 0, sizeof lcdc);
    memset(ext_bg_a, 0, sizeof ext_bg_a); memset(ext_bg_b, 0, sizeof ext_bg_b);
    ext_obj_a = ext_obj_b = NULL;
    memset(g_tex, 0, sizeof g_tex); memset(g_texpal, 0, sizeof g_texpal);
    static const uint32_t lcdc_off[9] = {0, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
    for (int b = 0; b < 9; b++) {
        uint8_t v = vramcnt[b];
        if (!(v & 0x80)) continue;
        int mst = v & 7, ofs = (v >> 3) & 3;
        uint8_t *m = banco[b];
        uint32_t t = tam_banco[b];
        if (mst == 0) { mapea(lcdc, 41, lcdc_off[b], m, t); continue; }
        switch (b) {
        case 0: case 1:  // A, B
            switch (mst & 3) {
            case 1: mapea(bg_a, 32, ofs * 0x20000, m, t); break;
            case 2: mapea(obj_a, 16, (ofs & 1) * 0x20000, m, t); break;
            case 3: g_tex[ofs] = m; break;
            }
            break;
        case 2: case 3:  // C, D
            switch (mst) {
            case 1: mapea(bg_a, 32, ofs * 0x20000, m, t); break;
            case 3: g_tex[ofs] = m; break;
            case 4: if (b == 2) mapea(bg_b, 8, 0, m, t); else mapea(obj_b, 8, 0, m, t); break;
            }  // mst 2: VRAM del ARM7 (va aparte)
            break;
        case 4:  // E
            switch (mst) {
            case 1: mapea(bg_a, 32, 0, m, t); break;
            case 2: mapea(obj_a, 16, 0, m, t); break;
            case 3: for (int k = 0; k < 4; k++) g_texpal[k] = m + k * 0x4000; break;
            case 4: for (int k = 0; k < 4; k++) ext_bg_a[k] = m + k * 0x2000; break;
            }
            break;
        case 5: case 6: {  // F, G
            uint32_t off = 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
            switch (mst) {
            case 1: mapea(bg_a, 32, off, m, t); break;
            case 2: mapea(obj_a, 16, off, m, t); break;
            case 3: g_texpal[(ofs & 1) + (ofs >> 1) * 4] = m; break;
            case 4: ext_bg_a[(ofs & 1) * 2] = m; ext_bg_a[(ofs & 1) * 2 + 1] = m + 0x2000; break;
            case 5: ext_obj_a = m; break;
            }
            break;
        }
        case 7:  // H
            switch (mst & 3) {
            case 1: mapea(bg_b, 8, 0, m, t); break;
            case 2: for (int k = 0; k < 4; k++) ext_bg_b[k] = m + k * 0x2000; break;
            }
            break;
        case 8:  // I
            switch (mst & 3) {
            case 1: mapea(bg_b, 8, 0x8000, m, t); break;
            case 2: mapea(obj_b, 8, 0, m, t); break;
            case 3: ext_obj_b = m; break;
            }
            break;
        }
    }
}

void video_vramcnt(int b, uint8_t v) {
    if (b < 0 || b > 8 || vramcnt[b] == v) return;
    vramcnt[b] = v;
    recalcula();
}

uint8_t *vram_ptr9(uint32_t a) {
    uint8_t *p;
    switch ((a >> 21) & 7) {
    case 0: p = bg_a[(a & 0x7FFFF) >> 14]; break;
    case 1: p = bg_b[(a & 0x1FFFF) >> 14]; break;
    case 2: p = obj_a[(a & 0x3FFFF) >> 14]; break;
    case 3: p = obj_b[(a & 0x1FFFF) >> 14]; break;
    default: {
        uint32_t o = a & 0xFFFFF;
        p = o < 0xA4000 ? lcdc[o >> 14] : NULL;
        break;
    }
    }
    return p ? p + (a & 0x3FFF) : NULL;
}

// ---- motores 2D ----------------------------------------------------------------
typedef struct {
    const uint8_t *r;               // registros del motor
    const uint16_t *pal_bg, *pal_obj;
    const uint8_t *oam;
    uint8_t **bg, **obj;
    uint32_t bg_mask, obj_mask;
    uint8_t **ext_bg, *ext_obj;
    int es_a;
} Motor;

static inline uint16_t r16(const Motor *m, int o) { return (uint16_t)(m->r[o] | m->r[o + 1] << 8); }
static inline uint32_t r32(const Motor *m, int o) { return r16(m, o) | (uint32_t)r16(m, o + 2) << 16; }

static inline uint8_t v8(uint8_t **t, uint32_t mask, uint32_t o) {
    o &= mask;
    uint8_t *p = t[o >> 14];
    return p ? p[o & 0x3FFF] : 0;
}
static inline uint16_t v16(uint8_t **t, uint32_t mask, uint32_t o) {
    return (uint16_t)(v8(t, mask, o) | v8(t, mask, o + 1) << 8);
}
static inline uint16_t ext16(uint8_t *slot, uint32_t idx) {
    return slot ? (uint16_t)(slot[idx * 2] | slot[idx * 2 + 1] << 8) : 0;
}

#define OPACO 0x8000  // en los búferes de línea: hay píxel

static uint16_t lin_bg[4][256];
static uint16_t lin_obj[256];
static uint8_t prio_obj[256], semi_obj[256], ventana_obj[256];
static uint8_t alfa_3d[256];  // alfa del fondo 0 cuando es 3D
static uint16_t lin_a15[256];  // la línea del motor A en 15 bits, antes del brillo general (para la captura)
static uint8_t marca3d[256];   // reescalado: en este punto se ve el 3D tal cual (se puede coger del 3D grande)
static uint16_t brillo_a;      // MASTER_BRIGHT del motor A en la línea actual
static int hofs_3d;           // desplazamiento horizontal de la capa 3D en la línea actual (el menú lo usa)

static void fondo_texto(const Motor *m, int n, int y) {
    uint16_t cnt = r16(m, 0x08 + 2 * n), dc_lo = r16(m, 0);
    uint32_t dc = r32(m, 0);
    int hofs = r16(m, 0x10 + 4 * n) & 0x1FF, vofs = r16(m, 0x12 + 4 * n) & 0x1FF;
    int w = (cnt >> 14) & 1 ? 512 : 256, h = (cnt >> 14) & 2 ? 512 : 256;
    uint32_t chr = ((cnt >> 2) & 15) * 0x4000 + (m->es_a ? ((dc >> 24) & 7) * 0x10000 : 0);
    uint32_t scr = ((cnt >> 8) & 31) * 0x800 + (m->es_a ? ((dc >> 27) & 7) * 0x10000 : 0);
    int bpp8 = (cnt >> 7) & 1;
    uint8_t *ext = NULL;
    if (bpp8 && (dc >> 30) & 1) ext = m->ext_bg[(n < 2 && (cnt & 0x2000)) ? n + 2 : n];
    int yy = (y + vofs) & (h - 1);
    (void)dc_lo;
    for (int x = 0; x < 256; x++) {
        int xx = (x + hofs) & (w - 1);
        uint32_t bloque = scr + (uint32_t)(((yy >> 8) * (w >> 8) + (xx >> 8)) * 0x800);
        uint16_t e = v16(m->bg, m->bg_mask, bloque + (uint32_t)((((yy & 255) >> 3) * 32 + ((xx & 255) >> 3)) * 2));
        int px = xx & 7, py = yy & 7;
        if (e & 0x400) px = 7 - px;
        if (e & 0x800) py = 7 - py;
        uint32_t tile = e & 0x3FF;
        uint16_t col;
        if (bpp8) {
            uint8_t i = v8(m->bg, m->bg_mask, chr + tile * 64 + (uint32_t)(py * 8 + px));
            if (!i) { lin_bg[n][x] = 0; continue; }
            col = ext ? ext16(ext, (uint32_t)((e >> 12) * 256 + i)) : m->pal_bg[i];
        } else {
            uint8_t b = v8(m->bg, m->bg_mask, chr + tile * 32 + (uint32_t)(py * 4 + px / 2));
            uint8_t i = (px & 1) ? b >> 4 : b & 15;
            if (!i) { lin_bg[n][x] = 0; continue; }
            col = m->pal_bg[(e >> 12) * 16 + i];
        }
        lin_bg[n][x] = col | OPACO;
    }
}

// Fondos que rotan/escalan (afines) y sus variantes extendidas (mapa de bits)
static int32_t ref_x[2][2], ref_y[2][2];  // posición interna por motor y fondo (2 y 3)

static void fondo_afin(const Motor *m, int n, int y, int extendido) {
    uint16_t cnt = r16(m, 0x08 + 2 * n);
    uint32_t dc = r32(m, 0);
    int o = n == 2 ? 0x20 : 0x30;
    int16_t pa = (int16_t)r16(m, o), pc = (int16_t)r16(m, o + 4);
    int32_t *rx = &ref_x[!m->es_a][n - 2], *ry = &ref_y[!m->es_a][n - 2];
    int sz = (cnt >> 14) & 3;
    int envuelve = (cnt >> 13) & 1;
    int32_t x0 = *rx, y0 = *ry;
    (void)y;
    uint32_t chr = ((cnt >> 2) & 15) * 0x4000 + (m->es_a ? ((dc >> 24) & 7) * 0x10000 : 0);
    uint32_t scr = ((cnt >> 8) & 31) * 0x800 + (m->es_a ? ((dc >> 27) & 7) * 0x10000 : 0);
    int tipo = 0, w, h;  // 0 afín normal, 1 afín con mapa de 16 bits, 2 bitmap 256 colores, 3 bitmap directo
    if (!extendido) { w = h = 128 << sz; }
    else if (!(cnt & 0x80)) { tipo = 1; w = h = 128 << sz; }
    else {
        tipo = (cnt & 4) ? 3 : 2;
        static const int bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
        w = bw[sz]; h = bh[sz];
        scr = ((cnt >> 8) & 31) * 0x4000;
    }
    uint8_t *ext = (dc >> 30) & 1 ? m->ext_bg[n] : NULL;
    for (int x = 0; x < 256; x++) {
        int tx = (x0 + pa * x) >> 8, ty = (y0 + pc * x) >> 8;
        if (envuelve) { tx &= w - 1; ty &= h - 1; }
        else if (tx < 0 || ty < 0 || tx >= w || ty >= h) { lin_bg[n][x] = 0; continue; }
        uint16_t col = 0;
        int opaco = 0;
        if (tipo == 0) {
            uint8_t t = v8(m->bg, m->bg_mask, scr + (uint32_t)((ty >> 3) * (w >> 3) + (tx >> 3)));
            uint8_t i = v8(m->bg, m->bg_mask, chr + t * 64u + (uint32_t)((ty & 7) * 8 + (tx & 7)));
            if (i) { col = m->pal_bg[i]; opaco = 1; }
        } else if (tipo == 1) {
            uint16_t e = v16(m->bg, m->bg_mask, scr + (uint32_t)(((ty >> 3) * (w >> 3) + (tx >> 3)) * 2));
            int px = tx & 7, py = ty & 7;
            if (e & 0x400) px = 7 - px;
            if (e & 0x800) py = 7 - py;
            uint8_t i = v8(m->bg, m->bg_mask, chr + (e & 0x3FFu) * 64 + (uint32_t)(py * 8 + px));
            if (i) { col = ext ? ext16(ext, (uint32_t)((e >> 12) * 256 + i)) : m->pal_bg[i]; opaco = 1; }
        } else if (tipo == 2) {
            uint8_t i = v8(m->bg, m->bg_mask, scr + (uint32_t)(ty * w + tx));
            if (i) { col = m->pal_bg[i]; opaco = 1; }
        } else {
            uint16_t c = v16(m->bg, m->bg_mask, scr + (uint32_t)(ty * w + tx) * 2);
            if (c & 0x8000) { col = c; opaco = 1; }
        }
        lin_bg[n][x] = opaco ? (col & 0x7FFF) | OPACO : 0;
    }
    *rx += (int16_t)r16(m, o + 2);  // PB
    *ry += (int16_t)r16(m, o + 6);  // PD
}

static void reinicia_afines(const Motor *m) {
    int k = !m->es_a;
    for (int n = 0; n < 2; n++) {
        int o = n == 0 ? 0x28 : 0x38;
        ref_x[k][n] = (int32_t)(r32(m, o) << 4) >> 4;
        ref_y[k][n] = (int32_t)(r32(m, o + 4) << 4) >> 4;
    }
}

static const uint8_t obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

static void sprites(const Motor *m, int y) {
    uint32_t dc = r32(m, 0);
    memset(lin_obj, 0, sizeof lin_obj);
    memset(prio_obj, 4, sizeof prio_obj);
    memset(semi_obj, 0, sizeof semi_obj);
    memset(ventana_obj, 0, sizeof ventana_obj);
    if (!((dc >> 12) & 1)) return;
    for (int i = 127; i >= 0; i--) {  // del último al primero: el de menor índice gana
        const uint8_t *e = m->oam + i * 8;
        uint16_t a0 = (uint16_t)(e[0] | e[1] << 8), a1 = (uint16_t)(e[2] | e[3] << 8), a2 = (uint16_t)(e[4] | e[5] << 8);
        int afin = (a0 >> 8) & 1;
        if (!afin && ((a0 >> 9) & 1)) continue;  // oculto
        int forma = (a0 >> 14) & 3, tam = (a1 >> 14) & 3;
        if (forma == 3) continue;
        int w = obj_w[forma][tam], h = obj_h[forma][tam];
        int doble = afin && ((a0 >> 9) & 1);
        int bw = doble ? w * 2 : w, bh = doble ? h * 2 : h;
        int oy = a0 & 0xFF;
        if (oy + bh > 256) oy -= 256;
        int fila = y - oy;
        if (fila < 0 || fila >= bh) continue;
        int ox = a1 & 0x1FF;
        if (ox >= 256) ox -= 512;
        int modo = (a0 >> 10) & 3, bpp8 = (a0 >> 13) & 1;
        int prio = (a2 >> 10) & 3;
        uint32_t tile = a2 & 0x3FF;
        int16_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (afin) {
            const uint8_t *p = m->oam + ((a1 >> 9) & 31) * 32;
            pa = (int16_t)(p[6] | p[7] << 8); pb = (int16_t)(p[14] | p[15] << 8);
            pc = (int16_t)(p[22] | p[23] << 8); pd = (int16_t)(p[30] | p[31] << 8);
        }
        for (int bx = 0; bx < bw; bx++) {
            int sx = ox + bx;
            if (sx < 0 || sx >= 256) continue;
            int tx, ty;
            if (afin) {
                int cx = bx - bw / 2, cy = fila - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - bx : bx;
                ty = (a1 & 0x2000) ? h - 1 - fila : fila;
            }
            uint16_t col = 0;
            int opaco = 0;
            if (modo == 3) {  // sprite de mapa de bits (color directo)
                uint32_t dir;
                if ((dc >> 6) & 1) dir = tile * (128u << ((dc >> 22) & 1)) + (uint32_t)(ty * w + tx) * 2;
                else {
                    int ancho = (dc >> 5) & 1 ? 256 : 128;
                    uint32_t mask = ancho == 256 ? 0x1F : 0x0F;
                    dir = (tile & mask) * 0x10 + (tile & ~mask) * 0x80 + (uint32_t)(ty * ancho + tx) * 2;
                }
                uint16_t c = v16(m->obj, m->obj_mask, dir);
                if (c & 0x8000) { col = c; opaco = 1; }
            } else {
                uint32_t base;
                int t_x = tx >> 3, t_y = ty >> 3;
                if ((dc >> 4) & 1) {  // mapeo 1D
                    uint32_t limite = 32u << ((dc >> 20) & 3);
                    base = tile * limite + (uint32_t)((t_y * (w >> 3) + t_x) * (bpp8 ? 64 : 32));
                } else {  // mapeo 2D: rejilla de 32x32 mosaicos de 32 bytes
                    base = (tile + (uint32_t)(t_y * 32 + t_x * (bpp8 ? 2 : 1))) * 32;
                }
                int px = tx & 7, py = ty & 7;
                if (bpp8) {
                    uint8_t ix = v8(m->obj, m->obj_mask, base + (uint32_t)(py * 8 + px));
                    if (ix) {
                        opaco = 1;
                        col = ((dc >> 31) & 1) ? ext16(m->ext_obj, (uint32_t)((a2 >> 12) * 256 + ix)) : m->pal_obj[ix];
                    }
                } else {
                    uint8_t b = v8(m->obj, m->obj_mask, base + (uint32_t)(py * 4 + px / 2));
                    uint8_t ix = (px & 1) ? b >> 4 : b & 15;
                    if (ix) { opaco = 1; col = m->pal_obj[(a2 >> 12) * 16 + ix]; }
                }
            }
            if (!opaco) continue;
            if (modo == 2) { ventana_obj[sx] = 1; continue; }
            if (modo == 3 && !(a2 >> 12)) continue;  // mapa de bits con alfa 0: no se ve
            if (prio <= prio_obj[sx]) {
                lin_obj[sx] = (col & 0x7FFF) | OPACO;
                prio_obj[sx] = (uint8_t)prio;
                // 1 = semitransparente (usa BLDALPHA); mapa de bits: 2 + su propio alfa (0-15, en attr2)
                semi_obj[sx] = modo == 1 ? 1 : modo == 3 ? (uint8_t)(2 + (a2 >> 12)) : 0;
            }
        }
    }
}

static inline uint32_t a_rgb(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r * 255 / 31) << 16 | (g * 255 / 31) << 8 | (b * 255 / 31);
}

static uint16_t mezcla(uint16_t a, uint16_t b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | g << 5 | bl << 10);
}

static uint16_t brillo(uint16_t c, int modo, int f) {
    if (f > 16) f = 16;
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    if (modo == 1) { r += (31 - r) * f / 16; g += (31 - g) * f / 16; b += (31 - b) * f / 16; }
    else { r -= r * f / 16; g -= g * f / 16; b -= b * f / 16; }
    return (uint16_t)(r | g << 5 | b << 10);
}

static void dibuja_linea(const Motor *m, int y, uint32_t *out) {
    uint32_t dc = r32(m, 0);
    if (m->es_a) memset(marca3d, 0, sizeof marca3d);
    int modo_pantalla = (dc >> 16) & 3;
    if (!m->es_a) modo_pantalla &= 1;
    if (modo_pantalla == 0 || (dc & 0x80)) {  // apagada o en blanco forzado
        for (int x = 0; x < 256; x++) out[x] = 0xFFFFFF;
        return;
    }
    if (modo_pantalla == 2) {  // muestra un banco de VRAM tal cual
        const uint8_t *b = banco_act[(dc >> 18) & 3] + y * 512;
        for (int x = 0; x < 256; x++) out[x] = a_rgb((uint16_t)(b[x * 2] | b[x * 2 + 1] << 8));
        return;
    }
    if (modo_pantalla == 3) {
        for (int x = 0; x < 256; x++) out[x] = 0;
        return;
    }
    int modo_bg = dc & 7;
    int activo[4];
    for (int n = 0; n < 4; n++) {
        activo[n] = (dc >> (8 + n)) & 1;
        if (!activo[n]) continue;
        int tipo = 0;  // 0 texto, 1 afín, 2 extendido
        if (n == 0 && m->es_a && (dc & 8)) {  // fondo 0 = la imagen del motor 3D
            const uint16_t *c3 = c3_act + y * 256;
            const uint8_t *a3 = a3_act + y * 256;
            int hofs = r16(m, 0x10) & 0x1FF;
            if (hofs & 0x100) hofs -= 0x200;
            hofs_3d = hofs;
            for (int x = 0; x < 256; x++) {
                int sx = x + hofs;
                uint8_t a = (sx >= 0 && sx < 256) ? a3[sx] : 0;
                alfa_3d[x] = a;
                lin_bg[0][x] = a ? (c3[sx] & 0x7FFF) | OPACO : 0;
            }
            continue;
        }
        if (n == 2) tipo = (modo_bg == 2 || modo_bg == 4) ? 1 : modo_bg == 5 ? 2 : 0;
        if (n == 3) tipo = (modo_bg == 1 || modo_bg == 2) ? 1 : modo_bg >= 3 ? 2 : 0;
        if (modo_bg == 6 && n != 0) { activo[n] = 0; continue; }
        if (tipo == 0) fondo_texto(m, n, y);
        else fondo_afin(m, n, y, tipo == 2);
    }
    sprites(m, y);
    // ventanas: qué capas se ven en cada píxel (bits 0-3 fondos, 4 sprites, 5 efectos)
    uint8_t vis[256];
    int hay_ventanas = (dc >> 13) & 7;
    for (int x = 0; x < 256; x++) vis[x] = 0x3F;
    if (hay_ventanas) {
        uint16_t winin = r16(m, 0x48), winout = r16(m, 0x4A);
        for (int x = 0; x < 256; x++) {
            uint8_t v = (uint8_t)(winout & 0x3F);
            if (((dc >> 15) & 1) && ventana_obj[x]) v = (uint8_t)((winout >> 8) & 0x3F);
            for (int w = 1; w >= 0; w--) {
                if (!((dc >> (13 + w)) & 1)) continue;
                int x1 = m->r[0x41 + 2 * w], x2 = m->r[0x40 + 2 * w];
                int y1 = m->r[0x45 + 2 * w], y2 = m->r[0x44 + 2 * w];
                int dentro_x = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
                int dentro_y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
                if (dentro_x && dentro_y) { v = (uint8_t)((winin >> (8 * w)) & 0x3F); }
            }
            vis[x] = v;
        }
        // la ventana 0 tiene preferencia: se aplica la última del bucle (w = 0)
    }
    uint16_t bldcnt = r16(m, 0x50), bldalpha = r16(m, 0x52);
    int efecto = (bldcnt >> 6) & 3;
    int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = r16(m, 0x54) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    uint16_t fondo = m->pal_bg[0] & 0x7FFF;
    uint16_t mb = r16(m, 0x6C);
    for (int x = 0; x < 256; x++) {
        // las dos capas de arriba: (color, id de capa) con id 0-3 fondos, 4 sprites, 5 fondo
        uint16_t c1 = fondo, c2 = fondo;
        int id1 = 5, id2 = 5, p1 = 5, p2 = 5;
        for (int pr = 3; pr >= 0; pr--) {
            for (int n = 3; n >= 0; n--) {
                if (!activo[n] || !(vis[x] & (1 << n)) || !(lin_bg[n][x] & OPACO)) continue;
                if ((r16(m, 0x08 + 2 * n) & 3) != pr) continue;
                c2 = c1; id2 = id1; p2 = p1;
                c1 = lin_bg[n][x] & 0x7FFF; id1 = n; p1 = pr;
            }
            if ((lin_obj[x] & OPACO) && prio_obj[x] == pr && (vis[x] & 0x10)) {
                c2 = c1; id2 = id1; p2 = p1;
                c1 = lin_obj[x] & 0x7FFF; id1 = 4; p1 = pr;
            }
        }
        (void)p1; (void)p2;
        uint16_t c = c1;
        int efectos = (vis[x] >> 5) & 1;
        int es_3d = id1 == 0 && m->es_a && (dc & 8);
        if (es_3d && alfa_3d[x] < 31 && ((bldcnt >> (8 + id2)) & 1)) {  // 3D semitransparente
            int a = alfa_3d[x] + 1;
            c = mezcla(c1, c2, a / 2, 16 - a / 2);
        } else if (id1 == 4 && semi_obj[x] && ((bldcnt >> (8 + id2)) & 1)) {
            // los sprites de mapa de bits mezclan con su propio alfa (alfa + 1)/16, no con BLDALPHA
            // (la copia de la captura que se muestra con sprites salía oscura en fotogramas alternos)
            if (semi_obj[x] >= 2) c = mezcla(c1, c2, semi_obj[x] - 1, 17 - semi_obj[x]);
            else c = mezcla(c1, c2, eva, evb);
        } else if (efectos && ((bldcnt >> id1) & 1)) {
            if (efecto == 1 && ((bldcnt >> (8 + id2)) & 1)) c = mezcla(c1, c2, eva, evb);
            else if (efecto == 2) c = brillo(c1, 1, evy);
            else if (efecto == 3) c = brillo(c1, 2, evy);
        }
        if (m->es_a) {
            lin_a15[x] = c;  // para la captura (va antes del brillo general)
            marca3d[x] = es_3d && c == c1;  // se ve el 3D sin mezclas
            brillo_a = mb;
        }
        int bm = (mb >> 14) & 3;
        if (bm == 1 || bm == 2) c = brillo(c, bm, mb & 31);
        out[x] = a_rgb(c);
    }
}

// Captura de pantalla (DISPCAPCNT): copia una línea de la imagen del motor A, del 3D solo o de la VRAM
// (o una mezcla) a un banco A-D. El juego la usa para tener 3D en las dos pantallas (presentación del
// circuito, ceremonia): pinta una pantalla cada fotograma y la otra muestra la copia del anterior.
static void captura_linea(const uint8_t *io9, uint32_t cap, int y) {
    static const int ancho[4] = {128, 256, 256, 256}, alto[4] = {128, 64, 128, 192};
    int sz = (cap >> 20) & 3, w = ancho[sz];
    if (y >= alto[sz]) return;
    uint8_t *dst = banco[(cap >> 16) & 3];
    uint32_t woff = ((cap >> 18) & 3) * 0x8000;
    uint32_t dispcnt = (uint32_t)(io9[0] | io9[1] << 8 | io9[2] << 16 | io9[3] << 24);
    const uint8_t *vram = banco[(dispcnt >> 18) & 3];
    uint32_t roff = ((cap >> 26) & 3) * 0x8000;
    int fuente = (cap >> 29) & 3, solo3d = (cap >> 24) & 1, desde_fifo = (cap >> 25) & 1;
    int eva = cap & 31, evb = (cap >> 8) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    const uint16_t *c3 = gx_color() + y * 256;
    const uint8_t *a3 = gx_alfa() + y * 256;
    for (int x = 0; x < w; x++) {
        uint16_t ca = solo3d ? (uint16_t)(c3[x] & 0x7FFF) : lin_a15[x];
        int aa = solo3d ? a3[x] != 0 : 1;
        uint16_t cb = 0;
        int ab = 0;
        if (!desde_fifo) {
            uint32_t o = (roff + (uint32_t)(y * w + x) * 2) & 0x1FFFF;
            cb = (uint16_t)(vram[o] | vram[o + 1] << 8);
            ab = cb >> 15;
            cb &= 0x7FFF;
        }
        uint16_t col;
        int alfa;
        if (fuente == 0) { col = ca; alfa = aa; }
        else if (fuente == 1) { col = cb; alfa = ab; }
        else {
            int r = ((ca & 31) * aa * eva + (cb & 31) * ab * evb) / 16;
            int g = (((ca >> 5) & 31) * aa * eva + ((cb >> 5) & 31) * ab * evb) / 16;
            int b = (((ca >> 10) & 31) * aa * eva + ((cb >> 10) & 31) * ab * evb) / 16;
            col = (uint16_t)((r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (b > 31 ? 31 : b) << 10);
            alfa = (aa && eva) || (ab && evb);
        }
        uint32_t o = (woff + (uint32_t)(y * w + x) * 2) & 0x1FFFF;
        uint16_t v = (uint16_t)(col | (alfa ? 0x8000 : 0));
        dst[o] = (uint8_t)v;
        dst[o + 1] = (uint8_t)(v >> 8);
    }
}

// ---- reescalado -----------------------------------------------------------------
int video_escala(void) {
    static int e;
    if (!e) {
        const char *v = getenv("MKDS_ESCALA");
        e = v ? atoi(v) : 1;
        if (e < 1) e = 1;
        if (e > 4) e = 4;
    }
    return e;
}

static uint32_t hd_arriba[1024 * 768], hd_abajo[1024 * 768];
// del último montaje: en qué puntos de cada línea del motor A se ve el 3D tal cual, con qué desplazamiento y
// brillo, y si el motor A sale arriba (para las imágenes intermedias: solo cambian esos puntos)
static uint8_t marca_f[192][256];
static int hofs_f[192];
static uint16_t brillo_f[192];
static int a_arriba;
const uint32_t *video_hd(int abajo) { return abajo ? hd_abajo : hd_arriba; }

// Una línea de la DS a tamaño grande: lo que es 3D se coge del 3D pintado a más resolución; lo demás
// (menús, marcador, mapa) se repite tal cual, sin difuminar
static void linea_hd(uint32_t *dst, const uint32_t *baja, int y, int es_a) {
    int e = video_escala(), an = 256 * e;
    const uint16_t *c3 = c3hd_act;
    const uint8_t *a3 = a3hd_act;
    int bm = (brillo_a >> 14) & 3, f = brillo_a & 31;
    for (int sy = 0; sy < e; sy++) {
        uint32_t *d = dst + (size_t)(y * e + sy) * an;
        int fila3d = (y * e + sy) * an;
        for (int x = 0; x < 256; x++) {
            if (es_a && marca3d[x]) {
                for (int sx = 0; sx < e; sx++) {
                    int i = fila3d + (x + hofs_3d) * e + sx;  // marca3d solo está donde x + hofs cae dentro
                    if (!a3[i]) { d[x * e + sx] = baja[x]; continue; }
                    uint16_t c = (uint16_t)(c3[i] & 0x7FFF);
                    if (bm == 1 || bm == 2) c = brillo(c, bm, f);
                    d[x * e + sx] = a_rgb(c);
                }
            } else {
                for (int sx = 0; sx < e; sx++) d[x * e + sx] = baja[x];
            }
        }
    }
}

// Tablas de páginas de los dos motores (las de verdad o las de la foto)
typedef struct {
    uint8_t *bg_a[32], *obj_a[16], *bg_b[8], *obj_b[8], *ext_bg_a[4], *ext_obj_a, *ext_bg_b[4], *ext_obj_b;
} Tablas;

static void monta(const uint8_t *io9, const uint8_t *pal, const uint8_t *oam, Tablas *t, uint32_t *arriba,
                  uint32_t *abajo);

void video_dibuja(const uint8_t *io9, uint32_t *arriba, uint32_t *abajo) {
    static Tablas t;
    memcpy(t.bg_a, bg_a, sizeof bg_a); memcpy(t.obj_a, obj_a, sizeof obj_a);
    memcpy(t.bg_b, bg_b, sizeof bg_b); memcpy(t.obj_b, obj_b, sizeof obj_b);
    memcpy(t.ext_bg_a, ext_bg_a, sizeof ext_bg_a); memcpy(t.ext_bg_b, ext_bg_b, sizeof ext_bg_b);
    t.ext_obj_a = ext_obj_a; t.ext_obj_b = ext_obj_b;
    banco_act = banco;
    c3_act = gx_color(); a3_act = gx_alfa(); c3hd_act = gx_hd_color(); a3hd_act = gx_hd_alfa();
    monta(io9, g_paleta, g_oam, &t, arriba, abajo);
}

// Imagen intermedia (MKDS_FPS): la del último montaje con los puntos de 3D cambiados por los de una escena
// intermedia (c3/a3, a tamaño 256*e x 192*e). dst: pantalla de arriba y debajo la de abajo, a ese tamaño.
static void monta_intermedia(uint32_t *dst, const uint32_t *arriba, const uint32_t *abajo, const uint16_t *c3,
                             const uint8_t *a3, int e) {
    int an = 256 * e;
    size_t n = (size_t)an * 192 * e;
    memcpy(dst, e > 1 ? hd_arriba : arriba, n * 4);
    memcpy(dst + n, e > 1 ? hd_abajo : abajo, n * 4);
    uint32_t *pa = a_arriba ? dst : dst + n;
    for (int y = 0; y < 192; y++) {
        int bm = (brillo_f[y] >> 14) & 3, f = brillo_f[y] & 31, h = hofs_f[y];
        for (int sy = 0; sy < e; sy++) {
            int fila = y * e + sy;
            uint32_t *d = pa + (size_t)fila * an;
            for (int x = 0; x < 256; x++) {
                if (!marca_f[y][x]) continue;
                for (int sx = 0; sx < e; sx++) {
                    int i = fila * an + (x + h) * e + sx;
                    if (!a3[i]) continue;
                    uint16_t c = (uint16_t)(c3[i] & 0x7FFF);
                    if (bm == 1 || bm == 2) c = brillo(c, bm, f);
                    d[x * e + sx] = a_rgb(c);
                }
            }
        }
    }
}

// ---- montaje en otro hilo ---------------------------------------------------------
static struct {
    uint8_t io[0x2000], pal[0x800], oam[0x800];
    uint8_t mA[0x20000], mB[0x20000], mC[0x20000], mD[0x20000], mE[0x10000];
    uint8_t mF[0x4000], mG[0x4000], mH[0x8000], mI[0x4000];
    uint16_t c3[256 * 192], c3hd[1024 * 768];
    uint8_t a3[256 * 192], a3hd[1024 * 768];
    Tablas t;
    int ind;      // juego de salidas del 3D de esta foto (para sus imágenes intermedias)
    int64_t ts;   // cuándo se hizo (hora del fotograma)
} foto;
static uint8_t *const banco_foto[9] = {foto.mA, foto.mB, foto.mC, foto.mD, foto.mE, foto.mF, foto.mG, foto.mH, foto.mI};
static HANDLE ev_foto;
static volatile LONG montando;

static uint8_t *a_foto(uint8_t *p) {  // un puntero a la VRAM de verdad -> el mismo sitio en la foto
    if (!p) return NULL;
    for (int b = 0; b < 9; b++)
        if (p >= banco[b] && p < banco[b] + tam_banco[b]) return banco_foto[b] + (p - banco[b]);
    return NULL;
}

static DWORD WINAPI hilo_monta(LPVOID arg) {
    (void)arg;
    static uint32_t arriba[256 * 192], abajo[256 * 192];
    for (;;) {
        WaitForSingleObject(ev_foto, INFINITE);
        banco_act = banco_foto;
        c3_act = foto.c3; a3_act = foto.a3; c3hd_act = foto.c3hd; a3hd_act = foto.a3hd;
        monta(foto.io, foto.pal, foto.oam, &foto.t, arriba, abajo);
        // imágenes intermedias entre el fotograma anterior y este, repartidas en el tiempo; luego este
        int ns = gx_nsub(foto.ind), e = video_escala();
        LARGE_INTEGER fr;
        QueryPerformanceFrequency(&fr);
        int64_t dt = fr.QuadPart / 60 / (ns + 1);
        // con intermedias, todo sale con un margen (MKDS_MARGEN_MS, 12 ms): montarlas lleva unos milisegundos
        // y, sin margen, la primera ya llegaba tarde y el pintor se la saltaba (salían 120 en vez de 180)
        static int64_t margen = -1;
        if (margen < 0) {
            const char *m = getenv("MKDS_MARGEN_MS");
            margen = fr.QuadPart * (m ? atoi(m) : 12) / 1000;
        }
        // (el margen va siempre que haya MKDS_FPS, también en las imágenes sin intermedias: si no, al pasar de
        // unas a otras una imagen nueva podía salir antes que la anterior)
        static int con_fps = -1;
        if (con_fps < 0) con_fps = getenv("MKDS_FPS") && atoi(getenv("MKDS_FPS")) > 60;
        int64_t base = foto.ts + (con_fps ? margen : 0);
        for (int j = 0; j < ns; j++) {
            uint32_t *dst = ventana_reserva_intermedia();
            monta_intermedia(dst, arriba, abajo, gx_sub_color(foto.ind, j), gx_sub_alfa(foto.ind, j), e);
            ventana_publica_intermedia(base + j * dt);
        }
        ventana_muestra_en(arriba, abajo, base + ns * dt);
        InterlockedExchange(&montando, 0);
    }
}

void video_espera_montaje(void) {
    while (montando) SwitchToThread();
}

void video_dibuja_aparte(const uint8_t *io9) {
    if (!ev_foto) {
        ev_foto = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE h = CreateThread(NULL, 0, hilo_monta, NULL, 0, NULL);
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);  // el juego primero
        CloseHandle(h);
    }
    if (montando) return;  // aún va por la anterior: esta se salta (el juego no espera)
    memcpy(foto.io, io9, sizeof foto.io);
    memcpy(foto.pal, g_paleta, sizeof foto.pal);
    memcpy(foto.oam, g_oam, sizeof foto.oam);
    for (int b = 0; b < 9; b++) memcpy(banco_foto[b], banco[b], tam_banco[b]);
    for (int i = 0; i < 32; i++) foto.t.bg_a[i] = a_foto(bg_a[i]);
    for (int i = 0; i < 16; i++) foto.t.obj_a[i] = a_foto(obj_a[i]);
    for (int i = 0; i < 8; i++) { foto.t.bg_b[i] = a_foto(bg_b[i]); foto.t.obj_b[i] = a_foto(obj_b[i]); }
    for (int i = 0; i < 4; i++) { foto.t.ext_bg_a[i] = a_foto(ext_bg_a[i]); foto.t.ext_bg_b[i] = a_foto(ext_bg_b[i]); }
    foto.t.ext_obj_a = a_foto(ext_obj_a);
    foto.t.ext_obj_b = a_foto(ext_obj_b);
    foto.ind = gx_indice();
    foto.ts = ventana_reloj();
    memcpy(foto.c3, gx_color(), sizeof foto.c3);
    memcpy(foto.a3, gx_alfa(), sizeof foto.a3);
    int e = video_escala();
    if (e > 1) {
        memcpy(foto.c3hd, gx_hd_color(), (size_t)256 * e * 192 * e * 2);
        memcpy(foto.a3hd, gx_hd_alfa(), (size_t)256 * e * 192 * e);
    }
    InterlockedExchange(&montando, 1);
    SetEvent(ev_foto);
}

static void monta(const uint8_t *io9, const uint8_t *pal, const uint8_t *oam, Tablas *t, uint32_t *arriba,
                  uint32_t *abajo) {
    Motor a = {io9, (const uint16_t *)pal, (const uint16_t *)(pal + 0x200), oam,
               t->bg_a, t->obj_a, 0x7FFFF, 0x3FFFF, t->ext_bg_a, t->ext_obj_a, 1};
    Motor b = {io9 + 0x1000, (const uint16_t *)(pal + 0x400), (const uint16_t *)(pal + 0x600),
               oam + 0x400, t->bg_b, t->obj_b, 0x1FFFF, 0x1FFFF, t->ext_bg_b, t->ext_obj_b, 0};
    uint16_t powcnt = (uint16_t)(io9[0x304] | io9[0x305] << 8);
    uint32_t *sal_a = (powcnt >> 15) & 1 ? arriba : abajo;
    uint32_t *sal_b = (powcnt >> 15) & 1 ? abajo : arriba;
    reinicia_afines(&a);
    reinicia_afines(&b);
    uint32_t cap = (uint32_t)(io9[0x64] | io9[0x65] << 8 | io9[0x66] << 16 | io9[0x67] << 24);
    a_arriba = sal_a == arriba;
    for (int y = 0; y < 192; y++) {
        memset(lin_a15, 0, sizeof lin_a15);
        if (powcnt & 2) dibuja_linea(&a, y, sal_a + y * 256);
        else memset(sal_a + y * 256, 0, 256 * 4);
        if (powcnt & 2) memcpy(marca_f[y], marca3d, 256);
        else memset(marca_f[y], 0, 256);
        hofs_f[y] = hofs_3d;
        brillo_f[y] = brillo_a;
        if (cap >> 31) captura_linea(io9, cap, y);
        if (powcnt & 0x200) dibuja_linea(&b, y, sal_b + y * 256);
        else memset(sal_b + y * 256, 0, 256 * 4);
        if (video_escala() > 1) {
            // con la captura encendida, sin 3D grande: la otra pantalla enseña la copia (a tamaño DS) y cada
            // pantalla temblaría entre nítida y no nítida un fotograma sí y otro no
            linea_hd(sal_a == arriba ? hd_arriba : hd_abajo, sal_a + y * 256, y, !(cap >> 31));
            linea_hd(sal_b == arriba ? hd_arriba : hd_abajo, sal_b + y * 256, y, 0);
        }
    }
}
