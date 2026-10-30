// Motor 3D de la DS: geometría (órdenes, matrices, luces, recorte) y rasterizador.
//
// El juego manda órdenes por la FIFO (0x04000400, empaquetadas de 4 en 4) o
// por puertos directos (0x04000440 + 4 * (orden - 0x10)). Los vértices se
// transforman al llegar y los polígonos se guardan en una lista; cuando el
// juego pide SWAP_BUFFERS, en la siguiente VBlank se pinta la lista entera en
// una imagen de 256x192 que el motor 2D A usa como fondo 0.
//
// Se calcula en coma flotante (la DS usa coma fija): pequeñas diferencias de
// redondeo, pero mucho más sencillo.
#include "gpu3d.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern uint8_t *g_tex[4], *g_texpal[6];

typedef float Mat[16];  // fila por fila; vector fila: v' = v * M

// ---- estado del motor de geometría ------------------------------------------
static Mat proj, pos, vec, tex;
static Mat pila_proj[1], pila_pos[32], pila_vec[32], pila_tex[1];
static int sp_proj, sp_pos, sp_tex, modo_mtx;
static int error_pila;

static uint32_t d_pl_min = 0xFFFF, d_pl_max;  // PLTT_BASE recibidos (depuración)
static uint32_t attr_pendiente, attr;   // POLYGON_ATTR (se aplica en BEGIN_VTXS)
static uint32_t teximage, pltt_base;
static int tipo_prim;
static float vx, vy, vz;                 // último vértice (para VTX_XY, VTX_DIFF...)
static float col_r = 31, col_g = 31, col_b = 31;
static float ts, tt;                     // coordenadas de textura actuales (en texels)
static float ts0, tt0;                   // las que se dieron con TEXCOORD (para los modos 2 y 3)
static uint32_t dif_amb, spe_emi;
static float luz_dir[4][3], luz_col[4][3];
static uint8_t brillo_tabla[128];
static int vp_x1, vp_y1 = 0, vp_x2 = 255, vp_y2 = 191;

// ---- lista de polígonos ------------------------------------------------------
typedef struct {
    float x, y, z, w;     // coordenadas de recorte
    float r, g, b;        // color 0..31
    float s, t;           // texels
} Vert;

#define MAX_VERT_POLI 10
typedef struct {
    Vert v[MAX_VERT_POLI];
    int n;                // vértices tras recortar (0 si queda fuera de la vista)
    uint32_t attr, teximage, pltt;
    int translucido;
    Vert crudo[4];        // sin recortar: para las imágenes intermedias (MKDS_FPS)
    int ncrudo;
} Poli;

#define MAX_POLIS 4096
static Poli polis[2][MAX_POLIS];
static int npolis[2], lista;  // lista = la que se está llenando (con los que quedan fuera de la vista)
static int nvisibles[2];      // los que se ven (lo que cuenta la consola)
static int swap_pendiente, swap_param;

static Vert tira[4];   // vértices pendientes de formar polígono
static int ntira, nvert_prim, impar;

// ---- matrices ----------------------------------------------------------------
static void identidad(float *m) {
    memset(m, 0, sizeof(Mat));
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void mult(float *dst, const float *a, const float *b) {  // dst = a * b
    Mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(dst, r, sizeof(Mat));
}

static void clip(float *m) { mult(m, pos, proj); }

// Aplica una matriz (cur = m * cur) según el modo
static void aplica(const float *m, int tambien_vec) {
    switch (modo_mtx) {
    case 0: mult(proj, m, proj); break;
    case 1: mult(pos, m, pos); break;
    case 2: mult(pos, m, pos); if (tambien_vec) mult(vec, m, vec); break;
    case 3: mult(tex, m, tex); break;
    }
}

static void carga(const float *m) {
    switch (modo_mtx) {
    case 0: memcpy(proj, m, sizeof(Mat)); break;
    case 1: memcpy(pos, m, sizeof(Mat)); break;
    case 2: memcpy(pos, m, sizeof(Mat)); memcpy(vec, m, sizeof(Mat)); break;
    case 3: memcpy(tex, m, sizeof(Mat)); break;
    }
}

static float f12(uint32_t v) { return (float)(int32_t)v / 4096.0f; }
static float s16_12(uint32_t v) { return (float)(int16_t)(v & 0xFFFF) / 4096.0f; }
static float s10(uint32_t v, int bit, float div) {
    int32_t x = (int32_t)((v >> bit) & 0x3FF);
    if (x & 0x200) x -= 0x400;
    return (float)x / div;
}

// ---- luces -------------------------------------------------------------------
static void color15(uint32_t c, float *r, float *g, float *b) {
    *r = (float)(c & 31); *g = (float)((c >> 5) & 31); *b = (float)((c >> 10) & 31);
}

static void normal(uint32_t p) {
    float n[3] = {s10(p, 0, 512), s10(p, 10, 512), s10(p, 20, 512)};
    // coordenadas de textura a partir de la normal (modo 2)
    if (((teximage >> 30) & 3) == 2) {
        ts = (n[0] * tex[0] + n[1] * tex[4] + n[2] * tex[8]) / 16 + ts0;  // el resultado va en 1/16 de texel
        tt = (n[0] * tex[1] + n[1] * tex[5] + n[2] * tex[9]) / 16 + tt0;
    }
    float tn[3];
    for (int j = 0; j < 3; j++) tn[j] = n[0] * vec[j] + n[1] * vec[4 + j] + n[2] * vec[8 + j];
    float dr, dg, db, ar, ag, ab, sr, sg, sb, er, eg, eb;
    color15(dif_amb, &dr, &dg, &db);
    color15(dif_amb >> 16, &ar, &ag, &ab);
    color15(spe_emi, &sr, &sg, &sb);
    color15(spe_emi >> 16, &er, &eg, &eb);
    float r = er, g = eg, b = eb;
    for (int l = 0; l < 4; l++) {
        if (!((attr >> l) & 1)) continue;
        float *L = luz_dir[l];
        float dif = -(L[0] * tn[0] + L[1] * tn[1] + L[2] * tn[2]);
        if (dif < 0) dif = 0;
        float h[3] = {(L[0]) / 2, (L[1]) / 2, (L[2] - 1) / 2};
        float spec = -(h[0] * tn[0] + h[1] * tn[1] + h[2] * tn[2]);
        if (spec < 0) spec = 0;
        spec = spec * spec;
        if (spec > 1) spec = 1;
        if ((spe_emi >> 15) & 1) spec = brillo_tabla[(int)(spec * 127)] / 255.0f;
        float *C = luz_col[l];
        r += C[0] * (dr * dif + sr * spec + ar) / 31;
        g += C[1] * (dg * dif + sg * spec + ag) / 31;
        b += C[2] * (db * dif + sb * spec + ab) / 31;
    }
    col_r = r > 31 ? 31 : r;
    col_g = g > 31 ? 31 : g;
    col_b = b > 31 ? 31 : b;
}

// ---- recorte contra la pirámide de visión ----------------------------------------
static Vert interp(const Vert *a, const Vert *b, float t) {
    Vert v;
    v.x = a->x + (b->x - a->x) * t; v.y = a->y + (b->y - a->y) * t;
    v.z = a->z + (b->z - a->z) * t; v.w = a->w + (b->w - a->w) * t;
    v.r = a->r + (b->r - a->r) * t; v.g = a->g + (b->g - a->g) * t; v.b = a->b + (b->b - a->b) * t;
    v.s = a->s + (b->s - a->s) * t; v.t = a->t + (b->t - a->t) * t;
    return v;
}

static float distancia(const Vert *v, int plano) {
    switch (plano) {
    case 0: return v->w + v->x;
    case 1: return v->w - v->x;
    case 2: return v->w + v->y;
    case 3: return v->w - v->y;
    case 4: return v->w + v->z;
    default: return v->w - v->z;
    }
}

static int recorta(Vert *v, int n) {
    Vert tmp[MAX_VERT_POLI + 6];
    Vert ent[MAX_VERT_POLI + 6];
    memcpy(ent, v, sizeof(Vert) * n);
    for (int p = 0; p < 6; p++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            Vert *a = &ent[i], *b = &ent[(i + 1) % n];
            float da = distancia(a, p), db = distancia(b, p);
            if (da >= 0) tmp[m++] = *a;
            if ((da >= 0) != (db >= 0)) tmp[m++] = interp(a, b, da / (da - db));
            if (m >= MAX_VERT_POLI + 5) break;
        }
        n = m;
        memcpy(ent, tmp, sizeof(Vert) * n);
        if (n < 3) return 0;
    }
    if (n > MAX_VERT_POLI) n = MAX_VERT_POLI;
    memcpy(v, ent, sizeof(Vert) * n);
    return n;
}

static void emite(Vert *v, int n) {
    if (npolis[lista] >= MAX_POLIS) return;
    Poli *p = &polis[lista][npolis[lista]];
    // también los que quedan fuera de la vista (con n = 0): así la lista de una escena y la de la siguiente
    // tienen la misma forma y se pueden emparejar para las imágenes intermedias
    p->ncrudo = n <= 4 ? n : 0;
    if (n <= 4) memcpy(p->crudo, v, sizeof(Vert) * n);
    memcpy(p->v, v, sizeof(Vert) * n);
    n = recorta(p->v, n);
    if (n < 3) n = 0;
    else nvisibles[lista]++;
    p->n = n;
    p->attr = attr;
    p->teximage = teximage;
    p->pltt = pltt_base;
    int alfa = (attr >> 16) & 31;
    int fmt = (teximage >> 26) & 7;
    p->translucido = (alfa > 0 && alfa < 31) || fmt == 1 || fmt == 6;
    npolis[lista]++;
}

static void vertice(float x, float y, float z) {
    vx = x; vy = y; vz = z;
    Mat c;
    clip(c);
    Vert v;
    v.x = x * c[0] + y * c[4] + z * c[8] + c[12];
    v.y = x * c[1] + y * c[5] + z * c[9] + c[13];
    v.z = x * c[2] + y * c[6] + z * c[10] + c[14];
    v.w = x * c[3] + y * c[7] + z * c[11] + c[15];
    v.r = col_r; v.g = col_g; v.b = col_b;
    if (((teximage >> 30) & 3) == 3) {  // coordenadas de textura desde el vértice
        v.s = (x * tex[0] + y * tex[4] + z * tex[8]) / 16 + ts0;
        v.t = (x * tex[1] + y * tex[5] + z * tex[9]) / 16 + tt0;
    } else {
        v.s = ts; v.t = tt;
    }
    tira[ntira++] = v;
    switch (tipo_prim) {
    case 0:  // triángulos
        if (ntira == 3) { emite(tira, 3); ntira = 0; }
        break;
    case 1:  // cuadriláteros
        if (ntira == 4) { emite(tira, 4); ntira = 0; }
        break;
    case 2:  // tira de triángulos
        if (ntira == 3) {
            Vert t[3] = {tira[0], tira[1], tira[2]};
            if (impar) { t[0] = tira[1]; t[1] = tira[0]; }
            emite(t, 3);
            impar ^= 1;
            tira[0] = tira[1]; tira[1] = tira[2];
            ntira = 2;
        }
        break;
    case 3:  // tira de cuadriláteros
        if (ntira == 4) {
            Vert q[4] = {tira[0], tira[1], tira[3], tira[2]};
            emite(q, 4);
            tira[0] = tira[2]; tira[1] = tira[3];
            ntira = 2;
        }
        break;
    }
    nvert_prim++;
}

// ---- órdenes -------------------------------------------------------------------
static const int n_param[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12,
    [0x18] = 16, [0x19] = 12, [0x1A] = 9, [0x1B] = 3, [0x1C] = 3,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1,
    [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
};
static const uint8_t orden_valida[256] = {
    [0x10] = 1, [0x11] = 1, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 1, [0x16] = 1, [0x17] = 1,
    [0x18] = 1, [0x19] = 1, [0x1A] = 1, [0x1B] = 1, [0x1C] = 1,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 1, [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1,
    [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 1,
    [0x40] = 1, [0x41] = 1, [0x50] = 1, [0x60] = 1, [0x70] = 1, [0x71] = 1, [0x72] = 1,
};

static float pos_res[4], vec_res[3];
static int caja_dentro = 1;

static void ejecuta(int cmd, const uint32_t *p) {
    Mat m;
    switch (cmd) {
    case 0x10: modo_mtx = p[0] & 3; break;
    case 0x11:  // PUSH
        if (modo_mtx == 0) { memcpy(pila_proj[0], proj, sizeof(Mat)); sp_proj = 1; }
        else if (modo_mtx == 3) { memcpy(pila_tex[0], tex, sizeof(Mat)); sp_tex = 1; }
        else {
            if (sp_pos >= 31) { error_pila = 1; break; }
            memcpy(pila_pos[sp_pos], pos, sizeof(Mat));
            memcpy(pila_vec[sp_pos], vec, sizeof(Mat));
            sp_pos++;
        }
        break;
    case 0x12: {  // POP
        if (modo_mtx == 0) { sp_proj = 0; memcpy(proj, pila_proj[0], sizeof(Mat)); }
        else if (modo_mtx == 3) { sp_tex = 0; memcpy(tex, pila_tex[0], sizeof(Mat)); }
        else {
            int n = (int)(p[0] & 63);
            if (n & 32) n -= 64;
            sp_pos -= n;
            if (sp_pos < 0) { sp_pos = 0; error_pila = 1; }
            if (sp_pos > 31) { sp_pos = 31; error_pila = 1; }
            memcpy(pos, pila_pos[sp_pos], sizeof(Mat));
            memcpy(vec, pila_vec[sp_pos], sizeof(Mat));
        }
        break;
    }
    case 0x13:  // STORE
        if (modo_mtx == 0) memcpy(pila_proj[0], proj, sizeof(Mat));
        else if (modo_mtx == 3) memcpy(pila_tex[0], tex, sizeof(Mat));
        else {
            int n = p[0] & 31;
            if (n == 31) error_pila = 1;
            memcpy(pila_pos[n], pos, sizeof(Mat));
            memcpy(pila_vec[n], vec, sizeof(Mat));
        }
        break;
    case 0x14:  // RESTORE
        if (modo_mtx == 0) memcpy(proj, pila_proj[0], sizeof(Mat));
        else if (modo_mtx == 3) memcpy(tex, pila_tex[0], sizeof(Mat));
        else {
            int n = p[0] & 31;
            memcpy(pos, pila_pos[n], sizeof(Mat));
            memcpy(vec, pila_vec[n], sizeof(Mat));
        }
        break;
    case 0x15: identidad(m); carga(m); break;
    case 0x16: for (int i = 0; i < 16; i++) m[i] = f12(p[i]); carga(m); break;
    case 0x17:
        identidad(m);
        for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) m[i * 4 + j] = f12(p[i * 3 + j]);
        carga(m);
        break;
    case 0x18: for (int i = 0; i < 16; i++) m[i] = f12(p[i]); aplica(m, 1); break;
    case 0x19:
        identidad(m);
        for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) m[i * 4 + j] = f12(p[i * 3 + j]);
        aplica(m, 1);
        break;
    case 0x1A:
        identidad(m);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i * 4 + j] = f12(p[i * 3 + j]);
        aplica(m, 1);
        break;
    case 0x1B:  // SCALE (no afecta a la matriz de vectores)
        identidad(m);
        m[0] = f12(p[0]); m[5] = f12(p[1]); m[10] = f12(p[2]);
        aplica(m, 0);
        break;
    case 0x1C:
        identidad(m);
        m[12] = f12(p[0]); m[13] = f12(p[1]); m[14] = f12(p[2]);
        aplica(m, 1);
        break;
    case 0x20: color15(p[0], &col_r, &col_g, &col_b); break;
    case 0x21: normal(p[0]); break;
    case 0x22: {
        float s = (float)(int16_t)(p[0] & 0xFFFF) / 16.0f, t = (float)(int16_t)(p[0] >> 16) / 16.0f;
        ts0 = s; tt0 = t;
        if (((teximage >> 30) & 3) == 1) {  // transformadas por la matriz de textura
            ts = s * tex[0] + t * tex[4] + (tex[8] + tex[12]) / 16;
            tt = s * tex[1] + t * tex[5] + (tex[9] + tex[13]) / 16;
        } else {
            ts = s; tt = t;
        }
        break;
    }
    case 0x23: vertice(s16_12(p[0]), s16_12(p[0] >> 16), s16_12(p[1])); break;
    case 0x24: vertice(s10(p[0], 0, 64), s10(p[0], 10, 64), s10(p[0], 20, 64)); break;
    case 0x25: vertice(s16_12(p[0]), s16_12(p[0] >> 16), vz); break;
    case 0x26: vertice(s16_12(p[0]), vy, s16_12(p[0] >> 16)); break;
    case 0x27: vertice(vx, s16_12(p[0]), s16_12(p[0] >> 16)); break;
    case 0x28:  // VTX_DIFF: 10 bits con signo que se suman tal cual en unidades de 4.12
        vertice(vx + s10(p[0], 0, 4096), vy + s10(p[0], 10, 4096), vz + s10(p[0], 20, 4096));
        break;
    case 0x29: attr_pendiente = p[0]; break;
    case 0x2A: teximage = p[0]; break;
    case 0x2B:
        pltt_base = p[0] & 0x1FFF;
        if (pltt_base < d_pl_min) d_pl_min = pltt_base;
        if (pltt_base > d_pl_max) d_pl_max = pltt_base;
        break;
    case 0x30:
        dif_amb = p[0];
        if (p[0] & 0x8000) color15(p[0], &col_r, &col_g, &col_b);
        break;
    case 0x31: spe_emi = p[0]; break;
    case 0x32: {
        int l = (p[0] >> 30) & 3;
        float L[3] = {s10(p[0], 0, 512), s10(p[0], 10, 512), s10(p[0], 20, 512)};
        for (int j = 0; j < 3; j++) luz_dir[l][j] = L[0] * vec[j] + L[1] * vec[4 + j] + L[2] * vec[8 + j];
        break;
    }
    case 0x33: {
        int l = (p[0] >> 30) & 3;
        color15(p[0], &luz_col[l][0], &luz_col[l][1], &luz_col[l][2]);
        break;
    }
    case 0x34: for (int i = 0; i < 32; i++) memcpy(brillo_tabla + i * 4, &p[i], 4); break;
    case 0x40:
        tipo_prim = p[0] & 3;
        attr = attr_pendiente;
        ntira = 0; nvert_prim = 0; impar = 0;
        break;
    case 0x41: break;
    case 0x50: swap_pendiente = 1; swap_param = p[0] & 3; break;
    case 0x60:
        vp_x1 = p[0] & 0xFF; vp_y1 = (p[0] >> 8) & 0xFF; vp_x2 = (p[0] >> 16) & 0xFF; vp_y2 = (p[0] >> 24) & 0xFF;
        break;
    case 0x70: caja_dentro = 1; break;  // BOX_TEST: siempre "se ve"
    case 0x71: {
        Mat c;
        clip(c);
        float x = s16_12(p[0]), y = s16_12(p[0] >> 16), z = s16_12(p[1]);
        vx = x; vy = y; vz = z;
        for (int j = 0; j < 4; j++) pos_res[j] = x * c[j] + y * c[4 + j] + z * c[8 + j] + c[12 + j];
        break;
    }
    case 0x72: {
        float n[3] = {s10(p[0], 0, 512), s10(p[0], 10, 512), s10(p[0], 20, 512)};
        for (int j = 0; j < 3; j++) vec_res[j] = n[0] * vec[j] + n[1] * vec[4 + j] + n[2] * vec[8 + j];
        break;
    }
    }
}

// FIFO: órdenes empaquetadas (4 órdenes por palabra, luego sus parámetros)
static uint32_t fifo_cmds;     // órdenes pendientes de la palabra empaquetada
static int fifo_ncmds;
static uint32_t params[32];
static int nparams, cmd_actual = -1;

static void siguiente_orden(void);

static void empieza_orden(int cmd) {
    cmd_actual = cmd;
    nparams = 0;
    if (!orden_valida[cmd] || n_param[cmd] == 0) {
        if (orden_valida[cmd]) ejecuta(cmd, params);
        cmd_actual = -1;
        siguiente_orden();
    }
}

static void siguiente_orden(void) {
    while (cmd_actual < 0 && fifo_ncmds > 0) {
        int c = fifo_cmds & 0xFF;
        fifo_cmds >>= 8;
        fifo_ncmds--;
        if (c == 0) continue;  // NOP
        cmd_actual = c;
        nparams = 0;
        if (!orden_valida[c] || n_param[c] == 0) {
            if (orden_valida[c]) ejecuta(c, params);
            cmd_actual = -1;
        }
    }
}

static void fifo_escribe(uint32_t v) {
    if (cmd_actual < 0) {
        if (v == 0) return;
        fifo_cmds = v;
        fifo_ncmds = 4;
        // quita los NOP del final
        while (fifo_ncmds > 0 && ((fifo_cmds >> (8 * (fifo_ncmds - 1))) & 0xFF) == 0) fifo_ncmds--;
        siguiente_orden();
        return;
    }
    params[nparams++] = v;
    if (nparams >= n_param[cmd_actual]) {
        ejecuta(cmd_actual, params);
        cmd_actual = -1;
        siguiente_orden();
    }
}

static uint32_t puerto_params[32];
static int puerto_n, puerto_cmd = -1;

void gx_escribe(uint32_t a, uint32_t v) {
    if (a < 0x04000440) { fifo_escribe(v); return; }
    if (a >= 0x04000600) return;
    int cmd = (int)((a - 0x04000400) >> 2);
    if (!orden_valida[cmd]) return;
    if (n_param[cmd] == 0) { ejecuta(cmd, puerto_params); return; }
    if (puerto_cmd != cmd) { puerto_cmd = cmd; puerto_n = 0; }
    puerto_params[puerto_n++] = v;
    if (puerto_n >= n_param[cmd]) {
        ejecuta(cmd, puerto_params);
        puerto_n = 0;
        puerto_cmd = -1;
    }
}

uint32_t gx_lee(uint32_t a) {
    Mat c;
    switch (a & ~3u) {
    case 0x04000600:  // GXSTAT: FIFO vacía, nada ocupado
        return (uint32_t)caja_dentro << 1 | (uint32_t)(sp_pos & 31) << 8 | (uint32_t)sp_proj << 13 |
               (uint32_t)error_pila << 15 | 0x06000000;
    case 0x04000604: return (uint32_t)nvisibles[lista] | (uint32_t)(nvisibles[lista] * 3) << 16;
    case 0x04000620: case 0x04000624: case 0x04000628: case 0x0400062C:
        return (uint32_t)(int32_t)(pos_res[(a - 0x04000620) / 4] * 4096);
    case 0x04000630: case 0x04000634:
        return (uint32_t)((int32_t)(vec_res[(a - 0x04000630) / 2] * 4096) & 0xFFFF) |
               (uint32_t)((int32_t)(vec_res[(a - 0x04000630) / 2 + 1] * 4096) << 16);
    }
    if (a >= 0x04000640 && a < 0x04000680) {
        clip(c);
        return (uint32_t)(int32_t)(c[(a - 0x04000640) / 4] * 4096);
    }
    if (a >= 0x04000680 && a < 0x040006A4) {
        int i = (int)(a - 0x04000680) / 4;
        return (uint32_t)(int32_t)(vec[(i / 3) * 4 + i % 3] * 4096);
    }
    return 0;
}

void gx_inicia(void) {
    identidad(proj); identidad(pos); identidad(vec); identidad(tex);
}

// ---- rasterizador ------------------------------------------------------------------
//
// Rápido y en paralelo:
//   - Cada triángulo se prepara una vez (ecuaciones de sus bordes y de sus
//     atributos) y se recorre sumando incrementos, fila a fila, solo dentro.
//   - La pantalla se reparte en NBANDAS bandas de filas, cada una en su hilo:
//     cada banda pinta todos los polígonos en orden, así que el resultado es
//     igual que pintando de uno en uno.
//   - Todo ocurre en un hilo aparte mientras el juego prepara la escena
//     siguiente. Si el juego termina otra escena antes de que acabe la
//     anterior, esa se salta (mejor perder un fotograma que frenar el juego).
#include "compat.h"

static inline uint8_t tex8(uint32_t o) {
    uint8_t *p = g_tex[(o >> 17) & 3];
    return p ? p[o & 0x1FFFF] : 0;
}
static inline uint16_t pal16(uint32_t o) {
    o %= 0x18000;  // 96 KB (6 slots de 16 KB); ojo: 0x17FFF NO sirve de máscara (borra el bit 15)
    uint8_t *p = g_texpal[o >> 14];
    return p ? (uint16_t)(p[o & 0x3FFF] | p[(o & 0x3FFF) + 1] << 8) : 0;
}

static inline uint16_t media15(uint16_t c0, uint16_t c1, int p0, int p1, int div) {
    return (uint16_t)((((c0 & 31) * p0 + (c1 & 31) * p1) / div) |
                      ((((c0 >> 5) & 31) * p0 + ((c1 >> 5) & 31) * p1) / div) << 5 |
                      ((((c0 >> 10) & 31) * p0 + ((c1 >> 10) & 31) * p1) / div) << 10);
}

// Color de un texel (RGB555) y su alfa 0..31
static uint16_t texel(uint32_t ti, uint32_t pltt, int s, int t, int *alfa) {
    int w = 8 << ((ti >> 20) & 7), h = 8 << ((ti >> 23) & 7);
    if ((ti >> 16) & 1) {  // repetir S
        if ((ti >> 18) & 1) { int per = s & (2 * w - 1); s = per >= w ? 2 * w - 1 - per : per; }
        else s &= w - 1;
    } else s = s < 0 ? 0 : s >= w ? w - 1 : s;
    if ((ti >> 17) & 1) {
        if ((ti >> 19) & 1) { int per = t & (2 * h - 1); t = per >= h ? 2 * h - 1 - per : per; }
        else t &= h - 1;
    } else t = t < 0 ? 0 : t >= h ? h - 1 : t;
    uint32_t base = (ti & 0xFFFF) * 8;
    int fmt = (ti >> 26) & 7;
    int trans0 = (ti >> 29) & 1;
    uint32_t pb = pltt * 16, i;
    *alfa = 31;
    switch (fmt) {
    case 1: {  // A3I5
        uint8_t b = tex8(base + (uint32_t)(t * w + s));
        int a = b >> 5;
        *alfa = a * 4 + a / 2;
        return pal16(pb + (b & 31) * 2);
    }
    case 2:  // 4 colores
        i = (tex8(base + (uint32_t)((t * w + s) / 4)) >> ((s & 3) * 2)) & 3;
        if (trans0 && i == 0) *alfa = 0;
        return pal16(pltt * 8 + i * 2);
    case 3:  // 16 colores
        i = (tex8(base + (uint32_t)((t * w + s) / 2)) >> ((s & 1) * 4)) & 15;
        if (trans0 && i == 0) *alfa = 0;
        return pal16(pb + i * 2);
    case 4:  // 256 colores
        i = tex8(base + (uint32_t)(t * w + s));
        if (trans0 && i == 0) *alfa = 0;
        return pal16(pb + i * 2);
    case 5: {  // comprimido en bloques de 4x4
        uint32_t datos = base + (uint32_t)((t / 4) * (w / 4) + s / 4) * 4;
        int idx = (tex8(datos + (uint32_t)(t & 3)) >> ((s & 3) * 2)) & 3;
        uint32_t slot = (datos & 0x7FFFF) >= 0x40000 ? 0x10000 : 0;  // datos en el slot 2: índices en slot 1 + 64 KB
        uint32_t info_dir = 0x20000 + slot + ((datos & 0x1FFFF) >> 1);
        uint16_t info = (uint16_t)(tex8(info_dir) | tex8(info_dir + 1) << 8);
        uint32_t pdir = pb + (uint32_t)(info & 0x3FFF) * 4;
        int modo = info >> 14;
        switch (idx) {
        case 0: return pal16(pdir);
        case 1: return pal16(pdir + 2);
        case 2:
            if (modo == 0 || modo == 2) return pal16(pdir + 4);
            if (modo == 1) return media15(pal16(pdir), pal16(pdir + 2), 1, 1, 2);
            return media15(pal16(pdir), pal16(pdir + 2), 5, 3, 8);
        default:
            if (modo == 2) return pal16(pdir + 6);
            if (modo == 3) return media15(pal16(pdir), pal16(pdir + 2), 3, 5, 8);
            *alfa = 0;
            return 0;
        }
    }
    case 6: {  // A5I3
        uint8_t b = tex8(base + (uint32_t)(t * w + s));
        *alfa = b >> 3;
        return pal16(pb + (b & 7) * 2);
    }
    case 7: {  // color directo
        uint32_t o = base + (uint32_t)(t * w + s) * 2;
        uint16_t c = (uint16_t)(tex8(o) | tex8(o + 1) << 8);
        if (!(c & 0x8000)) *alfa = 0;
        return c & 0x7FFF;
    }
    }
    return 0x7FFF;
}

typedef struct { float x, y, z, iw, r, g, b, s, t; } VP;  // vértice en pantalla (atributos / w)

typedef struct {  // polígono listo para pintar
    VP v[MAX_VERT_POLI];
    int n;
    uint32_t teximage, pltt;
    uint8_t modo, alfa, id, texturas, prof_igual, act_prof_trasl, translucido;
} PP;

typedef struct {  // lo que necesita el hilo de pintar (copiado al empezar la escena)
    uint8_t io[0x400];
    int wbuf, vp_x1, vp_y1, vp_x2, vp_y2, lst;
    int interp;  // se pueden hacer imágenes intermedias con la escena anterior
} Trabajo;

static PP pp[MAX_POLIS];
static int npp;
static Trabajo trabajo;

// Reescalado: el 3D se pinta a esc veces la resolución de la DS (MKDS_ESCALA, 1-4). salida_* es a tamaño de
// la DS (para el 2D, la captura y las repeticiones); salida_hd_* a tamaño grande (para la ventana).
#define MAXP (1024 * 768)
static int esc = 1, AN = 256, AL = 192;
static uint16_t color_buf[MAXP];   // RGB555
static uint8_t alfa_buf[MAXP];     // 0..31 (0 = nada)
static uint32_t prof_buf[MAXP];
static uint8_t id_buf[MAXP];
// Lo pintado: un anillo de 3 juegos. El hilo de pintar escribe el siguiente al actual; el montaje de la
// imagen (otro hilo) usa el que le toca durante un fotograma sin que se lo pisen.
typedef struct {
    uint16_t color[256 * 192];      // a tamaño de la DS
    uint8_t alfa[256 * 192];
    uint16_t hd_color[MAXP];        // a tamaño grande (reescalado)
    uint8_t hd_alfa[MAXP];
    uint16_t sub_color[3][MAXP];    // imágenes intermedias entre la escena anterior y esta (a tamaño 'esc')
    uint8_t sub_alfa[3][MAXP];
    int nsub;
} Salida;
static Salida salidas[3];
static volatile int salida_actual, polis_ultima;
static volatile uint64_t cont_frames, cont_saltadas;
volatile double g_render_ms_suma, g_render_ms_max;
volatile int g_render_n, g_render_sub;

// Pasa los polígonos de la lista a coordenadas de pantalla y descarta los que no se ven
static void prepara(const Trabajo *tj, const Poli *lst, int n) {
    uint16_t disp3d = (uint16_t)(tj->io[0x60] | tj->io[0x61] << 8);
    float vw = (float)(tj->vp_x2 - tj->vp_x1 + 1), vh = (float)(tj->vp_y2 - tj->vp_y1 + 1);
    npp = 0;
    for (int k = 0; k < n; k++) {
        const Poli *p = &lst[k];
        if (p->n < 3) continue;  // fuera de la vista
        int alfa = (p->attr >> 16) & 31, modo = (p->attr >> 4) & 3;
        if (alfa == 0 || modo == 3) continue;  // alambre y sombras: de momento no se pintan
        PP *q = &pp[npp];
        float area = 0;
        for (int i = 0; i < p->n; i++) {
            const Vert *a = &p->v[i];
            float w = a->w == 0 ? 1e-6f : a->w, iw = 1.0f / w;
            VP *v = &q->v[i];
            v->x = ((a->x * iw + 1) * vw / 2 + (float)tj->vp_x1) * (float)esc;
            v->y = ((1 - a->y * iw) * vh / 2 + (float)(191 - tj->vp_y2)) * (float)esc;
            v->z = tj->wbuf ? w : (a->z * iw + 1) / 2;
            v->iw = iw;
            v->r = a->r * iw; v->g = a->g * iw; v->b = a->b * iw;
            v->s = a->s * iw; v->t = a->t * iw;
        }
        for (int i = 0; i < p->n; i++) {
            const VP *a = &q->v[i], *b = &q->v[(i + 1) % p->n];
            area += a->x * b->y - b->x * a->y;
        }
        int delante = area < 0;
        if (delante && !((p->attr >> 7) & 1)) continue;
        if (!delante && !((p->attr >> 6) & 1)) continue;
        int fmt = (p->teximage >> 26) & 7;
        q->n = p->n;
        q->teximage = p->teximage;
        q->pltt = p->pltt;
        q->modo = (uint8_t)modo;
        q->alfa = (uint8_t)alfa;
        q->id = (uint8_t)((p->attr >> 24) & 63);
        q->texturas = (disp3d & 1) && fmt != 0;
        q->prof_igual = (p->attr >> 14) & 1;
        q->act_prof_trasl = (p->attr >> 11) & 1;
        q->translucido = (uint8_t)p->translucido;
        npp++;
    }
}

static void triangulo(const PP *p, const VP *A, const VP *B, const VP *C, int yb0, int yb1, const Trabajo *tj,
                      int capa_trasl) {
    float d = (B->x - A->x) * (C->y - A->y) - (C->x - A->x) * (B->y - A->y);
    if (fabsf(d) < 1e-6f) return;
    float id = 1.0f / d;
    int y0 = (int)floorf(fminf(A->y, fminf(B->y, C->y))), y1 = (int)ceilf(fmaxf(A->y, fmaxf(B->y, C->y)));
    int xa = (int)floorf(fminf(A->x, fminf(B->x, C->x))), xb = (int)ceilf(fmaxf(A->x, fmaxf(B->x, C->x)));
    if (y0 < yb0) y0 = yb0;
    if (y1 > yb1) y1 = yb1;
    if (xa < 0) xa = 0;
    if (xb > AN - 1) xb = AN - 1;
    if (y0 > y1 || xa > xb) return;
    // pesos baricéntricos de A y B como funciones lineales: l = (k + kx*px + ky*py) / d
    float k0 = B->x * C->y - C->x * B->y, k0x = B->y - C->y, k0y = C->x - B->x;
    float k1 = C->x * A->y - A->x * C->y, k1x = C->y - A->y, k1y = A->x - C->x;
    float l0x = k0x * id, l1x = k1x * id;
    // atributo = aC + l0*(aA - aC) + l1*(aB - aC)
#define DIF(c) float c##A = A->c - C->c, c##B = B->c - C->c, c##C = C->c
    DIF(z); DIF(iw); DIF(r); DIF(g); DIF(b); DIF(s); DIF(t);
#undef DIF
    const uint8_t *io = tj->io;
    uint16_t disp3d = (uint16_t)(io[0x60] | io[0x61] << 8);
    int prueba_alfa = (disp3d >> 2) & 1, ref_alfa = io[0x340] & 31, mezcla_alfa = (disp3d >> 3) & 1;
    int modo = p->modo, alfa_poli = p->alfa, idp = p->id, va = alfa_poli * 2 + 1;
    const float eps = -1e-4f;
    for (int y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        float px0 = (float)xa + 0.5f;
        float l0 = (k0 + k0x * px0 + k0y * py) * id, l1 = (k1 + k1x * px0 + k1y * py) * id;
        // tramo de la fila dentro del triángulo: l0, l1 y l2 = 1 - l0 - l1 >= 0
        float lo = 0, hi = (float)(xb - xa);
        float base[3] = {l0, l1, 1 - l0 - l1}, paso[3] = {l0x, l1x, -l0x - l1x};
        int fuera = 0;
        for (int e = 0; e < 3; e++) {
            if (paso[e] > 0) { float c = (eps - base[e]) / paso[e]; if (c > lo) lo = c; }
            else if (paso[e] < 0) { float c = (eps - base[e]) / paso[e]; if (c < hi) hi = c; }
            else if (base[e] < eps) fuera = 1;
        }
        if (fuera || lo > hi) continue;
        int xi = xa + (int)ceilf(lo), xf = xa + (int)floorf(hi);
        if (xi < xa) xi = xa;
        if (xf > xb) xf = xb;
        l0 += l0x * (float)(xi - xa);
        l1 += l1x * (float)(xi - xa);
        for (int x = xi; x <= xf; x++, l0 += l0x, l1 += l1x) {
            int i = y * AN + x;
            float w = 1.0f / (iwC + l0 * iwA + l1 * iwB);
            // w-buffer: w no es lineal en pantalla (1/w sí), así que se usa el w ya corregido
            uint32_t zi = tj->wbuf ? (uint32_t)(w * 0x1000) : (uint32_t)((zC + l0 * zA + l1 * zB) * 0xFFFFFF);
            if (p->prof_igual ? (zi > prof_buf[i] + 0x200 || zi + 0x200 < prof_buf[i]) : zi >= prof_buf[i]) continue;
            int vr = (int)((rC + l0 * rA + l1 * rB) * w * 2 + 0.5f);
            int vg = (int)((gC + l0 * gA + l1 * gB) * w * 2 + 0.5f);
            int vb = (int)((bC + l0 * bA + l1 * bB) * w * 2 + 0.5f);
            int cr = vr, cg = vg, cb = vb, ca = va;
            if (modo == 2) {  // toon / resaltado: tabla según el rojo
                uint32_t o = 0x380 + (uint32_t)(vr >> 1) * 2;
                uint16_t tc = (uint16_t)(io[o] | io[o + 1] << 8);
                if ((disp3d >> 1) & 1) {
                    cr = vr + (tc & 31) * 2; cg = vr + ((tc >> 5) & 31) * 2; cb = vr + ((tc >> 10) & 31) * 2;
                    if (cr > 63) cr = 63;
                    if (cg > 63) cg = 63;
                    if (cb > 63) cb = 63;
                } else {
                    cr = (tc & 31) * 2 + 1; cg = ((tc >> 5) & 31) * 2 + 1; cb = ((tc >> 10) & 31) * 2 + 1;
                }
                vr = cr; vg = cg; vb = cb;
            }
            if (p->texturas) {
                float s = (sC + l0 * sA + l1 * sB) * w, t = (tC + l0 * tA + l1 * tB) * w;
                int ta;
                uint16_t tcol = texel(p->teximage, p->pltt, (int)floorf(s), (int)floorf(t), &ta);
                int tr = (tcol & 31) * 2 + ((tcol & 31) ? 1 : 0);
                int tg = ((tcol >> 5) & 31) * 2 + (((tcol >> 5) & 31) ? 1 : 0);
                int tb = ((tcol >> 10) & 31) * 2 + (((tcol >> 10) & 31) ? 1 : 0);
                int ta6 = ta * 2 + (ta ? 1 : 0);
                if (modo == 1) {  // calcomanía
                    cr = (tr * ta6 + vr * (63 - ta6)) / 64;
                    cg = (tg * ta6 + vg * (63 - ta6)) / 64;
                    cb = (tb * ta6 + vb * (63 - ta6)) / 64;
                } else {  // modular (y toon con textura)
                    cr = ((tr + 1) * (vr + 1) - 1) / 64;
                    cg = ((tg + 1) * (vg + 1) - 1) / 64;
                    cb = ((tb + 1) * (vb + 1) - 1) / 64;
                    ca = ((ta6 + 1) * (va + 1) - 1) / 64;
                }
            }
            int a5 = ca >> 1;
            if (a5 == 0) continue;
            if (prueba_alfa && a5 <= ref_alfa) continue;
            uint16_t col = (uint16_t)((cr >> 1) | (cg >> 1) << 5 | (cb >> 1) << 10);
            if (a5 < 31 && mezcla_alfa) {
                if (capa_trasl && id_buf[i] == idp && alfa_buf[i] < 31 && alfa_buf[i] > 0) continue;
                if (alfa_buf[i]) {
                    uint16_t d0 = color_buf[i];
                    int dr = d0 & 31, dg = (d0 >> 5) & 31, db = (d0 >> 10) & 31;
                    int nr = ((cr >> 1) * (a5 + 1) + dr * (31 - a5)) / 32;
                    int ng = ((cg >> 1) * (a5 + 1) + dg * (31 - a5)) / 32;
                    int nb = ((cb >> 1) * (a5 + 1) + db * (31 - a5)) / 32;
                    col = (uint16_t)(nr | ng << 5 | nb << 10);
                    a5 = a5 > alfa_buf[i] ? a5 : alfa_buf[i];
                }
                if (p->act_prof_trasl) prof_buf[i] = zi;
            } else {
                prof_buf[i] = zi;
            }
            color_buf[i] = col;
            alfa_buf[i] = (uint8_t)a5;
            id_buf[i] = (uint8_t)idp;
        }
    }
}

static void pinta_banda(int yb0, int yb1) {
    const Trabajo *tj = &trabajo;
    const uint8_t *io = tj->io;
    uint32_t clear = (uint32_t)(io[0x350] | io[0x351] << 8 | io[0x352] << 16 | io[0x353] << 24);
    uint16_t clear_prof = (uint16_t)((io[0x354] | io[0x355] << 8) & 0x7FFF);
    uint16_t cc = clear & 0x7FFF;
    uint8_t ca = (clear >> 16) & 31, cid = (clear >> 24) & 63;
    uint32_t cz = (uint32_t)clear_prof * 0x200 + 0x1FF;
    for (int i = yb0 * AN; i < (yb1 + 1) * AN; i++) {
        color_buf[i] = cc; alfa_buf[i] = ca; prof_buf[i] = cz; id_buf[i] = cid;
    }
    for (int pasada = 0; pasada < 2; pasada++)  // primero los opacos, luego los translúcidos
        for (int k = 0; k < npp; k++) {
            const PP *p = &pp[k];
            if (p->translucido != pasada) continue;
            for (int j = 1; j + 1 < p->n; j++) triangulo(p, &p->v[0], &p->v[j], &p->v[j + 1], yb0, yb1, tj, pasada);
        }
}

#define NBANDAS 24
static HANDLE ev_banda_ir[NBANDAS], ev_banda_hecha[NBANDAS], ev_trabajo, ev_libre;
static int nbandas = NBANDAS;  // tantas como núcleos menos uno (el del juego), como mucho NBANDAS

static DWORD WINAPI hilo_banda(LPVOID arg) {
    int b = (int)(intptr_t)arg;
    int filas = AL / nbandas;
    for (;;) {
        WaitForSingleObject(ev_banda_ir[b], INFINITE);
        pinta_banda(b * filas, b == nbandas - 1 ? AL - 1 : b * filas + filas - 1);
        SetEvent(ev_banda_hecha[b]);
    }
}

// ---- imágenes intermedias (MKDS_FPS = 120, 180 o 240) -----------------------------------
// El juego va a 60. Entre la escena anterior y la nueva se pintan 1, 2 o 3 escenas intermedias: cada
// polígono de la nueva se empareja con el de la anterior (mismo orden, misma textura, paleta y tipo; se
// toleran polígonos que aparecen o desaparecen) y sus vértices sin recortar se mezclan.
static int sub_por_escena = -1;  // imágenes intermedias por escena (MKDS_FPS / 60 - 1)
static struct { Vert v[4]; int n; uint32_t attr, teximage, pltt; } ant[MAX_POLIS];  // la escena anterior
static int nant;
static int pareja[MAX_POLIS];  // para cada polígono de la escena: su pareja en la anterior, o -1
static Poli intermedia[MAX_POLIS];

static int mismo(const Poli *p, int j) {
    return ant[j].n == p->ncrudo && ant[j].teximage == p->teximage && ant[j].pltt == p->pltt &&
           (ant[j].attr & 0xFFFF) == (p->attr & 0xFFFF);
}

// posición en pantalla (-1..1) de un vértice; detrás de la cámara, lo más parecido que se puede
static void ndc(const Vert *v, float *x, float *y) {
    float w = fabsf(v->w) > 1e-3f ? fabsf(v->w) : 1e-3f;
    *x = v->x / w;
    *y = v->y / w;
}

// cuánto se ha movido en pantalla un polígono respecto al de la escena anterior, sin contar el movimiento
// general de la escena (gx, gy: la cámara), por vértice; y su tamaño
static float desplazamiento(const Poli *p, int j, float gx, float gy, float *tam) {
    float d = 0, x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int k = 0; k < p->ncrudo; k++) {
        float ax, ay, bx, by;
        ndc(&ant[j].v[k], &ax, &ay);
        ndc(&p->crudo[k], &bx, &by);
        d += fabsf(bx - ax - gx) + fabsf(by - ay - gy);
        if (bx < x0) x0 = bx;
        if (bx > x1) x1 = bx;
        if (by < y0) y0 = by;
        if (by > y1) y1 = by;
    }
    *tam = (x1 - x0) + (y1 - y0);
    return d / (float)p->ncrudo;
}

// Pareja de cada polígono en la escena anterior. Entre los iguales (misma textura, paleta y tipo) de alrededor
// de su sitio en la lista se elige el que estaba MÁS CERCA en pantalla: con muchos objetos iguales (cubos de
// objetos, tuberías) ir solo por orden emparejaba caras de un cubo con las de otro cuando el juego cambiaba qué
// cubos dibuja al acercarse, y en la imagen intermedia salían estiradas. Si ni el más cercano está cerca, sin pareja.
static uint8_t usado[MAX_POLIS];

static float movs_x[MAX_POLIS], movs_y[MAX_POLIS];
static int por_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

// movimiento general de la escena (sobre todo la cámara): la mediana del desplazamiento de los polígonos que
// están en el mismo sitio de la lista que en la escena anterior
static void movimiento_general(const Poli *lst, int n, float *gx, float *gy) {
    int m = 0;
    for (int i = 0; i < n && i < nant; i++) {
        if (lst[i].ncrudo < 3 || !mismo(&lst[i], i)) continue;
        float ax, ay, bx, by;
        ndc(&ant[i].v[0], &ax, &ay);
        ndc(&lst[i].crudo[0], &bx, &by);
        movs_x[m] = bx - ax;
        movs_y[m] = by - ay;
        m++;
    }
    *gx = *gy = 0;
    if (!m) return;
    qsort(movs_x, (size_t)m, sizeof(float), por_float);
    qsort(movs_y, (size_t)m, sizeof(float), por_float);
    *gx = movs_x[m / 2];
    *gy = movs_y[m / 2];
}

static int empareja(const Poli *lst, int n) {
    int j = 0, hechos = 0;
    memset(usado, 0, (size_t)nant);
    float gx, gy;
    movimiento_general(lst, n, &gx, &gy);
    for (int i = 0; i < n; i++) {
        pareja[i] = -1;
        if (lst[i].ncrudo < 3) continue;
        static int en_orden = -1;  // MKDS_EMPAREJA_ORDEN=1: como antes, el primero igual (para comparar)
        if (en_orden < 0) en_orden = getenv("MKDS_EMPAREJA_ORDEN") != NULL;
        if (en_orden) {
            for (int k = j; k < nant && k < j + 48; k++)
                if (mismo(&lst[i], k)) { pareja[i] = k; j = k + 1; hechos++; break; }
            continue;
        }
        // primero la pareja en orden (la siguiente igual): en un personaje, hecho de muchos triángulos iguales y
        // pegados, el más cercano solía ser el triángulo de al lado y el cuerpo se deshacía en las intermedias
        int en_su_sitio = -1;
        static int solo_cercano = -1;  // MKDS_EMPAREJA_CERCANO=1: sin este paso (para comparar)
        if (solo_cercano < 0) solo_cercano = getenv("MKDS_EMPAREJA_CERCANO") != NULL;
        for (int k = j; !solo_cercano && k < nant && k < j + 48; k++)
            if (!usado[k] && mismo(&lst[i], k)) { en_su_sitio = k; break; }
        if (en_su_sitio >= 0) {
            float t, d = desplazamiento(&lst[i], en_su_sitio, gx, gy, &t);
            if (d <= 0.08f + 0.25f * t) {
                pareja[i] = en_su_sitio;
                usado[en_su_sitio] = 1;
                j = en_su_sitio + 1;
                hechos++;
                continue;
            }
        }
        // si no encaja (cubos iguales que el juego dibuja en otro orden), el igual que menos se ha movido
        int mejor = -1;
        float mejor_d = 1e9f, tam = 0;
        for (int k = j > 16 ? j - 16 : 0; k < nant && k < j + 64; k++) {
            if (usado[k] || !mismo(&lst[i], k)) continue;
            float t, d = desplazamiento(&lst[i], k, gx, gy, &t);
            if (d < mejor_d) { mejor_d = d; mejor = k; tam = t; }
        }
        // se tiene que mover casi como la escena (un objeto que "salta" a otro igual no lo hace)
        if (mejor < 0 || mejor_d > 0.08f + 0.25f * tam) continue;
        pareja[i] = mejor;
        usado[mejor] = 1;
        j = mejor + 1;
        hechos++;
    }
    return hechos;
}

static int interpola(const Poli *lst, int n, float t) {
    for (int i = 0; i < n; i++) {
        Poli *q = &intermedia[i];
        *q = lst[i];
        int j = pareja[i];
        if (j < 0) continue;  // nuevo: tal cual
        Vert v[4];
        int lejos = 0;
        for (int k = 0; k < q->ncrudo; k++) {
            const Vert *a = &ant[j].v[k], *b = &lst[i].crudo[k];
            float esc_w = fmaxf(fabsf(a->w), fabsf(b->w)) + 1e-3f;
            if ((fabsf(a->x - b->x) + fabsf(a->y - b->y)) / esc_w > 0.6f) lejos = 1;  // salto (cámara, reaparece)
            v[k].x = a->x + (b->x - a->x) * t; v[k].y = a->y + (b->y - a->y) * t;
            v[k].z = a->z + (b->z - a->z) * t; v[k].w = a->w + (b->w - a->w) * t;
            v[k].r = a->r + (b->r - a->r) * t; v[k].g = a->g + (b->g - a->g) * t; v[k].b = a->b + (b->b - a->b) * t;
            v[k].s = a->s + (b->s - a->s) * t; v[k].t = a->t + (b->t - a->t) * t;
        }
        if (lejos) continue;
        memcpy(q->v, v, sizeof(Vert) * q->ncrudo);
        q->n = recorta(q->v, q->ncrudo);
        if (q->n < 3) q->n = 0;
    }
    return n;
}

static void guarda_anterior(const Poli *lst, int n) {
    for (int i = 0; i < n; i++) {
        memcpy(ant[i].v, lst[i].crudo, sizeof(Vert) * 4);
        ant[i].n = lst[i].ncrudo;
        ant[i].attr = lst[i].attr; ant[i].teximage = lst[i].teximage; ant[i].pltt = lst[i].pltt;
    }
    nant = n;
}

static int nvis_de(const Poli *lst, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v += lst[i].ncrudo >= 3;
    return v;
}

static void pinta_todo(void) {
    for (int b = 0; b < nbandas; b++) SetEvent(ev_banda_ir[b]);
    WaitForMultipleObjects(nbandas, ev_banda_hecha, TRUE, INFINITE);
}

static double ms_imagen;  // lo que tarda en pintarse una imagen (media móvil), para las intermedias

static DWORD WINAPI hilo_render(LPVOID arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(ev_trabajo, INFINITE);
        LARGE_INTEGER t0, t1, fr;
        QueryPerformanceCounter(&t0);
        const Poli *lst = polis[trabajo.lst];
        int n = npolis[trabajo.lst];
        int otra = (salida_actual + 1) % 3;
        Salida *S = &salidas[otra];
        int nsub = trabajo.interp && nant > 0 ? sub_por_escena : 0;
        // En un PC justo no da tiempo a pintar todas las intermedias antes de la escena siguiente, y entonces
        // se saltaban escenas de verdad (el 3D a tirones). Se pintan las que quepan en ~3/4 de un fotograma
        // según lo que ha tardado cada imagen hasta ahora.
        if (nsub && ms_imagen > 0) {
            int caben = (int)(12.5 / ms_imagen) - 1;
            if (caben < nsub) nsub = caben < 0 ? 0 : caben;
        }
        // casi nada empareja (corte de cámara, otra pantalla): esta vez sin intermedias
        if (nsub && empareja(lst, n) * 2 < nvis_de(lst, n)) nsub = 0;
        for (int j = 1; j <= nsub; j++) {
            int m = interpola(lst, n, (float)j / (float)(nsub + 1));
            prepara(&trabajo, intermedia, m);
            pinta_todo();
            memcpy(S->sub_color[j - 1], color_buf, (size_t)AN * AL * 2);
            memcpy(S->sub_alfa[j - 1], alfa_buf, (size_t)AN * AL);
        }
        S->nsub = nsub;
        prepara(&trabajo, lst, n);
        pinta_todo();
        guarda_anterior(lst, n);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fr);
        {  // para MKDS_MIDE: cuánto tarda en pintarse una escena
            double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fr.QuadPart;
            double una = ms / (nsub + 1);
            ms_imagen = ms_imagen > 0 ? ms_imagen * 0.9 + una * 0.1 : una;
            g_render_ms_suma += ms; g_render_n++; g_render_sub += nsub;
            if (ms > g_render_ms_max) g_render_ms_max = ms;
        }
        if (esc == 1) {
            memcpy(S->color, color_buf, sizeof S->color);
            memcpy(S->alfa, alfa_buf, sizeof S->alfa);
        } else {
            memcpy(S->hd_color, color_buf, (size_t)AN * AL * 2);
            memcpy(S->hd_alfa, alfa_buf, (size_t)AN * AL);
            for (int y = 0; y < 192; y++)  // a tamaño de la DS: el punto central de cada bloque
                for (int x = 0; x < 256; x++) {
                    int i = (y * esc + esc / 2) * AN + x * esc + esc / 2;
                    S->color[y * 256 + x] = color_buf[i];
                    S->alfa[y * 256 + x] = alfa_buf[i];
                }
        }
        salida_actual = otra;
        polis_ultima = npp;
        cont_frames++;
        SetEvent(ev_libre);
    }
}

int video_escala(void);

static void arranca_hilos(void) {
    esc = video_escala();
    const char *fps = getenv("MKDS_FPS");
    sub_por_escena = fps ? atoi(fps) / 60 - 1 : 0;
    if (sub_por_escena < 0) sub_por_escena = 0;
    if (sub_por_escena > 3) sub_por_escena = 3;
    AN = 256 * esc;
    AL = 192 * esc;
    ev_trabajo = CreateEventW(NULL, FALSE, FALSE, NULL);
    ev_libre = CreateEventW(NULL, FALSE, TRUE, NULL);
    DWORD_PTR del_proceso = 0, del_sistema = 0;  // núcleos que puede usar el juego
    GetProcessAffinityMask(GetCurrentProcess(), &del_proceso, &del_sistema);
    // Con más bandas que núcleos el juego y la música se quedaban sin turno y se saltaban más escenas
    // (probado limitando el juego a 2 y 4 núcleos). MKDS_BANDAS=n las fija, para pruebas.
    nbandas = -1;
    for (; del_proceso; del_proceso &= del_proceso - 1) nbandas++;
    if (getenv("MKDS_BANDAS")) nbandas = atoi(getenv("MKDS_BANDAS"));
    if (nbandas < 1) nbandas = 1;
    if (nbandas > NBANDAS) nbandas = NBANDAS;
    for (int b = 0; b < nbandas; b++) {
        ev_banda_ir[b] = CreateEventW(NULL, FALSE, FALSE, NULL);
        ev_banda_hecha[b] = CreateEventW(NULL, FALSE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, hilo_banda, (LPVOID)(intptr_t)b, 0, NULL));
    }
    HANDLE h = CreateThread(NULL, 0, hilo_render, NULL, 0, NULL);
    SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(h);
}

// Con la captura encendida se muestra la escena anterior a la que se acaba de pedir: en la consola la imagen
// 3D de un fotograma es la que el juego preparó en el anterior, y así cada vista cae en su pantalla (si no, en
// la presentación del circuito la vista de arriba salía abajo y al revés). Se fija cuál (indice_fijo) en vez
// de esperar a que se pinte la nueva: esa espera frenaba al ARM7 y la música en la presentación.
static volatile int fijo, indice_fijo;

void gx_vblank(const uint8_t *io9) {
    int captura = (io9[0x67] & 0x80) != 0;
    if (!captura) fijo = 0;
    if (!swap_pendiente) return;
    swap_pendiente = 0;
    if (!ev_trabajo) arranca_hilos();
    // Con la captura de pantalla encendida (3D en las dos pantallas: presentación del circuito, ceremonia)
    // el juego alterna cada fotograma la vista de arriba y la de abajo. Ahí la escena se pinta al momento:
    // si saliera un fotograma tarde (lo normal, para no frenar el juego) se mezclarían las dos vistas.
    int al_momento = captura;
    if (WaitForSingleObject(ev_libre, al_momento ? INFINITE : 0) != WAIT_OBJECT_0) {
        // aún se está pintando la escena anterior: esta se salta y el juego no espera
        npolis[lista] = nvisibles[lista] = 0;
        cont_saltadas++;
        return;
    }
    memcpy(trabajo.io, io9, sizeof trabajo.io);
    trabajo.wbuf = (swap_param >> 1) & 1;
    trabajo.vp_x1 = vp_x1; trabajo.vp_y1 = vp_y1; trabajo.vp_x2 = vp_x2; trabajo.vp_y2 = vp_y2;
    trabajo.lst = lista;
    lista ^= 1;            // la otra lista se empieza a llenar ya
    npolis[lista] = nvisibles[lista] = 0;
    trabajo.interp = !al_momento;  // con la captura encendida (presentación) no hay imágenes intermedias
    if (al_momento) {  // la anterior ya está pintada (se esperó arriba): esa es la que se enseña
        indice_fijo = salida_actual;
        fijo = 1;
    }
    SetEvent(ev_trabajo);
}

int gx_indice(void) { return fijo ? indice_fijo : salida_actual; }
const uint16_t *gx_color(void) { return salidas[gx_indice()].color; }
const uint8_t *gx_alfa(void) { return salidas[gx_indice()].alfa; }
int gx_polis(void) { return polis_ultima; }
const uint16_t *gx_hd_color(void) { return salidas[gx_indice()].hd_color; }
const uint8_t *gx_hd_alfa(void) { return salidas[gx_indice()].hd_alfa; }
// imágenes intermedias del juego de salidas i (de la escena anterior a esa): cuántas y cada una
int gx_nsub(int i) { return salidas[i].nsub; }
const uint16_t *gx_sub_color(int i, int j) { return salidas[i].sub_color[j]; }
const uint8_t *gx_sub_alfa(int i, int j) { return salidas[i].sub_alfa[j]; }
uint64_t gx_frames(void) { return cont_frames; }

void gx_depura(void) {
    fprintf(stderr, "    3D: %d polígonos pintados en la última escena, %llu escenas saltadas por ir lento\n",
            polis_ultima, (unsigned long long)cont_saltadas);
}
