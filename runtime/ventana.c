// Ventana de Windows con las dos pantallas de la DS (una encima de otra, x2)
// y la entrada: teclado como botones, ratón como lápiz en la pantalla de abajo.
//
// Controles:  flechas = cruceta   X = A   Z = B   S = X   A = Y
//             Q = L   W = R   Enter = Start   Retroceso = Select   F11 = pantalla completa
//             F10 = guardar los últimos 30 segundos en vídeo (repeticiones/)   F1 = menú de ajustes
// Mando (XInput): A = A, B = B, Y (arriba) = X, X (izquierda) = Y, LB/LT = L, RB/RT = R, Start,
//                 Back = Select, cruceta o stick izquierdo; L3 + R3 = menú de ajustes
// Mando de PS5/PS4 (leído directamente por USB o Bluetooth): X = A, círculo = B,
//                 cuadrado = Y, triángulo = X, L1/L2 = L, R1/R2 = R, Options = Start, Create = Select
#include <windows.h>
#include <xinput.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <GL/gl.h>
#include "video.h"
#include "menu.h"
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

const char *repe_aviso(void);  // repeticion.c

#define ESCALA 2

volatile uint16_t g_keyinput = 0x3FF;
volatile uint16_t g_keyxy = 0x3;
volatile int g_lapiz, g_lapiz_x, g_lapiz_y;

static HWND ventana;
// Las imágenes se pintan en un hilo aparte (hilo_pinta), con OpenGL y al ritmo del monitor. Van por una cola de
// NM imágenes, cada una con la hora a la que toca enseñarla (QueryPerformanceCounter): así caben las imágenes
// intermedias (MKDS_FPS: 120, 180 o 240) repartidas entre dos fotogramas del juego. El pintor enseña en cada
// refresco la más nueva cuya hora ya ha llegado; si va tarde, se salta las que sobran. El juego nunca espera.
typedef struct { uint32_t baja[256 * 384]; uint32_t hd[1024 * 1536]; } Marco;
#define NM 8
static Marco marcos[NM];
static int64_t marco_ts[NM];   // cuándo enseñarla; 0 = aún no está lista
static int i_escribe;          // la siguiente que se rellena
static volatile int i_viendo = -1;  // la que tiene el pintor (no se toca)
static volatile LONG enseñadas;     // imágenes distintas enseñadas (para el título: las que se ven por segundo)
static int i_reservada;
static const Marco *ultimo = &marcos[0];  // la última imagen del juego completa (para las capturas en BMP)
static CRITICAL_SECTION cs_marcos, cs_gdi;
static HANDLE ev_pintar;
static uint32_t *imagen = marcos[0].baja;  // la que pinta compone()
static BITMAPINFO bmi;
static uint16_t teclado_in = 0x3FF, teclado_xy = 3;  // estado del teclado (0 = pulsado)
static DWORD WINAPI hilo_mando_ps(LPVOID p);
// Dónde se dibuja la imagen dentro de la ventana (cambia en pantalla completa)
static int dst_x, dst_y, dst_w = 256 * ESCALA, dst_h = 384 * ESCALA;
static int pantalla_completa;
// En pantalla completa: las dos pantallas lado a lado (principal a la izquierda, táctil a la derecha)
static uint32_t imagen_lado[512 * 192];
static BITMAPINFO bmi_lado;
static RECT rect_normal;
// Reescalado (MKDS_ESCALA > 1): la misma imagen a tamaño grande, con el 3D pintado a más resolución
int video_escala(void);
const uint32_t *video_hd(int abajo);
static uint32_t *imagen_hd = marcos[0].hd, imagen_lado_hd[2048 * 768];
static BITMAPINFO bmi_hd, bmi_lado_hd;
static LONG estilo_normal;

static void calcula_destino(void) {
    RECT r;
    GetClientRect(ventana, &r);
    int w = r.right, h = r.bottom;
    if (w <= 0 || h <= 0) return;
    int iw = pantalla_completa ? 512 : 256, ih = pantalla_completa ? 192 : 384;  // tamaño de la imagen
    if (w * ih > h * iw) { dst_h = h; dst_w = h * iw / ih; }   // sobra ancho: bandas a los lados
    else { dst_w = w; dst_h = w * ih / iw; }                   // sobra alto: bandas arriba y abajo
    dst_x = (w - dst_w) / 2;
    dst_y = (h - dst_h) / 2;
}

static void compone(HDC dc) {  // dibuja todo (imagen, bandas, cartel) en dc
    RECT r;
    GetClientRect(ventana, &r);
    HBRUSH negro = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT b1 = {0, 0, r.right, dst_y}, b2 = {0, dst_y + dst_h, r.right, r.bottom};
    RECT b3 = {0, dst_y, dst_x, dst_y + dst_h}, b4 = {dst_x + dst_w, dst_y, r.right, dst_y + dst_h};
    FillRect(dc, &b1, negro); FillRect(dc, &b2, negro); FillRect(dc, &b3, negro); FillRect(dc, &b4, negro);
    SetStretchBltMode(dc, COLORONCOLOR);
    int e = video_escala();
    if (e > 1 && pantalla_completa) {
        int an = 256 * e;
        for (int y = 0; y < 192 * e; y++) {
            memcpy(imagen_lado_hd + (size_t)y * an * 2, imagen_hd + (size_t)y * an, (size_t)an * 4);
            memcpy(imagen_lado_hd + (size_t)y * an * 2 + an, imagen_hd + (size_t)(192 * e + y) * an, (size_t)an * 4);
        }
        StretchDIBits(dc, dst_x, dst_y, dst_w, dst_h, 0, 0, 512 * e, 192 * e, imagen_lado_hd, &bmi_lado_hd,
                      DIB_RGB_COLORS, SRCCOPY);
    } else if (e > 1) {
        StretchDIBits(dc, dst_x, dst_y, dst_w, dst_h, 0, 0, 256 * e, 384 * e, imagen_hd, &bmi_hd, DIB_RGB_COLORS, SRCCOPY);
    } else if (pantalla_completa) {
        for (int y = 0; y < 192; y++) {  // izquierda: pantalla de arriba; derecha: la de abajo
            memcpy(imagen_lado + y * 512, imagen + y * 256, 256 * 4);
            memcpy(imagen_lado + y * 512 + 256, imagen + (192 + y) * 256, 256 * 4);
        }
        StretchDIBits(dc, dst_x, dst_y, dst_w, dst_h, 0, 0, 512, 192, imagen_lado, &bmi_lado, DIB_RGB_COLORS, SRCCOPY);
    } else {
        StretchDIBits(dc, dst_x, dst_y, dst_w, dst_h, 0, 0, 256, 384, imagen, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }
    // cartel de la repetición instantánea, arriba en el centro de la imagen
    const char *av = repe_aviso();
    if (av) {
        wchar_t txt[128];
        MultiByteToWideChar(CP_UTF8, 0, av, -1, txt, 128);
        int alto = dst_h / (pantalla_completa ? 9 : 14);
        if (alto < 24) alto = 24;
        HFONT fuente = CreateFontW(alto * 6 / 10, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                   CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ vieja = SelectObject(dc, fuente);
        SIZE tam;
        GetTextExtentPoint32W(dc, txt, (int)wcslen(txt), &tam);
        int ancho = tam.cx + alto;
        RECT caja = {dst_x + (dst_w - ancho) / 2, dst_y + alto / 3, dst_x + (dst_w + ancho) / 2, dst_y + alto / 3 + alto};
        HBRUSH fondo = CreateSolidBrush(RGB(20, 20, 30));
        HPEN borde = CreatePen(PS_SOLID, 3, RGB(255, 200, 40));
        HGDIOBJ vb = SelectObject(dc, fondo), vp = SelectObject(dc, borde);
        RoundRect(dc, caja.left, caja.top, caja.right, caja.bottom, alto / 2, alto / 2);
        SelectObject(dc, vb); SelectObject(dc, vp);
        DeleteObject(fondo); DeleteObject(borde);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, txt, -1, &caja, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, vieja);
        DeleteObject(fuente);
    }
}

// Doble búfer: se compone en una imagen invisible y se manda a la pantalla de una vez
// (si no, entre pintar el juego y pintar el cartel se ve un parpadeo).
static HDC mem_dc;
static HBITMAP mem_bmp;
static int mem_w, mem_h;

static void dibuja_en(HDC dc) {
    RECT r;
    GetClientRect(ventana, &r);
    if (r.right <= 0 || r.bottom <= 0) return;
    if (!mem_dc || mem_w != r.right || mem_h != r.bottom) {
        if (mem_bmp) DeleteObject(mem_bmp);
        if (!mem_dc) mem_dc = CreateCompatibleDC(dc);
        mem_bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        SelectObject(mem_dc, mem_bmp);
        mem_w = r.right;
        mem_h = r.bottom;
    }
    compone(mem_dc);
    BitBlt(dc, 0, 0, mem_w, mem_h, mem_dc, 0, 0, SRCCOPY);
}

static void cambia_pantalla_completa(void) {
    if (!pantalla_completa) {
        GetWindowRect(ventana, &rect_normal);
        estilo_normal = GetWindowLongW(ventana, GWL_STYLE);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(ventana, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(ventana, GWL_STYLE, (estilo_normal & ~(LONG)(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)) | WS_POPUP);
        SetWindowPos(ventana, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        pantalla_completa = 1;
    } else {
        SetWindowLongW(ventana, GWL_STYLE, estilo_normal);
        SetWindowPos(ventana, NULL, rect_normal.left, rect_normal.top, rect_normal.right - rect_normal.left,
                     rect_normal.bottom - rect_normal.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        pantalla_completa = 0;
    }
    EnterCriticalSection(&cs_gdi);
    calcula_destino();
    LeaveCriticalSection(&cs_gdi);
    SetEvent(ev_pintar);
}

void repe_frame(const uint32_t *arriba, const uint32_t *abajo);
void repe_guarda(void);
const char *repe_aviso(void);

static int oculto;
static volatile int pide_pc;  // el menú pide pantalla completa (1) o ventana (2): se hace en ventana_mensajes
int ventana_pantalla_completa(void) { return pantalla_completa; }
void ventana_pide_pantalla_completa(int si) { pide_pc = si ? 1 : 2; }

static int tecla_menu(WPARAM vk) {
    switch (vk) {
    case VK_TAB: return MT_TAB;
    case VK_LEFT: return MT_IZQ;
    case VK_RIGHT: return MT_DER;
    case VK_UP: return MT_ARRIBA;
    case VK_DOWN: return MT_ABAJO;
    case VK_HOME: return MT_INICIO;
    case VK_END: return MT_FIN;
    case VK_BACK: return MT_BORRAR;
    case VK_DELETE: return MT_SUPR;
    case VK_RETURN: return MT_ENTER;
    case VK_ESCAPE: return MT_ESC;
    case VK_CONTROL: return MT_CTRL;
    case VK_SHIFT: return MT_SHIFT;
    }
    return 0;
}  // MKDS_OCULTO=1: sin ventana (pruebas sin molestar), solo capturas

static void tecla(WPARAM vk, int pulsada) {
    int bit = -1, xy = -1;
    switch (vk) {
    case 'X': bit = 0; break;          // A
    case 'Z': bit = 1; break;          // B
    case VK_BACK: bit = 2; break;      // Select
    case VK_RETURN: bit = 3; break;    // Start
    case VK_RIGHT: bit = 4; break;
    case VK_LEFT: bit = 5; break;
    case VK_UP: bit = 6; break;
    case VK_DOWN: bit = 7; break;
    case 'W': bit = 8; break;          // R
    case 'Q': bit = 9; break;          // L
    case 'S': xy = 0; break;           // X
    case 'A': xy = 1; break;           // Y
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

static void raton(LPARAM lp, int pulsado) {
    if (dst_w <= 0 || dst_h <= 0) return;
    int x, y;
    if (pantalla_completa) {  // la táctil es la mitad derecha
        x = ((short)LOWORD(lp) - dst_x) * 512 / dst_w - 256;
        y = ((short)HIWORD(lp) - dst_y) * 192 / dst_h;
    } else {
        x = ((short)LOWORD(lp) - dst_x) * 256 / dst_w;
        y = ((short)HIWORD(lp) - dst_y) * 384 / dst_h - 192;
    }
    if (pulsado && x >= 0 && x < 256 && y >= 0 && y < 192) {
        g_lapiz_x = x;
        g_lapiz_y = y;
        g_lapiz = 1;
    } else {
        g_lapiz = 0;
    }
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: case WM_DESTROY: ExitProcess(0);
    case WM_KEYDOWN:
        if (wp == VK_F1) { menu_alterna(); return 0; }
        if (menu_abierto()) { if (tecla_menu(wp)) menu_tecla(tecla_menu(wp), 1); return 0; }
        if (wp == VK_F11 || (wp == VK_ESCAPE && pantalla_completa)) { cambia_pantalla_completa(); return 0; }
        tecla(wp, 1);
        return 0;
    case WM_CHAR:
        if (menu_abierto() && wp >= 32) menu_caracter((unsigned)wp);
        return 0;
    case WM_MOUSEWHEEL:
        menu_rueda((float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
        return 0;
    case WM_RBUTTONDOWN: menu_boton(1, 1); return 0;
    case WM_RBUTTONUP: menu_boton(1, 0); return 0;
    case WM_SIZE:
        EnterCriticalSection(&cs_gdi);
        calcula_destino();
        LeaveCriticalSection(&cs_gdi);
        SetEvent(ev_pintar);
        return 0;
    case WM_SYSKEYDOWN:  // F10 es tecla de sistema: repetición instantánea
        if (wp == VK_F10) { repe_guarda(); return 0; }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_KEYUP:
        if (tecla_menu(wp)) menu_tecla(tecla_menu(wp), 0);
        tecla(wp, 0);  // soltar también para el juego (pudo pulsarse antes de abrir el menú)
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        if (menu_abierto()) { menu_raton((short)LOWORD(lp), (short)HIWORD(lp)); menu_boton(0, 1); }
        else raton(lp, 1);
        return 0;
    case WM_MOUSEMOVE:
        if (menu_abierto()) menu_raton((short)LOWORD(lp), (short)HIWORD(lp));
        else if (wp & MK_LBUTTON) raton(lp, 1);
        return 0;
    case WM_LBUTTONUP: ReleaseCapture(); menu_boton(0, 0); raton(lp, 0); return 0;
    case WM_PAINT: {  // lo pinta hilo_pinta (aquí, en el hilo del juego, no se espera a nada)
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        SetEvent(ev_pintar);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int64_t reloj(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
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

// ---- OpenGL ----
static GLuint tex_juego, tex_cartel;
static int tex_w, tex_h, cartel_w, cartel_h;
static char cartel_texto[128];

static int gl_inicia(HDC dc) {
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 PFD_TYPE_RGBA, 32};
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return 0;
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) return 0;
    typedef BOOL(WINAPI * Intervalo)(int);
    Intervalo intervalo = (Intervalo)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (!intervalo || !intervalo(1)) return 0;  // sin sincronía con el monitor no merece la pena
    glGenTextures(1, &tex_juego);
    glGenTextures(1, &tex_cartel);
    return 1;
}

// el cartel de la repetición, dibujado con GDI en una textura (fondo magenta = transparente)
static void gl_prepara_cartel(const char *av) {
    if (!strcmp(av, cartel_texto)) return;
    snprintf(cartel_texto, sizeof cartel_texto, "%s", av);
    wchar_t txt[128];
    MultiByteToWideChar(CP_UTF8, 0, av, -1, txt, 128);
    int alto = 64;
    HDC mdc = CreateCompatibleDC(NULL);
    HFONT fuente = CreateFontW(alto * 6 / 10, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HGDIOBJ vf = SelectObject(mdc, fuente);
    SIZE tam;
    GetTextExtentPoint32W(mdc, txt, (int)wcslen(txt), &tam);
    cartel_w = tam.cx + alto;
    cartel_h = alto;
    BITMAPINFO bi = {{sizeof bi.bmiHeader, cartel_w, -cartel_h, 1, 32, BI_RGB}};
    uint32_t *px;
    HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HGDIOBJ vb = SelectObject(mdc, bmp);
    for (int i = 0; i < cartel_w * cartel_h; i++) px[i] = 0x00FF00FF;
    HBRUSH fondo = CreateSolidBrush(RGB(20, 20, 30));
    HPEN borde = CreatePen(PS_SOLID, 3, RGB(255, 200, 40));
    HGDIOBJ vfo = SelectObject(mdc, fondo), vp = SelectObject(mdc, borde);
    RoundRect(mdc, 1, 1, cartel_w - 1, cartel_h - 1, alto / 2, alto / 2);
    SetBkMode(mdc, TRANSPARENT);
    SetTextColor(mdc, RGB(255, 255, 255));
    RECT caja = {0, 0, cartel_w, cartel_h};
    DrawTextW(mdc, txt, -1, &caja, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();
    for (int i = 0; i < cartel_w * cartel_h; i++)
        px[i] = (px[i] & 0xFFFFFF) == 0xFF00FF ? 0 : (px[i] | 0xFF000000);
    glBindTexture(GL_TEXTURE_2D, tex_cartel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cartel_w, cartel_h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    SelectObject(mdc, vfo); SelectObject(mdc, vp); SelectObject(mdc, vb); SelectObject(mdc, vf);
    DeleteObject(fondo); DeleteObject(borde); DeleteObject(bmp); DeleteObject(fuente);
    DeleteDC(mdc);
}

static void gl_rect(float x, float y, float w, float h, float u0, float v0, float u1, float v1) {
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
    glTexCoord2f(u0, v1); glVertex2f(x, y + h);
    glEnd();
}

static void gl_pinta(HDC dc, int i, int subir) {
    RECT r;
    GetClientRect(ventana, &r);
    int e = video_escala(), iw = 256 * e, ih = 384 * e;
    const uint32_t *px = e > 1 ? marcos[i].hd : marcos[i].baja;
    glBindTexture(GL_TEXTURE_2D, tex_juego);
    if (tex_w != iw || tex_h != ih) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLint filtro = e > 1 ? GL_LINEAR : GL_NEAREST;  // con reescalado se reduce: suave; sin él, píxeles nítidos
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtro);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtro);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
        tex_w = iw; tex_h = ih;
    } else if (subir) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, iw, ih, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    }
    glViewport(0, 0, r.right, r.bottom);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, r.right, r.bottom, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glColor4f(1, 1, 1, 1);
    EnterCriticalSection(&cs_gdi);
    float x = (float)dst_x, y = (float)dst_y, w = (float)dst_w, h = (float)dst_h;
    int lado = pantalla_completa;
    LeaveCriticalSection(&cs_gdi);
    if (lado) {  // pantalla de arriba a la izquierda, táctil a la derecha
        gl_rect(x, y, w / 2, h, 0, 0, 1, 0.5f);
        gl_rect(x + w / 2, y, w / 2, h, 0, 0.5f, 1, 1);
    } else {
        gl_rect(x, y, w, h, 0, 0, 1, 1);
    }
    const char *av = repe_aviso();
    if (av) {
        gl_prepara_cartel(av);
        float alto = h / (lado ? 9.0f : 14.0f);
        if (alto < 24) alto = 24;
        float ancho = alto * (float)cartel_w / (float)cartel_h;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, tex_cartel);
        gl_rect(x + (w - ancho) / 2, y + alto / 3, ancho, alto, 0, 0, 1, 1);
    }
    menu_pinta(r.right, r.bottom, r.right, r.bottom);  // el menú de ajustes y el contador de FPS, si toca
    // pruebas: MKDS_CAPTURA_VENTANA=x.bmp guarda lo que se ve en la ventana (con el menú) a los 5 s y sale
    static const char *captura = (const char *)1;
    static int pintadas;
    if (captura == (const char *)1) captura = getenv("MKDS_CAPTURA_VENTANA");
    if (captura && ++pintadas == 300) {
        int w = r.right, hh = r.bottom;
        uint32_t *px = malloc((size_t)w * hh * 4);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, hh, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);  // de abajo arriba, como un BMP normal
        FILE *f = fopen(captura, "wb");
        if (f && px) {
            uint32_t tam = 54 + (uint32_t)w * hh * 4, off = 54, cab_tam = 40, planos_bits = 1 | (32 << 16);
            uint8_t cab[54] = {'B', 'M'};
            memcpy(cab + 2, &tam, 4); memcpy(cab + 10, &off, 4); memcpy(cab + 14, &cab_tam, 4);
            memcpy(cab + 18, &w, 4); memcpy(cab + 22, &hh, 4); memcpy(cab + 26, &planos_bits, 4);
            fwrite(cab, 1, 54, f);
            fwrite(px, 4, (size_t)w * hh, f);
            fclose(f);
        }
        ExitProcess(0);
    }
    SwapBuffers(dc);  // espera al refresco del monitor
}

static DWORD WINAPI hilo_pinta(LPVOID p) {
    (void)p;
    HDC dc = GetDC(ventana);
    int gl = !getenv("MKDS_GDI") && gl_inicia(dc);
    if (gl && !menu_inicia()) fprintf(stderr, "[ventana] no se pudo preparar el menú\n");
    int subido = -1;
    int64_t ts_subido = 0;
    for (;;) {
        if (!gl) WaitForSingleObject(ev_pintar, INFINITE);
        int i = elige_marco();
        if (i < 0) { if (gl) Sleep(1); continue; }
        if (gl) {
            int nuevo = i != subido || marco_ts[i] != ts_subido;
            gl_pinta(dc, i, nuevo);
            if (nuevo) InterlockedIncrement(&enseñadas);
            subido = i;
            ts_subido = marco_ts[i];
        } else {
            imagen = marcos[i].baja;
            imagen_hd = marcos[i].hd;
            EnterCriticalSection(&cs_gdi);
            dibuja_en(dc);
            LeaveCriticalSection(&cs_gdi);
        }
    }
}

void ventana_inicia(void) {
    InitializeCriticalSection(&cs_marcos);
    InitializeCriticalSection(&cs_gdi);
    ev_pintar = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (getenv("MKDS_OCULTO")) { oculto = 1; return; }
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(NULL, L"icono.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);  // el logo
    wc.lpszClassName = L"mkds";
    RegisterClassW(&wc);
    RECT r = {0, 0, 256 * ESCALA, 384 * ESCALA};
    DWORD estilo = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, estilo, FALSE);
    int fuera = getenv("MKDS_VENTANA_FUERA") != NULL;  // pruebas: la ventana fuera de la pantalla, sin molestar
    ventana = CreateWindowExW(fuera ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0, L"mkds", L"DS Kart Recompiled", estilo,
                              fuera ? -6000 : CW_USEDEFAULT, fuera ? -6000 : CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = 256;
    bmi.bmiHeader.biHeight = -384;  // de arriba abajo
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi_lado = bmi;
    bmi_lado.bmiHeader.biWidth = 512;
    bmi_lado.bmiHeader.biHeight = -192;
    int e = video_escala();
    bmi_hd = bmi;
    bmi_hd.bmiHeader.biWidth = 256 * e;
    bmi_hd.bmiHeader.biHeight = -384 * e;
    bmi_lado_hd = bmi;
    bmi_lado_hd.bmiHeader.biWidth = 512 * e;
    bmi_lado_hd.bmiHeader.biHeight = -192 * e;
    ShowWindow(ventana, fuera ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (getenv("MKDS_PANTALLA_COMPLETA") && atoi(getenv("MKDS_PANTALLA_COMPLETA"))) cambia_pantalla_completa();
    CreateThread(NULL, 0, hilo_mando_ps, NULL, 0, NULL);
    HANDLE h = CreateThread(NULL, 0, hilo_pinta, NULL, 0, NULL);
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);  // el juego (y su música) primero
    CloseHandle(h);
}

// MKDS_GRABA=portada.mp4 (tools/graba_portada.sh): graba el vídeo de la portada del launcher con el modo
// demostración del título. Detecta solas las carreras (hay 3D en pantalla) y de cada una guarda 15 s de la
// pantalla de arriba (desde el segundo 5, tras la salida) mandándolos a ffmpeg; tras MKDS_GRABA_CARRERAS
// carreras (7) cierra el vídeo y termina.
#include "gpu3d.h"
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
        ff = _popen(orden, "wb");
    }
    if (!ff) return;
    if (gx_polis() <= 50) { fuera++; return; }  // sin carrera (título, fundidos)
    if (fuera >= 120) { carrera++; fotogramas = 0; }  // 2 s sin 3D: es otra carrera
    fuera = 0;
    fotogramas++;
    if (fotogramas > 300 && fotogramas <= 300 + 900)
        fwrite(e > 1 ? hd : baja, 4, (size_t)256 * e * 192 * e, ff);  // la pantalla de arriba va primero
    if (carrera >= carreras_max && fotogramas == 300 + 900) {
        _pclose(ff);
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
    SetEvent(ev_pintar);
}

// imágenes intermedias (video.c): se rellena la imagen (a tamaño grande si hay reescalado) y se publica con
// la hora a la que toca enseñarla
uint32_t *ventana_reserva_intermedia(void) {
    Marco *m = reserva();
    return video_escala() > 1 ? m->hd : m->baja;
}
void ventana_publica_intermedia(int64_t ts) {
    if (oculto) { guarda_intermedia(); return; }
    publica(ts);
}

int64_t ventana_reloj(void) { return reloj(); }

void ventana_muestra(const uint32_t *arriba, const uint32_t *abajo) { ventana_muestra_en(arriba, abajo, 0); }

// una imagen del juego; ts = cuándo enseñarla (0 = ya)
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

// ---- mando de PlayStation (DualSense / DualShock 4) por HID --------------------
static volatile uint16_t ps_in = 0x3FF, ps_xy = 3;
static volatile int ps_combo, ps_producto;  // L3 + R3 pulsados; qué mando de PS está conectado

static HANDLE abre_mando_ps(void) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO lista = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (lista == INVALID_HANDLE_VALUE) return NULL;
    HANDLE res = NULL;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof ifd};
    for (DWORD i = 0; !res && SetupDiEnumDeviceInterfaces(lista, NULL, &guid, i, &ifd); i++) {
        DWORD tam = 0;
        SetupDiGetDeviceInterfaceDetailW(lista, &ifd, NULL, 0, &tam, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = malloc(tam);
        if (!det) continue;
        det->cbSize = sizeof *det;
        if (SetupDiGetDeviceInterfaceDetailW(lista, &ifd, det, tam, NULL, NULL)) {
            HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES at = {sizeof at};
                if (HidD_GetAttributes(h, &at) && at.VendorID == 0x054C &&
                    (at.ProductID == 0x0CE6 || at.ProductID == 0x0DF2 ||   // DualSense / Edge
                     at.ProductID == 0x05C4 || at.ProductID == 0x09CC))    // DualShock 4
                    res = h;
                else
                    CloseHandle(h);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(lista);
    return res;
}

// sticks, botones de caras + cruceta (b0), hombros (b1)
static void interpreta_ps(uint8_t lx, uint8_t ly, uint8_t b0, uint8_t b1) {
    uint16_t in = 0x3FF, xy = 3;
    int hat = b0 & 15;
    int arriba = hat == 7 || hat == 0 || hat == 1 || ly < 64;
    int derecha = hat == 1 || hat == 2 || hat == 3 || lx > 192;
    int abajo = hat == 3 || hat == 4 || hat == 5 || ly > 192;
    int izquierda = hat == 5 || hat == 6 || hat == 7 || lx < 64;
    if (b0 & 0x20) in &= ~1;          // cruz -> A
    if (b0 & 0x40) in &= ~2;          // círculo -> B
    if (b1 & 0x10) in &= ~4;          // Create -> Select
    if (b1 & 0x20) in &= ~8;          // Options -> Start
    if (derecha) in &= ~0x10;
    if (izquierda) in &= ~0x20;
    if (arriba) in &= ~0x40;
    if (abajo) in &= ~0x80;
    if (b1 & 0x0A) in &= ~0x100;      // R1 / R2 -> R
    if (b1 & 0x05) in &= ~0x200;      // L1 / L2 -> L
    if (b0 & 0x80) xy &= ~1;          // triángulo -> X
    if (b0 & 0x10) xy &= ~2;          // cuadrado -> Y
    ps_in = in;
    ps_xy = xy;
    ps_combo = (b1 & 0xC0) == 0xC0;   // L3 + R3: menú de ajustes
}

static DWORD WINAPI hilo_mando_ps(LPVOID p) {
    (void)p;
    for (;;) {
        HANDLE h = abre_mando_ps();
        if (!h) { Sleep(2000); continue; }
        HIDD_ATTRIBUTES at = {sizeof at};
        ps_producto = HidD_GetAttributes(h, &at) ? at.ProductID : 1;
        uint8_t buf[128];
        DWORD n;
        while (ReadFile(h, buf, sizeof buf, &n, NULL)) {
            if (buf[0] == 0x31 && n >= 12) {            // DualSense por Bluetooth (informe completo)
                interpreta_ps(buf[2], buf[3], buf[9], buf[10]);
            } else if (buf[0] == 0x01 && n >= 40) {     // DualSense por USB
                interpreta_ps(buf[1], buf[2], buf[8], buf[9]);
            } else if (buf[0] == 0x01 && n >= 9) {      // DualShock 4, o informe simple por Bluetooth
                interpreta_ps(buf[1], buf[2], buf[5], buf[6]);
            } else if (buf[0] == 0x11 && n >= 12) {     // DualShock 4 por Bluetooth
                interpreta_ps(buf[3], buf[4], buf[7], buf[8]);
            }
        }
        CloseHandle(h);  // desconectado: esperar a que vuelva
        ps_in = 0x3FF;
        ps_xy = 3;
        ps_combo = ps_producto = 0;
    }
}

// Mando: se suma a lo que se pulse en el teclado
static void lee_mando(uint16_t *in, uint16_t *xy, int *combo) {
    *in = 0x3FF;
    *xy = 3;
    *combo = 0;
    XINPUT_STATE st;
    for (DWORD i = 0; i < 4; i++) {
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        WORD b = st.Gamepad.wButtons;
        SHORT lx = st.Gamepad.sThumbLX, ly = st.Gamepad.sThumbLY;
        const SHORT zona = 12000;
        struct { int pulsado, bit; } m[] = {
            {b & XINPUT_GAMEPAD_A, 0}, {b & XINPUT_GAMEPAD_B, 1}, {b & XINPUT_GAMEPAD_BACK, 2},
            {b & XINPUT_GAMEPAD_START, 3}, {(b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > zona, 4},
            {(b & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -zona, 5}, {(b & XINPUT_GAMEPAD_DPAD_UP) || ly > zona, 6},
            {(b & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -zona, 7},
            {(b & XINPUT_GAMEPAD_RIGHT_SHOULDER) || st.Gamepad.bRightTrigger > 60, 8},
            {(b & XINPUT_GAMEPAD_LEFT_SHOULDER) || st.Gamepad.bLeftTrigger > 60, 9},
        };
        for (int k = 0; k < 10; k++)
            if (m[k].pulsado) *in &= (uint16_t)~(1u << m[k].bit);
        if (b & XINPUT_GAMEPAD_Y) *xy &= ~1;  // por posición, como la DS (y como los de PlayStation): arriba = X
        if (b & XINPUT_GAMEPAD_X) *xy &= ~2;  // izquierda = Y
        if ((b & XINPUT_GAMEPAD_LEFT_THUMB) && (b & XINPUT_GAMEPAD_RIGHT_THUMB)) *combo = 1;
    }
}

// Botones que pulsa el guion de pruebas (MKDS_GUION, en ds.c): se suman al teclado y al mando
volatile uint16_t g_guion_in = 0x3FF, g_guion_xy = 3;

// los mandos conectados (para el menú)
int ventana_mandos(char nombres[][64], int max) {
    int n = 0;
    XINPUT_STATE st;
    for (DWORD i = 0; i < 4 && n < max; i++)
        if (XInputGetState(i, &st) == ERROR_SUCCESS) snprintf(nombres[n++], 64, "Xbox / XInput %lu", i + 1);
    int p = ps_producto;
    if (p && n < max)
        snprintf(nombres[n++], 64, "%s", p == 0x05C4 || p == 0x09CC ? "DualShock 4" : p == 0x0DF2 ? "DualSense Edge" : "DualSense");
    return n;
}

void ventana_mensajes(void) {
    uint16_t mando_in = 0x3FF, mando_xy = 3;
    int combo = 0;
    if (!oculto) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        lee_mando(&mando_in, &mando_xy, &combo);
    }
    static int combo_antes;  // L3 + R3: abre o cierra el menú
    combo |= ps_combo;
    if (combo && !combo_antes) menu_alterna();
    combo_antes = combo;
    if (pide_pc) {
        if ((pide_pc == 1) != (pantalla_completa != 0)) cambia_pantalla_completa();
        pide_pc = 0;
    }
    if (menu_abierto()) {  // el menú se queda los botones; el juego no ve nada pulsado
        menu_mando(mando_in & ps_in);
        g_keyinput = g_guion_in;
        g_keyxy = g_guion_xy;
        g_lapiz = 0;
        return;
    }
    g_keyinput = teclado_in & mando_in & ps_in & g_guion_in;
    g_keyxy = (uint16_t)(teclado_xy & mando_xy & ps_xy & g_guion_xy);
}

// Para guardar una captura (MKDS_CAPTURA=archivo.bmp)
void ventana_guarda_bmp(const char *ruta) {
    FILE *f = fopen(ruta, "wb");
    if (!f) return;
    int e = video_escala();  // con reescalado, a tamaño grande
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
    fwrite(e > 1 ? ultimo->hd : ultimo->baja, 4, (size_t)256 * 384 * e * e, f);
    fclose(f);
}

void ventana_guarda_bmp_de(const char *ruta, const uint32_t *px, int e) {
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

// Contador de FPS en el título de la ventana
void ventana_titulo_fps(int fps, int fps3d) {
    int pantalla = (int)InterlockedExchange(&enseñadas, 0);
    menu_pon_fps(fps, fps3d, pantalla);
    if (oculto || !ventana) return;
    wchar_t t[128];
    const char *av = repe_aviso();
    wchar_t avw[128];
    if (av) MultiByteToWideChar(CP_UTF8, 0, av, -1, avw, 128);
    if (av) swprintf(t, 128, L"DS Kart Recompiled  -  %ls", avw);
    else swprintf(t, 128, L"DS Kart Recompiled  -  %d FPS  (3D: %d)  pantalla: %d", fps, fps3d, pantalla);
    SetWindowTextW(ventana, t);
}
