// 3D por la GPU (experimental; de momento solo en Windows). Pinta la misma lista de polígonos que el rasterizador
// de gpu3d.c (ya recortados, en pantalla y sin los que no miran a la cámara) con OpenGL 2.1, en un contexto propio
// y oculto del hilo de pintar, y lee la imagen a los mismos búferes (RGB555 y alfa 0..31). Así todo lo demás (el
// 2D, la captura, las repeticiones, las imágenes intermedias) sigue igual. Si algo falla al empezar (sin OpenGL 2,
// sin framebuffers...), devuelve 0 y gpu3d.c pinta por la CPU como siempre.
//
// Lo que hace la DS y cómo va aquí:
//   - color: el shader hace las mismas cuentas que triangulo() (6 bits, modular / calcomanía / toon y resaltado)
//   - profundidad: z-buffer con gl_FragCoord.z (lineal en pantalla, igual que la CPU); w-buffer escribiendo w
//     en gl_FragDepth. "Profundidad igual" (calcomanías sobre el suelo): un poco por delante (solo por un lado)
//   - translúcidos: la imagen guarda el color multiplicado por el alfa; así, sobre lo vacío el color queda tal
//     cual y sobre lo opaco se mezcla. El alfa, el mayor de los dos. Al leer se divide.
//   - mismo ID translúcido sobre sí mismo: no se pinta (plantilla con ID + 1)
#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gl3d_pinta(const void *lista, int n, int tam_pp, const uint8_t *io, int wbuf, uint16_t *color, uint8_t *alfa,
               int an, int al);

#ifndef _WIN32
int gl3d_pinta(const void *lista, int n, int tam_pp, const uint8_t *io, int wbuf, uint16_t *color, uint8_t *alfa,
               int an, int al) {
    (void)lista; (void)n; (void)tam_pp; (void)io; (void)wbuf; (void)color; (void)alfa; (void)an; (void)al;
    return 0;  // en Linux y macOS, por la CPU
}
#else
#include <GL/gl.h>

// gpu3d.c: las texturas (de la VRAM) y su "huella" para saber si han cambiado
uint64_t gx_tex_huella(uint32_t ti, uint32_t pltt);
void gx_tex_decodifica(uint32_t ti, uint32_t pltt, uint8_t *rgba);

// Igual que VP y PP de gpu3d.c (se comprueba el tamaño)
typedef struct { float x, y, z, iw, r, g, b, s, t; } VP;
typedef struct {
    VP v[10];
    int n;
    uint32_t teximage, pltt;
    uint8_t modo, alfa, id, texturas, prof_igual, act_prof_trasl, translucido;
} PP;

// ---- OpenGL 2: lo que no trae opengl32.dll se pide al driver --------------------------------------------------
typedef char GLchar;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_MIRRORED_REPEAT 0x8370
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FUNC_ADD 0x8006
#define GL_MAX 0x8008
#define GL_TEXTURE0 0x84C0

#define FUNCIONES(X) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glBlendEquationSeparate, (GLenum, GLenum)) \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, glActiveTexture, (GLenum))
#define DECLARA(r, f, a) static r(APIENTRY *p_##f) a;
FUNCIONES(DECLARA)

static int carga_funciones(void) {
    int ok = 1;
#define CARGA(r, f, a) \
    p_##f = (r(APIENTRY *) a)(void (*)(void))wglGetProcAddress(#f); \
    if (!p_##f) p_##f = (r(APIENTRY *) a)(void (*)(void))wglGetProcAddress(#f "EXT"); \
    if (!p_##f) { fprintf(stderr, "[gpu] falta %s\n", #f); ok = 0; }
    FUNCIONES(CARGA)
    return ok;
}

// ---- el contexto (una ventana oculta del hilo de pintar) -----------------------------------------------------------
static HWND vent;
static HDC dc;
static int estado;  // 0 sin empezar, 1 listo, -1 no se puede (por la CPU)
static GLuint prog, fbo, tex_color, rb_prof;
static int fbo_an, fbo_al;
static GLint u_tex, u_toon, u_resaltado, u_prueba, u_ref, u_wbuf, u_premult;

static const char *VS =
    "#version 120\n"
    "attribute vec4 a_pos;\n"
    "attribute vec3 a_col;\n"   // color del vértice 0..31
    "attribute vec2 a_st;\n"    // coordenadas de textura en texels
    "attribute vec4 a_par;\n"   // modo, alfa del polígono 0..31, con textura, profundidad igual
    "attribute vec2 a_tam;\n"   // tamaño de la textura
    "varying vec3 v_col; varying vec2 v_st; varying vec4 v_par; varying vec2 v_tam;\n"
    "void main() { gl_Position = a_pos; v_col = a_col; v_st = a_st; v_par = a_par; v_tam = a_tam; }\n";

// Las mismas cuentas que triangulo() en gpu3d.c (colores de 6 bits, 0..63)
static const char *FS =
    "#version 120\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec3 u_toon[32];\n"
    "uniform int u_resaltado, u_prueba, u_wbuf, u_premult;\n"
    "uniform float u_ref;\n"
    "varying vec3 v_col; varying vec2 v_st; varying vec4 v_par; varying vec2 v_tam;\n"
    "void main() {\n"
    "  int modo = int(v_par.x + 0.5);\n"
    "  vec3 v = clamp(floor(v_col * 2.0 + 0.5), 0.0, 63.0);\n"
    "  vec3 c = v;\n"
    "  float va = floor(v_par.y + 0.5) * 2.0 + 1.0, ca = va;\n"
    "  if (modo == 2) {\n"
    "    vec3 t = u_toon[int(v.r / 2.0)];\n"
    "    c = u_resaltado != 0 ? min(v.r + t * 2.0, vec3(63.0)) : t * 2.0 + 1.0;\n"
    "    v = c;\n"
    "  }\n"
    "  if (v_par.z > 0.5) {\n"
    "    vec4 tx = texture2D(u_tex, v_st / v_tam);\n"
    "    vec3 t5 = floor(tx.rgb * 31.0 + 0.5);\n"
    "    float a5 = floor(tx.a * 31.0 + 0.5);\n"
    "    vec3 t6 = t5 * 2.0 + sign(t5);\n"
    "    float ta6 = a5 * 2.0 + sign(a5);\n"
    "    if (modo == 1) c = floor((t6 * ta6 + v * (63.0 - ta6)) / 64.0);\n"
    "    else { c = floor(((t6 + 1.0) * (v + 1.0) - 1.0) / 64.0); ca = floor(((ta6 + 1.0) * (va + 1.0) - 1.0) / 64.0); }\n"
    "  }\n"
    "  float a5 = floor(ca / 2.0);\n"
    "  if (a5 < 1.0 || (u_prueba != 0 && a5 <= u_ref)) discard;\n"
    "  float a = a5 / 31.0;\n"
    "  vec3 col = floor(c / 2.0) / 31.0;\n"
    "  gl_FragColor = vec4(u_premult != 0 ? col * a : col, a);\n"
    "  float z = u_wbuf != 0 ? (1.0 / gl_FragCoord.w) * (4096.0 / 16777215.0) : gl_FragCoord.z;\n"
    "  gl_FragDepth = clamp(z - v_par.w * (512.0 / 16777215.0), 0.0, 1.0);\n"
    "}\n";

static GLuint compila(GLenum tipo, const char *src) {
    GLuint s = p_glCreateShader(tipo);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[gpu] shader: %s\n", log);
        return 0;
    }
    return s;
}

enum { A_POS, A_COL, A_ST, A_PAR, A_TAM };

static int empieza(void) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"DSKartRecompiled3D";
    RegisterClassW(&wc);
    vent = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
    if (!vent) return 0;
    dc = GetDC(vent);
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return 0;
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) return 0;
    const char *ver = (const char *)glGetString(GL_VERSION);
    fprintf(stderr, "[gpu] OpenGL %s (%s)\n", ver ? ver : "?", (const char *)glGetString(GL_RENDERER));
    if (!ver || ver[0] < '2' || !carga_funciones()) return 0;
    GLuint vs = compila(GL_VERTEX_SHADER, VS), fs = compila(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return 0;
    prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    p_glBindAttribLocation(prog, A_POS, "a_pos");
    p_glBindAttribLocation(prog, A_COL, "a_col");
    p_glBindAttribLocation(prog, A_ST, "a_st");
    p_glBindAttribLocation(prog, A_PAR, "a_par");
    p_glBindAttribLocation(prog, A_TAM, "a_tam");
    p_glLinkProgram(prog);
    GLint ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "[gpu] programa: %s\n", log);
        return 0;
    }
    p_glUseProgram(prog);
    u_tex = p_glGetUniformLocation(prog, "u_tex");
    u_toon = p_glGetUniformLocation(prog, "u_toon");
    u_resaltado = p_glGetUniformLocation(prog, "u_resaltado");
    u_prueba = p_glGetUniformLocation(prog, "u_prueba");
    u_ref = p_glGetUniformLocation(prog, "u_ref");
    u_wbuf = p_glGetUniformLocation(prog, "u_wbuf");
    u_premult = p_glGetUniformLocation(prog, "u_premult");
    p_glUniform1i(u_tex, 0);
    p_glGenFramebuffers(1, &fbo);
    p_glGenRenderbuffers(1, &rb_prof);
    glGenTextures(1, &tex_color);
    return 1;
}

static int pon_tamaño(int an, int al) {
    if (an == fbo_an && al == fbo_al) return 1;
    glBindTexture(GL_TEXTURE_2D, tex_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, an, al, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glBindRenderbuffer(GL_RENDERBUFFER, rb_prof);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, an, al);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_color, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb_prof);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb_prof);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[gpu] el framebuffer de %dx%d no vale\n", an, al);
        return 0;
    }
    fbo_an = an;
    fbo_al = al;
    return 1;
}

// ---- texturas: decodificadas a RGBA8 y guardadas mientras no cambien en la VRAM ---------------------------------------
// Tabla hash por (TEXIMAGE_PARAM, PLTT_BASE): cada una con su textura de OpenGL y la huella de cuando se subió.
// La huella se mira una vez por imagen; si ha cambiado (paletas animadas, otra pista), se vuelve a subir.
typedef struct { uint32_t ti, pltt; uint64_t huella, visto; GLuint tex; int usada; } Tx;
#define NTX 4096
static Tx txs[NTX];
static int ntx;
static uint64_t turno;
static uint8_t rgba[1024 * 1024 * 4];

static Tx *busca(uint32_t ti, uint32_t pltt) {
    uint32_t h = (ti * 2654435761u ^ pltt * 40503u) & (NTX - 1);
    while (txs[h].usada && !(txs[h].ti == ti && txs[h].pltt == pltt)) h = (h + 1) & (NTX - 1);
    return &txs[h];
}

static GLuint textura(uint32_t ti, uint32_t pltt) {
    ti &= 0x3FF0FFFF;  // sin repetir/espejo (eso va en cada dibujo)
    if (((ti >> 26) & 7) == 7) pltt = 0;  // color directo: sin paleta
    Tx *t = busca(ti, pltt);
    if (t->usada && t->visto == turno) return t->tex;  // ya mirada en esta imagen
    if (!t->usada) {
        if (ntx >= NTX * 3 / 4) {  // llena (no debería pasar): se empieza de cero
            for (int i = 0; i < NTX; i++) if (txs[i].tex) glDeleteTextures(1, &txs[i].tex);
            memset(txs, 0, sizeof txs);
            ntx = 0;
            t = busca(ti, pltt);
        }
        t->usada = 1;
        t->ti = ti;
        t->pltt = pltt;
        ntx++;
    }
    uint64_t h = gx_tex_huella(ti, pltt);
    if (!t->tex || t->huella != h) {
        int w = 8 << ((ti >> 20) & 7), al = 8 << ((ti >> 23) & 7);
        gx_tex_decodifica(ti, pltt, rgba);
        if (!t->tex) glGenTextures(1, &t->tex);
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, al, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        t->huella = h;
    }
    t->visto = turno;
    return t->tex;
}

// ---- pintar --------------------------------------------------------------------------------------------------------
typedef struct { float pos[4], col[3], st[2], par[4], tam[2]; } GV;
#define MAX_GV (4096 * 8 * 3)
static GV gv[MAX_GV];
static uint8_t lectura[1024 * 768 * 4];

int gl3d_pinta(const void *lista, int n, int tam_pp, const uint8_t *io, int wbuf, uint16_t *color, uint8_t *alfa,
               int an, int al) {
    if (estado < 0) return 0;
    if (tam_pp != (int)sizeof(PP)) { estado = -1; fprintf(stderr, "[gpu] PP no cuadra\n"); return 0; }
    if (estado == 0) {
        estado = empieza() ? 1 : -1;
        if (estado < 0) { fprintf(stderr, "[gpu] no se puede: el 3D va por la CPU\n"); return 0; }
    }
    MSG m;  // la ventana oculta es de este hilo: que no se le acumulen mensajes
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    if (!pon_tamaño(an, al)) { estado = -1; return 0; }
    turno++;
    const PP *pp = (const PP *)lista;

    uint16_t disp3d = (uint16_t)(io[0x60] | io[0x61] << 8);
    int mezcla = (disp3d >> 3) & 1;
    // la tabla toon (0x04000380) y lo demás del motor
    float toon[32 * 3];
    for (int i = 0; i < 32; i++) {
        uint16_t c = (uint16_t)(io[0x380 + i * 2] | io[0x381 + i * 2] << 8);
        toon[i * 3] = (float)(c & 31); toon[i * 3 + 1] = (float)((c >> 5) & 31); toon[i * 3 + 2] = (float)((c >> 10) & 31);
    }
    p_glUseProgram(prog);
    p_glUniform3fv(u_toon, 32, toon);
    p_glUniform1i(u_resaltado, (disp3d >> 1) & 1);
    p_glUniform1i(u_prueba, (disp3d >> 2) & 1);
    p_glUniform1f(u_ref, (float)(io[0x340] & 31));
    p_glUniform1i(u_wbuf, wbuf);
    p_glUniform1i(u_premult, !mezcla);

    // fondo: CLEAR_COLOR (con el color ya multiplicado por su alfa) y CLEAR_DEPTH
    uint32_t clear = (uint32_t)(io[0x350] | io[0x351] << 8 | io[0x352] << 16 | io[0x353] << 24);
    float ca = (float)((clear >> 16) & 31) / 31.0f;
    float cr = (float)(clear & 31) / 31.0f, cg = (float)((clear >> 5) & 31) / 31.0f, cb = (float)((clear >> 10) & 31) / 31.0f;
    uint16_t clear_prof = (uint16_t)((io[0x354] | io[0x355] << 8) & 0x7FFF);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, an, al);
    glClearColor(cr * ca, cg * ca, cb * ca, ca);
    glClearDepth(((double)clear_prof * 0x200 + 0x1FF) / 16777215.0);
    glClearStencil(0);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    if (mezcla) {
        glEnable(GL_BLEND);
        p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
        p_glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
    } else glDisable(GL_BLEND);
    p_glActiveTexture(GL_TEXTURE0);
    for (int a = A_POS; a <= A_TAM; a++) p_glEnableVertexAttribArray((GLuint)a);
    p_glVertexAttribPointer(A_POS, 4, GL_FLOAT, GL_FALSE, sizeof(GV), gv[0].pos);
    p_glVertexAttribPointer(A_COL, 3, GL_FLOAT, GL_FALSE, sizeof(GV), gv[0].col);
    p_glVertexAttribPointer(A_ST, 2, GL_FLOAT, GL_FALSE, sizeof(GV), gv[0].st);
    p_glVertexAttribPointer(A_PAR, 4, GL_FLOAT, GL_FALSE, sizeof(GV), gv[0].par);
    p_glVertexAttribPointer(A_TAM, 2, GL_FLOAT, GL_FALSE, sizeof(GV), gv[0].tam);

    // Por tandas: polígonos seguidos con la misma textura y el mismo estado van en un solo dibujo
    int ngv = 0;
    GLuint tex_tanda = 0;
    int clave_tanda = -1;
    for (int pasada = 0; pasada < 2; pasada++)  // primero los opacos, luego los translúcidos (como la CPU)
        for (int k = 0; k <= n; k++) {
            const PP *p = k < n ? &pp[k] : NULL;
            if (p && p->translucido != pasada) continue;
            GLuint tx = 0;
            int clave = -2;
            if (p) {
                if (p->texturas) tx = textura(p->teximage, p->pltt);
                // estado: escribe profundidad, plantilla (ID), repetir/espejo de la textura
                int escribe = !pasada || p->act_prof_trasl;
                clave = escribe | (pasada ? (p->id + 1) << 1 : 0) | (int)((p->teximage >> 16) & 15) << 8 |
                        p->prof_igual << 12;
            }
            if (ngv && (!p || tx != tex_tanda || clave != clave_tanda)) {  // se dibuja lo que había
                glBindTexture(GL_TEXTURE_2D, tex_tanda);
                if (tex_tanda) {
                    int r = (clave_tanda >> 8) & 15;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, r & 1 ? (r & 4 ? GL_MIRRORED_REPEAT : GL_REPEAT) : GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, r & 2 ? (r & 8 ? GL_MIRRORED_REPEAT : GL_REPEAT) : GL_CLAMP_TO_EDGE);
                }
                glDepthMask(clave_tanda & 1 ? GL_TRUE : GL_FALSE);
                int id = (clave_tanda >> 1) & 127;
                if (id) glStencilFunc(GL_NOTEQUAL, id, 0xFF);
                else glStencilFunc(GL_ALWAYS, 0, 0xFF);
                glDepthFunc(clave_tanda >> 12 & 1 ? GL_LEQUAL : GL_LESS);  // "profundidad igual": ya va algo por delante
                glDrawArrays(GL_TRIANGLES, 0, ngv);
                ngv = 0;
            }
            if (!p) break;
            tex_tanda = tx;
            clave_tanda = clave;
            float tw = (float)(8 << ((p->teximage >> 20) & 7)), th = (float)(8 << ((p->teximage >> 23) & 7));
            for (int j = 1; j + 1 < p->n && ngv + 3 <= MAX_GV; j++) {
                const VP *tri[3] = {&p->v[0], &p->v[j], &p->v[j + 1]};
                for (int q = 0; q < 3; q++) {
                    const VP *v = tri[q];
                    GV *o = &gv[ngv++];
                    float w = 1.0f / v->iw;
                    o->pos[0] = (v->x / (float)an * 2 - 1) * w;
                    o->pos[1] = (1 - v->y / (float)al * 2) * w;
                    o->pos[2] = wbuf ? 0 : (v->z * 2 - 1) * w;
                    o->pos[3] = w;
                    o->col[0] = v->r * w; o->col[1] = v->g * w; o->col[2] = v->b * w;
                    o->st[0] = v->s * w; o->st[1] = v->t * w;
                    o->par[0] = p->modo; o->par[1] = p->alfa; o->par[2] = p->texturas; o->par[3] = p->prof_igual;
                    o->tam[0] = tw; o->tam[1] = th;
                }
            }
        }

    // de vuelta a la CPU (la GPU guarda de abajo arriba; el color, multiplicado por el alfa)
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, an, al, GL_RGBA, GL_UNSIGNED_BYTE, lectura);
    uint16_t cc = (uint16_t)(clear & 0x7FFF);
    static uint8_t a5[256];  // de 8 bits a 5
    if (!a5[255]) for (int i = 0; i < 256; i++) a5[i] = (uint8_t)((i * 31 + 127) / 255);
    for (int y = 0; y < al; y++) {
        const uint8_t *f = lectura + (size_t)(al - 1 - y) * an * 4;
        uint16_t *dc16 = color + (size_t)y * an;
        uint8_t *da = alfa + (size_t)y * an;
        for (int x = 0; x < an; x++, f += 4) {
            int A = f[3];
            if (!A) { dc16[x] = cc; da[x] = 0; continue; }
            if (A == 255) {  // lo normal: sin dividir (a escala 4 son 786.000 puntos por imagen)
                dc16[x] = (uint16_t)(a5[f[0]] | a5[f[1]] << 5 | a5[f[2]] << 10);
                da[x] = 31;
                continue;
            }
            int r, g, b;
            {
                r = f[0] * 255 / A; g = f[1] * 255 / A; b = f[2] * 255 / A;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
            }
            dc16[x] = (uint16_t)(a5[r] | a5[g] << 5 | a5[b] << 10);
            da[x] = a5[A];
        }
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}
#endif
