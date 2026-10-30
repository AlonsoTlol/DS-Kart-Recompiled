// Micrófono del PC para el micrófono de la DS (Batalla de globos: soplar infla el globo).
// Se abre solo cuando el juego empieza a grabar y se cierra cuando para. Graba a 8000 muestras por segundo,
// 16 bits; ds.c (mic_actualiza) va pidiendo muestras al ritmo del juego. Sin micrófono, o con MKDS_MICRO=0
// (MICRO=0 en ajustes.ini, opción del launcher), da silencio.
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <SDL2/SDL.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBUF 6
#define MUESTRAS 400   // por búfer: 50 ms
static int16_t anillo[8192];
static int lee, escribe;  // anillo de lo grabado
#ifdef _WIN32
static HWAVEIN entrada;
static WAVEHDR cab[NBUF];
static int16_t datos[NBUF][MUESTRAS];
#else
static SDL_AudioDeviceID entrada;
#endif

static int permitido(void) {
    const char *m = getenv("MKDS_MICRO");
    return !m || atoi(m) != 0;
}

#ifndef _WIN32
// En Linux lo graba SDL en su hilo y lo deja en el anillo
static void SDLCALL llega(void *u, Uint8 *d, int bytes) {
    (void)u;
    const int16_t *m = (const int16_t *)d;
    for (int k = 0; k < bytes / 2; k++) {
        anillo[escribe] = m[k];
        escribe = (escribe + 1) % 8192;
        if (escribe == lee) lee = (lee + 1) % 8192;  // lleno: se pierde lo más viejo
    }
}

void mic_pc_abre(void) {
    if (entrada || !permitido()) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec quiero = {0}, tengo;
    quiero.freq = 8000;
    quiero.format = AUDIO_S16SYS;
    quiero.channels = 1;
    quiero.samples = MUESTRAS;
    quiero.callback = llega;
    const char *nombre = getenv("MKDS_MICRO_DISPOSITIVO");
    entrada = SDL_OpenAudioDevice(nombre && *nombre ? nombre : NULL, 1, &quiero, &tengo, 0);
    if (!entrada && nombre && *nombre) entrada = SDL_OpenAudioDevice(NULL, 1, &quiero, &tengo, 0);
    fprintf(stderr, "[mkds] micrófono del PC: %s\n", entrada ? "abierto" : SDL_GetError());
    lee = escribe = 0;
    if (entrada) SDL_PauseAudioDevice(entrada, 0);
}

void mic_pc_cierra(void) {
    if (!entrada) return;
    SDL_CloseAudioDevice(entrada);
    entrada = 0;
}

static void recoge(void) {}
#else
void mic_pc_abre(void) {
    if (entrada || !permitido()) return;
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, 8000, 16000, 2, 16, 0};
    // MKDS_MICRO_DISPOSITIVO (MICRO_DISPOSITIVO en ajustes.ini, lo elige el launcher): el nombre del micrófono
    // tal como lo da Windows (waveInGetDevCaps, 31 letras como mucho); si no está, el predeterminado
    UINT disp = WAVE_MAPPER;
    const char *nombre = getenv("MKDS_MICRO_DISPOSITIVO");
    if (nombre && *nombre) {
        wchar_t buscado[64];
        MultiByteToWideChar(CP_UTF8, 0, nombre, -1, buscado, 64);
        for (UINT i = 0; i < waveInGetNumDevs(); i++) {
            WAVEINCAPSW caps;
            if (waveInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR && !wcscmp(caps.szPname, buscado)) {
                disp = i;
                break;
            }
        }
    }
    MMRESULT r = waveInOpen(&entrada, disp, &f, 0, 0, CALLBACK_NULL);
    fprintf(stderr, "[mkds] micrófono del PC: dispositivo %d (%s), %s\n", disp == WAVE_MAPPER ? -1 : (int)disp,
            nombre && *nombre ? nombre : "predeterminado", r == MMSYSERR_NOERROR ? "abierto" : "NO se pudo abrir");
    if (r != MMSYSERR_NOERROR) { entrada = NULL; return; }
    for (int i = 0; i < NBUF; i++) {
        memset(&cab[i], 0, sizeof cab[i]);
        cab[i].lpData = (LPSTR)datos[i];
        cab[i].dwBufferLength = sizeof datos[i];
        waveInPrepareHeader(entrada, &cab[i], sizeof cab[i]);
        waveInAddBuffer(entrada, &cab[i], sizeof cab[i]);
    }
    lee = escribe = 0;
    waveInStart(entrada);
}

void mic_pc_cierra(void) {
    if (!entrada) return;
    waveInReset(entrada);
    for (int i = 0; i < NBUF; i++) waveInUnprepareHeader(entrada, &cab[i], sizeof cab[i]);
    waveInClose(entrada);
    entrada = NULL;
}

// pasa al anillo los búferes que ya ha llenado Windows y los vuelve a poner a grabar
static void recoge(void) {
    for (int i = 0; i < NBUF; i++) {
        if (!(cab[i].dwFlags & WHDR_DONE)) continue;
        int n = (int)cab[i].dwBytesRecorded / 2;
        static int pico, cuenta;  // para saber qué llega: el pico de cada segundo
        for (int k = 0; k < n; k++) {
            int a = datos[i][k] < 0 ? -datos[i][k] : datos[i][k];
            if (a > pico) pico = a;
        }
        cuenta += n;
        if (cuenta >= 8000) {
            fprintf(stderr, "[mkds] micrófono del PC: pico del último segundo %d de 32767\n", pico);
            pico = cuenta = 0;
        }
        for (int k = 0; k < n; k++) {
            anillo[escribe] = datos[i][k];
            escribe = (escribe + 1) % 8192;
            if (escribe == lee) lee = (lee + 1) % 8192;  // lleno: se pierde lo más viejo
        }
        cab[i].dwFlags &= ~WHDR_DONE;
        waveInAddBuffer(entrada, &cab[i], sizeof cab[i]);
    }
}
#endif

// El juego (0201EC40) decide que soplas si muchas muestras llegan al tope (|s| >= 125 de 127) y por la forma
// de la onda: el micrófono de la DS, al soplar, se satura. El del PC graba mucho más flojo (un Razer Seiren da
// picos de ~550 en una habitación en silencio), así que se amplifica y se recorta como haría la DS.
// MKDS_MICRO_GANANCIA (MICRO_GANANCIA en ajustes.ini): por cuánto se multiplica; 8 si no se dice.
static int16_t amplifica(int s) {
    static int ganancia = -1;
    if (ganancia < 0) {
        const char *g = getenv("MKDS_MICRO_GANANCIA");
        ganancia = g && atoi(g) > 0 ? atoi(g) : 8;
    }
    s *= ganancia;
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

// la siguiente muestra (silencio si no hay micrófono o aún no ha llegado nada)
int16_t mic_pc_muestra(void) {
    // MKDS_MICRO_PRUEBA=N: como si alguien soplara con fuerza N (de 32767), para probar sin micrófono: ruido
    // grave (el de soplar), no blanco
    static int prueba = -1;
    static double grave;
    if (prueba < 0) prueba = getenv("MKDS_MICRO_PRUEBA") ? atoi(getenv("MKDS_MICRO_PRUEBA")) : 0;
    if (prueba == 1) prueba = 32767;
    if (prueba) {
        static double k = -1;  // MKDS_MICRO_GRAVE: cuánto se suaviza el ruido (0 = blanco, 0,99 = muy grave)
        if (k < 0) k = getenv("MKDS_MICRO_GRAVE") ? atof(getenv("MKDS_MICRO_GRAVE")) : 0.9;
        grave = k * grave + (rand() % 2001 - 1000) / 1000.0 * prueba * 0.45;
        return amplifica((int)grave);
    }
    if (!entrada) return 0;
    if (lee == escribe) recoge();
    if (lee == escribe) return 0;
    int16_t s = anillo[lee];
    lee = (lee + 1) % 8192;
    return amplifica(s);
}
