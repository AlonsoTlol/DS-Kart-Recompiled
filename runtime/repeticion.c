// Repetición instantánea (como NVIDIA App o Medal): siempre se guardan en memoria
// los últimos 30 segundos de imagen y sonido; con F10 se convierten en un MP4
// (con ffmpeg, en segundo plano) dentro de la carpeta "repeticiones".
//
// Imagen: 30 fotogramas por segundo, las dos pantallas lado a lado (principal a
// la izquierda), en 15 bits por píxel. Sonido: el mismo que sale por los altavoces.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SEGUNDOS 30
#define FPS_REPE 30
#define ANCHO 512
#define ALTO 192
#define FRAMES (SEGUNDOS * FPS_REPE)
#define FREC 32768
#define MUESTRAS_AUDIO (SEGUNDOS * FREC)

static uint16_t *video;          // FRAMES imágenes de ANCHO x ALTO
static volatile uint64_t n_frames;
static int16_t *audio;           // estéreo
static volatile uint64_t n_audio;
static volatile int guardando;
static char aviso[128];
static DWORD aviso_hasta;
static uint64_t contador_frames;

static inline uint16_t a15(uint32_t c) {
    return (uint16_t)(((c >> 19) & 31) | ((c >> 11) & 31) << 5 | ((c >> 3) & 31) << 10);
}

void repe_frame(const uint32_t *arriba, const uint32_t *abajo) {
    if (++contador_frames % (60 / FPS_REPE)) return;  // la DS va a 60: se guarda uno de cada dos
    if (!video) {
        video = malloc((size_t)FRAMES * ANCHO * ALTO * 2);
        if (!video) return;
    }
    uint16_t *f = video + (size_t)(n_frames % FRAMES) * ANCHO * ALTO;
    for (int y = 0; y < ALTO; y++)
        for (int x = 0; x < 256; x++) {
            f[y * ANCHO + x] = a15(arriba[y * 256 + x]);
            f[y * ANCHO + 256 + x] = a15(abajo[y * 256 + x]);
        }
    n_frames++;
}

void repe_audio(const int16_t *m, int n) {
    if (!audio) {
        audio = malloc((size_t)MUESTRAS_AUDIO * 4);
        if (!audio) return;
    }
    for (int i = 0; i < n; i++) {
        size_t k = (size_t)(n_audio % MUESTRAS_AUDIO);
        audio[k * 2] = m[i * 2];
        audio[k * 2 + 1] = m[i * 2 + 1];
        n_audio++;
    }
}

static void escribe_wav(FILE *f, const int16_t *d, uint32_t muestras) {
    uint32_t datos = muestras * 4, tam = 36 + datos, fmt = 16, frec = FREC, bps = FREC * 4;
    uint16_t pcm = 1, canales = 2, bloque = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&tam, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&canales, 2, 1, f);
    fwrite(&frec, 4, 1, f); fwrite(&bps, 4, 1, f); fwrite(&bloque, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&datos, 4, 1, f);
    fwrite(d, 4, muestras, f);
}

static void pon_aviso(const char *t, DWORD ms) {
    snprintf(aviso, sizeof aviso, "%s", t);
    aviso_hasta = GetTickCount() + ms;
}

static DWORD WINAPI hilo_guarda(LPVOID p) {
    (void)p;
    // copia de lo grabado hasta ahora (el juego sigue escribiendo mientras)
    uint64_t nf = n_frames, na = n_audio;
    uint32_t frames = (uint32_t)(nf < FRAMES ? nf : FRAMES);
    uint32_t muestras = (uint32_t)((uint64_t)frames * FREC / FPS_REPE);
    if (muestras > na) muestras = (uint32_t)(na < MUESTRAS_AUDIO ? na : MUESTRAS_AUDIO);
    if (frames < FPS_REPE) { pon_aviso("Aún no hay suficiente grabado", 3000); guardando = 0; return 0; }
    CreateDirectoryA("repeticiones", NULL);
    char raw[MAX_PATH], wav[MAX_PATH], mp4[MAX_PATH];
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    snprintf(raw, sizeof raw, "repeticiones\\tmp_video.raw");
    snprintf(wav, sizeof wav, "repeticiones\\tmp_audio.wav");
    snprintf(mp4, sizeof mp4, "repeticiones\\MKDS_%04d-%02d-%02d_%02d-%02d-%02d.mp4", lt->tm_year + 1900,
             lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
    FILE *f = fopen(raw, "wb");
    if (!f) { guardando = 0; return 0; }
    for (uint32_t i = 0; i < frames; i++) {
        uint64_t k = (nf - frames + i) % FRAMES;
        fwrite(video + (size_t)k * ANCHO * ALTO, 2, (size_t)ANCHO * ALTO, f);
    }
    fclose(f);
    int16_t *a = malloc((size_t)muestras * 4 + 4);
    if (a) {
        for (uint32_t i = 0; i < muestras; i++) {
            uint64_t k = (na - muestras + i) % MUESTRAS_AUDIO;
            a[i * 2] = audio ? audio[k * 2] : 0;
            a[i * 2 + 1] = audio ? audio[k * 2 + 1] : 0;
        }
        f = fopen(wav, "wb");
        if (f) { escribe_wav(f, a, muestras); fclose(f); }
        free(a);
    }
    pon_aviso("Guardando repetición...", 60000);
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -y -loglevel error -f rawvideo -pixel_format bgr555le -video_size %dx%d -framerate %d -i \"%s\" "
             "-i \"%s\" -vf scale=%d:%d:flags=neighbor -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p "
             "-c:a aac -b:a 160k -shortest \"%s\"",
             ANCHO, ALTO, FPS_REPE, raw, wav, ANCHO * 2, ALTO * 2, mp4);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD codigo = 1;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL,
                       &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &codigo);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    DeleteFileA(raw);
    DeleteFileA(wav);
    if (codigo == 0) {
        char m[128];
        snprintf(m, sizeof m, "¡Repetición guardada! (%u s)", frames / FPS_REPE);
        pon_aviso(m, 4000);
    } else {
        pon_aviso("No se pudo guardar la repetición", 5000);
    }
    guardando = 0;
    return 0;
}

void repe_guarda(void) {
    if (guardando) return;
    guardando = 1;
    pon_aviso("Guardando repetición...", 60000);
    CloseHandle(CreateThread(NULL, 0, hilo_guarda, NULL, 0, NULL));
}

// Texto para el título de la ventana mientras dura el aviso (NULL si no hay)
const char *repe_aviso(void) {
    return (aviso[0] && GetTickCount() < aviso_hasta) ? aviso : NULL;
}
