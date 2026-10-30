// Sonido de la DS: los 16 canales del ARM7, mezclados a 32768 Hz y enviados a
// los altavoces del PC con waveOut, en un hilo aparte.
//
// El driver de sonido de Nintendo (en el ARM7 traducido) programa los canales
// escribiendo en 0x04000400 + 16 * canal:
//   +0 CNT  volumen (0-6), divisor (8-9), panorama (16-22), ciclo PSG (24-26),
//           repetición (27-28: 0 manual, 1 bucle, 2 una vez), formato (29-30:
//           PCM8, PCM16, ADPCM, PSG/ruido), en marcha (31)
//   +4 SAD  dirección de las muestras    +8 TMR  frecuencia (16,76 MHz / (0x10000 - TMR))
//   +A PNT  inicio del bucle (palabras)  +C LEN  longitud tras el bucle (palabras)
// y en 0x04000500 el volumen general.
#include "compat.h"  // antes que cpu.h: su macro R choca con windows.h
#ifdef _WIN32
#include <mmsystem.h>
#else
#include <SDL2/SDL.h>
#endif
#include "ds.h"
#include <stdio.h>
#include <stdlib.h>

#define FREC 32768
#define MUESTRAS 512   // por búfer (estéreo, 16 bits)
#define NBUF 6
#define RELOJ_SONIDO (33513982.0 / 2)

extern uint8_t *ds_io7(void);
volatile uint32_t g_snd_gen[16];  // cambia cada vez que el juego arranca un canal

typedef struct {
    uint32_t gen;
    int activo;
    double pos;          // en muestras
    int16_t adpcm, adpcm_bucle;
    int idx, idx_bucle, bucle_guardado;
    uint32_t ultimo;     // última muestra ADPCM decodificada (índice)
    uint16_t lfsr;
    int16_t actual;
} Canal;

static Canal canal[16];
static volatile int volumen = 100, silencio;  // del menú (0-100)

void sonido_pon_volumen(int v, int s) {
    volumen = v < 0 ? 0 : v > 100 ? 100 : v;
    silencio = s;
}
void repe_audio(const int16_t *m, int n);  // repeticion.c
static const int adpcm_idx[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const uint16_t adpcm_paso[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static uint32_t r32(const uint8_t *io, uint32_t o) { uint32_t v; memcpy(&v, io + o, 4); return v; }
static uint16_t r16(const uint8_t *io, uint32_t o) { uint16_t v; memcpy(&v, io + o, 2); return v; }

static void adpcm_paso1(Canal *c, uint8_t n) {
    int paso = adpcm_paso[c->idx];
    int dif = paso / 8;
    if (n & 1) dif += paso / 4;
    if (n & 2) dif += paso / 2;
    if (n & 4) dif += paso;
    int s = c->adpcm + ((n & 8) ? -dif : dif);
    if (s > 0x7FFF) s = 0x7FFF;
    if (s < -0x7FFF) s = -0x7FFF;
    c->adpcm = (int16_t)s;
    c->idx += adpcm_idx[n & 7];
    if (c->idx < 0) c->idx = 0;
    if (c->idx > 88) c->idx = 88;
}

static void reinicia(Canal *c, uint32_t sad) {
    c->activo = 1;
    c->pos = 0;
    c->bucle_guardado = 0;
    c->ultimo = 0;
    c->lfsr = 0x7FFF;
    uint32_t h = rd32_7(sad);  // ADPCM: cabecera con la primera muestra y el índice
    c->adpcm = (int16_t)(h & 0xFFFF);
    c->idx = (int)((h >> 16) & 0x7F);
    if (c->idx > 88) c->idx = 88;
}

// Una muestra del canal n (y avanza)
static int16_t muestra(int n, const uint8_t *io, double paso) {
    Canal *c = &canal[n];
    uint32_t o = 0x400 + 16 * n;
    uint32_t cnt = r32(io, o), sad = r32(io, o + 4) & 0x07FFFFFC;
    uint32_t pnt = r16(io, o + 0xA), len = r32(io, o + 0xC) & 0x3FFFFF;
    int formato = (cnt >> 29) & 3, rep = (cnt >> 27) & 3;
    uint32_t total, bucle;
    switch (formato) {
    case 0: total = (pnt + len) * 4; bucle = pnt * 4; break;
    case 1: total = (pnt + len) * 2; bucle = pnt * 2; break;
    case 2: total = (pnt + len) * 8; bucle = pnt * 8; if (total >= 8) total -= 8; bucle = bucle >= 8 ? bucle - 8 : 0; break;
    default: total = 0xFFFFFFFF; bucle = 0; break;
    }
    uint32_t p = (uint32_t)c->pos;
    if (p >= total) {
        if (rep == 2 || total == 0) {  // una vez: se acabó
            c->activo = 0;
            uint8_t *w = ds_io7();
            w[o + 3] &= 0x7F;
            return 0;
        }
        c->pos = bucle + (c->pos - total);
        p = (uint32_t)c->pos;
        if (formato == 2 && c->bucle_guardado) {
            c->adpcm = c->adpcm_bucle; c->idx = c->idx_bucle; c->ultimo = bucle;
        }
    }
    int16_t s = 0;
    switch (formato) {
    case 0: s = (int16_t)((int8_t)rd8_7(sad + p) << 8); break;
    case 1: s = (int16_t)rd16_7(sad + p * 2); break;
    case 2:
        while (c->ultimo <= p) {
            if (c->ultimo == bucle && !c->bucle_guardado) {
                c->adpcm_bucle = c->adpcm; c->idx_bucle = c->idx; c->bucle_guardado = 1;
            }
            uint8_t b = (uint8_t)rd8_7(sad + 4 + c->ultimo / 2);
            adpcm_paso1(c, (c->ultimo & 1) ? b >> 4 : b & 15);
            c->ultimo++;
        }
        s = c->adpcm;
        break;
    default:
        if (n >= 8 && n <= 13) {  // onda cuadrada
            int ciclo = (cnt >> 24) & 7;
            s = ((p & 7) <= (uint32_t)ciclo) ? 0x7FFF : -0x7FFF;
        } else if (n >= 14) {  // ruido
            uint32_t antes = (uint32_t)(c->pos - paso);
            for (uint32_t k = antes; k < p; k++) {
                int bit = c->lfsr & 1;
                c->lfsr >>= 1;
                if (bit) c->lfsr ^= 0x6000;
            }
            s = (c->lfsr & 1) ? -0x7FFF : 0x7FFF;
        }
        break;
    }
    c->pos += paso;
    return s;
}

static void mezcla(int16_t *sal, int n_muestras) {
    const uint8_t *io = ds_io7();
    uint16_t soundcnt = r16(io, 0x500);
    int maestro = soundcnt & 0x7F;
    for (int i = 0; i < n_muestras; i++) {
        int32_t izq = 0, der = 0;
        if (soundcnt & 0x8000) {
            for (int n = 0; n < 16; n++) {
                uint32_t o = 0x400 + 16 * n;
                uint32_t cnt = r32(io, o);
                Canal *c = &canal[n];
                if (c->gen != g_snd_gen[n]) { c->gen = g_snd_gen[n]; reinicia(c, r32(io, o + 4) & 0x07FFFFFC); }
                if (!(cnt & 0x80000000) || !c->activo) continue;
                uint16_t tmr = r16(io, o + 8);
                double paso = RELOJ_SONIDO / (double)(0x10000 - tmr) / FREC;
                int32_t s = muestra(n, io, paso);
                int vol = cnt & 0x7F, div = (cnt >> 8) & 3, pan = (cnt >> 16) & 0x7F;
                static const int desp[4] = {0, 1, 2, 4};
                s = (s * vol / 128) >> desp[div];
                izq += s * (128 - pan) / 128;
                der += s * pan / 128;
            }
        }
        int vol = silencio ? 0 : volumen;
        izq = izq * maestro / 128 / 4 * vol / 100;
        der = der * maestro / 128 / 4 * vol / 100;
        if (izq > 32767) izq = 32767;
        if (izq < -32768) izq = -32768;
        if (der > 32767) der = 32767;
        if (der < -32768) der = -32768;
        sal[i * 2] = (int16_t)izq;
        sal[i * 2 + 1] = (int16_t)der;
    }
}

#ifdef _WIN32
static DWORD WINAPI hilo_sonido(LPVOID p) {
    (void)p;
    HANDLE evento = CreateEventW(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, FREC, FREC * 4, 4, 16, 0};
    HWAVEOUT wo;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)evento, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[sonido] no se pudo abrir la salida de audio\n");
        return 0;
    }
    static int16_t datos[NBUF][MUESTRAS * 2];
    static WAVEHDR cab[NBUF];
    for (int b = 0; b < NBUF; b++) {
        cab[b].lpData = (LPSTR)datos[b];
        cab[b].dwBufferLength = sizeof datos[b];
        waveOutPrepareHeader(wo, &cab[b], sizeof cab[b]);
        mezcla(datos[b], MUESTRAS);
        repe_audio(datos[b], MUESTRAS);
        waveOutWrite(wo, &cab[b], sizeof cab[b]);
    }
    for (;;) {
        WaitForSingleObject(evento, 100);
        for (int b = 0; b < NBUF; b++) {
            if (!(cab[b].dwFlags & WHDR_DONE)) continue;
            mezcla(datos[b], MUESTRAS);
        repe_audio(datos[b], MUESTRAS);
            waveOutWrite(wo, &cab[b], sizeof cab[b]);
        }
    }
}

void sonido_inicia(void) {
    if (getenv("MKDS_OCULTO") || getenv("MKDS_SIN_SONIDO")) return;  // pruebas: en silencio
    HANDLE h = CreateThread(NULL, 0, hilo_sonido, NULL, 0, NULL);
    SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
}
#else
// En Linux, SDL pide las muestras cuando le hacen falta (en su propio hilo)
static void SDLCALL pide_sonido(void *u, Uint8 *datos, int bytes) {
    (void)u;
    int n = bytes / 4;
    mezcla((int16_t *)datos, n);
    repe_audio((int16_t *)datos, n);
}

void sonido_inicia(void) {
    if (getenv("MKDS_OCULTO") || getenv("MKDS_SIN_SONIDO")) return;  // pruebas: en silencio
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "[sonido] SDL: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec quiero = {0}, tengo;
    quiero.freq = FREC;
    quiero.format = AUDIO_S16SYS;
    quiero.channels = 2;
    quiero.samples = MUESTRAS;
    quiero.callback = pide_sonido;
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(NULL, 0, &quiero, &tengo, 0);  // SDL convierte si hace falta
    if (!d) { fprintf(stderr, "[sonido] no se pudo abrir la salida de audio: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(d, 0);
}
#endif

int sonido_activo(void) { return !getenv("MKDS_OCULTO") && !getenv("MKDS_SIN_SONIDO"); }
