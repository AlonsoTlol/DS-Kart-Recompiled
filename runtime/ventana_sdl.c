// Ventana para Linux (y cualquier sistema con SDL2): lo mismo que ventana.c (la de Windows) con SDL2 y OpenGL.
// Las dos pantallas de la DS una encima de otra (x2), el teclado como botones, el ratón como lápiz en la de
// abajo y los mandos con la API GameController de SDL (Xbox, PlayStation y los que SDL conozca).
//
// Controles:  flechas = cruceta   X = A   Z = B   S = X   A = Y
//             Q = L   W = R   Enter = Start   Retroceso = Select   F11 = pantalla completa
//             F10 = guardar los últimos 30 segundos en vídeo (repeticiones/)
// Mando: botón de abajo = A, derecha = B, arriba = X, izquierda = Y (por posición, como la DS), LB/LT = L,
//        RB/RT = R, Start, Back/Create = Select, cruceta o stick izquierdo
//
// Como en Windows, las imágenes se pintan en un hilo aparte (hilo_pinta) al ritmo del monitor, desde una cola de
// NM imágenes con la hora a la que toca enseñar cada una (así caben las intermedias de MKDS_FPS).
// El cartel de la repetición instantánea va en el título de la ventana (aquí no hay GDI para dibujar texto).
#include "compat.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video.h"
#include "gpu3d.h"

#define ESCALA 2

const char *repe_aviso(void);
void repe_frame(const uint32_t *arriba, const uint32_t *abajo);
void repe_guarda(void);
int video_escala(void);
const uint32_t *video_hd(int abajo);

volatile uint16_t g_keyinput = 0x3FF;
volatile uint16_t g_keyxy = 0x3;
volatile int g_lapiz, g_lapiz_x, g_lapiz_y;
// Botones que pulsa el guion de pruebas (MKDS_GUION, en ds.c): se suman al teclado y al mando
volatile uint16_t g_guion_in = 0x3FF, g_guion_xy = 3;

typedef struct { uint32_t baja[256 * 384]; uint32_t hd[1024 * 1536]; } Marco;
#define NM 8
static Marco marcos[NM];
static int64_t marco_ts[NM];   // cuándo enseñarla; 0 = aún no está lista
static int i_escribe, i_reservada;
static volatile int i_viendo = -1;
static volatile LONG enseñadas;
static const Marco *ultimo = &marcos[0];
static CRITICAL_SECTION cs_marcos, cs_destino;

static SDL_Window *ventana;
static SDL_GLContext contexto;
static int oculto, pantalla_completa;
static int dst_x, dst_y, dst_w = 256 * ESCALA, dst_h = 384 * ESCALA;  // dónde va la imagen (en puntos de la ventana)
static uint16_t teclado_in = 0x3FF, teclado_xy = 3;
static char titulo[160];
static volatile int titulo_nuevo;

static int64_t reloj(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
int64_t ventana_reloj(void) { return reloj(); }

static void calcula_destino(void) {
    int w, h;
    SDL_GetWindowSize(ventana, &w, &h);
    if (w <= 0 || h <= 0) return;
    int iw = pantalla_completa ? 512 : 256, ih = pantalla_completa ? 192 : 384;
    EnterCriticalSection(&cs_destino);
    if (w * ih > h * iw) { dst_h = h; dst_w = h * iw / ih; }   // sobra ancho: bandas a los lados
    else { dst_w = w; dst_h = w * ih / iw; }                   // sobra alto: bandas arriba y abajo
    dst_x = (w - dst_w) / 2;
    dst_y = (h - dst_h) / 2;
    LeaveCriticalSection(&cs_destino);
}

// la imagen más nueva cuya hora ya ha llegado (y se la queda el pintor); -1 si no hay ninguna
static int elige_marco(void) {
    int64_t ya = reloj();
    EnterCriticalSection(&cs_marcos);
    int mejor = -1;
    for (int i = 0; i < NM; i++)
        if (marco_ts[i] && marco_ts[i] <= ya && (mejor < 0 || marco_ts[i] > marco_ts[mejor])) mejor = i;
    if (mejor >= 0) i_viendo = mejor;
    LeaveCriticalSection(&cs_marcos);
    return mejor;
}

static GLuint tex_juego;
static int tex_w, tex_h;

static void gl_rect(float x, float y, float w, float h, float u0, float v0, float u1, float v1) {
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
    glTexCoord2f(u0, v1); glVertex2f(x, y + h);
    glEnd();
}

static void gl_pinta(int i, int subir) {
    int vw, vh, ww, wh;
    SDL_GL_GetDrawableSize(ventana, &vw, &vh);  // en píxeles (con pantallas HiDPI no es lo mismo que los puntos)
    SDL_GetWindowSize(ventana, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    int e = video_escala(), iw = 256 * e, ih = 384 * e;
    const uint32_t *px = e > 1 ? marcos[i].hd : marcos[i].baja;
    glBindTexture(GL_TEXTURE_2D, tex_juego);
    if (tex_w != iw || tex_h != ih) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLint filtro = e > 1 ? GL_LINEAR : GL_NEAREST;  // con reescalado se reduce: suave; sin él, píxeles nítidos
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtro);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtro);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_BGRA, GL_UNSIGNED_BYTE, px);
        tex_w = iw; tex_h = ih;
    } else if (subir) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, iw, ih, GL_BGRA, GL_UNSIGNED_BYTE, px);
    }
    glViewport(0, 0, vw, vh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, ww, wh, 0, -1, 1);  // en puntos de la ventana, como el ratón
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    EnterCriticalSection(&cs_destino);
    float x = (float)dst_x, y = (float)dst_y, w = (float)dst_w, h = (float)dst_h;
    int lado = pantalla_completa;
    LeaveCriticalSection(&cs_destino);
    if (lado) {  // pantalla de arriba a la izquierda, táctil a la derecha
        gl_rect(x, y, w / 2, h, 0, 0, 1, 0.5f);
        gl_rect(x + w / 2, y, w / 2, h, 0, 0.5f, 1, 1);
    } else {
        gl_rect(x, y, w, h, 0, 0, 1, 1);
    }
    SDL_GL_SwapWindow(ventana);  // espera al refresco del monitor
}

static DWORD WINAPI hilo_pinta(LPVOID p) {
    (void)p;
    SDL_GL_MakeCurrent(ventana, contexto);
    if (SDL_GL_SetSwapInterval(1)) fprintf(stderr, "[ventana] sin sincronía con el monitor: %s\n", SDL_GetError());
    glGenTextures(1, &tex_juego);
    int subido = -1;
    int64_t ts_subido = 0;
    for (;;) {
        int i = elige_marco();
        if (i < 0) { Sleep(1); continue; }
        int nuevo = i != subido || marco_ts[i] != ts_subido;
        gl_pinta(i, nuevo);
        if (nuevo) InterlockedIncrement(&enseñadas);
        subido = i;
        ts_subido = marco_ts[i];
    }
    return 0;
}

static void cambia_pantalla_completa(void) {
    pantalla_completa = !pantalla_completa;
    SDL_SetWindowFullscreen(ventana, pantalla_completa ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    calcula_destino();
}

void ventana_inicia(void) {
    InitializeCriticalSection(&cs_marcos);
    InitializeCriticalSection(&cs_destino);
    if (getenv("MKDS_OCULTO")) { oculto = 1; return; }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr, "[ventana] SDL: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    ventana = SDL_CreateWindow("DS Kart Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 256 * ESCALA,
                               384 * ESCALA, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!ventana) { fprintf(stderr, "[ventana] no se pudo abrir: %s\n", SDL_GetError()); exit(1); }
    contexto = SDL_GL_CreateContext(ventana);
    if (!contexto) { fprintf(stderr, "[ventana] sin OpenGL: %s\n", SDL_GetError()); exit(1); }
    SDL_GL_MakeCurrent(ventana, NULL);  // lo usa el hilo del pintor
    calcula_destino();
    if (getenv("MKDS_PANTALLA_COMPLETA") && atoi(getenv("MKDS_PANTALLA_COMPLETA"))) cambia_pantalla_completa();
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
    CreateThread(NULL, 0, hilo_pinta, NULL, 0, NULL);
}

// MKDS_GRABA=portada.mp4: graba el vídeo de la portada del launcher (ver ventana.c)
static void graba_portada(const uint32_t *hd, const uint32_t *baja, int e) {
    static int listo, carreras_max, carrera, fotogramas, fuera = 1000;
    static FILE *ff;
    if (!listo) {
        listo = 1;
        const char *ruta = getenv("MKDS_GRABA");
        if (!ruta) return;
        carreras_max = getenv("MKDS_GRABA_CARRERAS") ? atoi(getenv("MKDS_GRABA_CARRERAS")) : 7;
        char orden[1024];
        snprintf(orden, sizeof orden,
                 "ffmpeg -v error -y -f rawvideo -pixel_format bgra -video_size %dx%d -framerate 60 -i - "
                 "-vf \"scale=1152:864:flags=lanczos,format=yuv420p\" -c:v libx264 -preset veryfast -crf 20 "
                 "-movflags +faststart -an \"%s\"", 256 * e, 192 * e, ruta);
        ff = popen(orden, "w");
    }
    if (!ff) return;
    if (gx_polis() <= 50) { fuera++; return; }
    if (fuera >= 120) { carrera++; fotogramas = 0; }
    fuera = 0;
    fotogramas++;
    if (fotogramas > 300 && fotogramas <= 300 + 900)
        fwrite(e > 1 ? hd : baja, 4, (size_t)256 * e * 192 * e, ff);
    if (carrera >= carreras_max && fotogramas == 300 + 900) {
        pclose(ff);
        fprintf(stderr, "[mkds] portada grabada: %d carreras\n", carrera);
        ExitProcess(0);
    }
}

void ventana_guarda_bmp_de(const char *ruta, const uint32_t *px, int e);
static void guarda_intermedia(void) {
    static int listo, n, desde, hasta = 1 << 30;  // MKDS_CAPTURA_TODO_RANGO="desde,hasta" (nº de imagen)
    static const char *dir;
    if (!listo) {
        listo = 1;
        dir = getenv("MKDS_CAPTURA_TODO");
        if (getenv("MKDS_CAPTURA_TODO_RANGO")) sscanf(getenv("MKDS_CAPTURA_TODO_RANGO"), "%d,%d", &desde, &hasta);
    }
    if (!dir) return;
    if (n < desde || n > hasta) { n++; return; }
    char ruta[512];
    snprintf(ruta, sizeof ruta, "%s/t_%06d.bmp", dir, n++);
    int e = video_escala();
    ventana_guarda_bmp_de(ruta, e > 1 ? marcos[i_reservada].hd : marcos[i_reservada].baja, e);
}

// una imagen libre de la cola (nunca la que está enseñando el pintor)
static Marco *reserva(void) {
    EnterCriticalSection(&cs_marcos);
    int i = i_escribe;
    if (i == i_viendo) i = (i + 1) % NM;
    i_escribe = (i + 1) % NM;
    marco_ts[i] = 0;
    i_reservada = i;
    LeaveCriticalSection(&cs_marcos);
    return &marcos[i];
}

static void publica(int64_t ts) {
    EnterCriticalSection(&cs_marcos);
    marco_ts[i_reservada] = ts ? ts : 1;
    LeaveCriticalSection(&cs_marcos);
}

uint32_t *ventana_reserva_intermedia(void) {
    Marco *m = reserva();
    return video_escala() > 1 ? m->hd : m->baja;
}
void ventana_publica_intermedia(int64_t ts) {
    if (oculto) { guarda_intermedia(); return; }
    publica(ts);
}

void ventana_muestra_en(const uint32_t *arriba, const uint32_t *abajo, int64_t ts);
void ventana_muestra(const uint32_t *arriba, const uint32_t *abajo) { ventana_muestra_en(arriba, abajo, 0); }

void ventana_muestra_en(const uint32_t *arriba, const uint32_t *abajo, int64_t ts) {
    Marco *m = reserva();
    memcpy(m->baja, arriba, 256 * 192 * 4);
    memcpy(m->baja + 256 * 192, abajo, 256 * 192 * 4);
    int e = video_escala();
    if (e > 1) {
        size_t n = (size_t)256 * e * 192 * e;
        memcpy(m->hd, video_hd(0), n * 4);
        memcpy(m->hd + n, video_hd(1), n * 4);
    }
    ultimo = m;
    publica(ts ? ts : reloj());
    graba_portada(m->hd, m->baja, e);
    if (oculto) { guarda_intermedia(); return; }
    repe_frame(arriba, abajo);
}

static void tecla(SDL_Keycode k, int pulsada) {
    int bit = -1, xy = -1;
    switch (k) {
    case SDLK_x: bit = 0; break;          // A
    case SDLK_z: bit = 1; break;          // B
    case SDLK_BACKSPACE: bit = 2; break;  // Select
    case SDLK_RETURN: bit = 3; break;     // Start
    case SDLK_RIGHT: bit = 4; break;
    case SDLK_LEFT: bit = 5; break;
    case SDLK_UP: bit = 6; break;
    case SDLK_DOWN: bit = 7; break;
    case SDLK_w: bit = 8; break;          // R
    case SDLK_q: bit = 9; break;          // L
    case SDLK_s: xy = 0; break;           // X
    case SDLK_a: xy = 1; break;           // Y
    }
    if (bit >= 0) {
        if (pulsada) teclado_in &= (uint16_t)~(1u << bit);
        else teclado_in |= (uint16_t)(1u << bit);
    }
    if (xy >= 0) {
        if (pulsada) teclado_xy &= (uint16_t)~(1u << xy);
        else teclado_xy |= (uint16_t)(1u << xy);
    }
}

static void raton(int mx, int my, int pulsado) {
    EnterCriticalSection(&cs_destino);
    int x0 = dst_x, y0 = dst_y, w = dst_w, h = dst_h;
    LeaveCriticalSection(&cs_destino);
    if (w <= 0 || h <= 0) return;
    int x, y;
    if (pantalla_completa) {  // la táctil es la mitad derecha
        x = (mx - x0) * 512 / w - 256;
        y = (my - y0) * 192 / h;
    } else {
        x = (mx - x0) * 256 / w;
        y = (my - y0) * 384 / h - 192;
    }
    if (pulsado && x >= 0 && x < 256 && y >= 0 && y < 192) {
        g_lapiz_x = x;
        g_lapiz_y = y;
        g_lapiz = 1;
    } else {
        g_lapiz = 0;
    }
}

// Mandos: se suman a lo que se pulse en el teclado
static void lee_mandos(uint16_t *in, uint16_t *xy) {
    *in = 0x3FF;
    *xy = 3;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_GameController *m = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (!m) continue;
        const int zona = 12000;
        int lx = SDL_GameControllerGetAxis(m, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(m, SDL_CONTROLLER_AXIS_LEFTY);
#define B(b) SDL_GameControllerGetButton(m, SDL_CONTROLLER_BUTTON_##b)
        struct { int pulsado, bit; } t[] = {
            {B(A), 0}, {B(B), 1}, {B(BACK), 2}, {B(START), 3},
            {B(DPAD_RIGHT) || lx > zona, 4}, {B(DPAD_LEFT) || lx < -zona, 5},
            {B(DPAD_UP) || ly < -zona, 6}, {B(DPAD_DOWN) || ly > zona, 7},
            {B(RIGHTSHOULDER) || SDL_GameControllerGetAxis(m, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000, 8},
            {B(LEFTSHOULDER) || SDL_GameControllerGetAxis(m, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000, 9},
        };
        for (int k = 0; k < 10; k++)
            if (t[k].pulsado) *in &= (uint16_t)~(1u << t[k].bit);
        if (B(Y)) *xy &= ~1;  // el de arriba: X de la DS
        if (B(X)) *xy &= ~2;  // el de la izquierda: Y de la DS
#undef B
    }
}

void ventana_mensajes(void) {
    uint16_t mando_in = 0x3FF, mando_xy = 3;
    if (!oculto) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: ExitProcess(0);
            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                if (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_ESCAPE && pantalla_completa))
                    cambia_pantalla_completa();
                else if (e.key.keysym.sym == SDLK_F10) repe_guarda();
                else tecla(e.key.keysym.sym, 1);
                break;
            case SDL_KEYUP: tecla(e.key.keysym.sym, 0); break;
            case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_LEFT) raton(e.button.x, e.button.y, 1); break;
            case SDL_MOUSEMOTION: if (e.motion.state & SDL_BUTTON_LMASK) raton(e.motion.x, e.motion.y, 1); break;
            case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) raton(e.button.x, e.button.y, 0); break;
            case SDL_WINDOWEVENT: if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) calcula_destino(); break;
            case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
            }
        }
        if (titulo_nuevo) { titulo_nuevo = 0; SDL_SetWindowTitle(ventana, titulo); }
        lee_mandos(&mando_in, &mando_xy);
    }
    g_keyinput = teclado_in & mando_in & g_guion_in;
    g_keyxy = (uint16_t)(teclado_xy & mando_xy & g_guion_xy);
}

static void escribe_bmp(const char *ruta, const uint32_t *px, int e) {
    FILE *f = fopen(ruta, "wb");
    if (!f) return;
    uint32_t tam = 54 + 256 * 384 * 4 * (uint32_t)(e * e);
    uint8_t cab[54] = {'B', 'M'};
    memcpy(cab + 2, &tam, 4);
    uint32_t off = 54, cab_tam = 40, ancho = 256 * (uint32_t)e, planos_bits = 1 | (32 << 16);
    int32_t alto = -384 * e;
    memcpy(cab + 10, &off, 4);
    memcpy(cab + 14, &cab_tam, 4);
    memcpy(cab + 18, &ancho, 4);
    memcpy(cab + 22, &alto, 4);
    memcpy(cab + 26, &planos_bits, 4);
    fwrite(cab, 1, 54, f);
    fwrite(px, 4, (size_t)256 * 384 * e * e, f);
    fclose(f);
}

// Para guardar una captura (MKDS_CAPTURA=archivo.bmp)
void ventana_guarda_bmp(const char *ruta) {
    int e = video_escala();  // con reescalado, a tamaño grande
    escribe_bmp(ruta, e > 1 ? ultimo->hd : ultimo->baja, e);
}

void ventana_guarda_bmp_de(const char *ruta, const uint32_t *px, int e) { escribe_bmp(ruta, px, e); }

// Contador de FPS en el título de la ventana (lo pone ventana_mensajes: SDL quiere el hilo de la ventana)
void ventana_titulo_fps(int fps, int fps3d) {
    if (oculto || !ventana) return;
    const char *av = repe_aviso();
    if (av) snprintf(titulo, sizeof titulo, "DS Kart Recompiled  -  %s", av);
    else snprintf(titulo, sizeof titulo, "DS Kart Recompiled  -  %d FPS  (3D: %d)  pantalla: %d", fps, fps3d,
                  (int)InterlockedExchange(&enseñadas, 0));
    titulo_nuevo = 1;
}
