// Hardware de la DS para el código recompilado de los dos procesadores.
//
// Los dos procesadores se turnan en el mismo hilo de Windows (cada uno en su
// fibra): cada cierto "presupuesto" (c->ciclos) el código traducido llama a
// cpu_tick, que mira el reloj, entrega interrupciones y deja correr al otro.
// Así no hay carreras entre ARM9 y ARM7.
//
// El tiempo es el del PC: una VBlank cada 1/60 s, temporizadores a 33,5 MHz.
#include <windows.h>  // antes que cpu.h: su macro R choca con windows.h
#include "ds.h"
#include "video.h"
#include "gpu3d.h"

// sonido.c
void sonido_inicia(void);
int sonido_activo(void);
extern volatile uint32_t g_snd_gen[16];
uint8_t *ds_io7(void);
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

Cpu g_cpu9, g_cpu7;
static int g_traza;  // MKDS_TRAZA=1: apunta mensajes de la FIFO e interrupciones
static uint64_t cont_tarjeta, cont_3d;  // bytes leídos de la ROM, escrituras al motor 3D
uint8_t g_wramcnt = 3;
SoConfig g_so[2];
volatile uint32_t g_actividad;  // para el vigilante de main_mkds.c

#define RELOJ 33513982.0      // ciclos por segundo del bus de la DS
#define CICLOS_LINEA 2130
#define LINEAS 263
#define CICLOS_FRAME (CICLOS_LINEA * LINEAS)
#define TURNO 20000           // presupuesto entre tick y tick

typedef struct {
    uint8_t io[0x10000];      // registros guardados tal cual se escriben
    uint32_t ie, if_, ime;
    uint16_t dispstat;
    struct {
        uint16_t recarga, ctl;
        uint64_t inicio;      // ciclo en que arrancó
        uint64_t desbordes;   // desbordes ya avisados
        uint32_t pend;        // interrupciones aún por entregar (si saltó varias veces entre turnos)
    } tm[4];
    uint16_t sync;            // IPCSYNC: lo que este procesador escribe
    uint16_t fifocnt;
    uint32_t cola[16];        // FIFO de entrada (lo que manda el otro)
    int n;
    uint32_t ultimo;          // último valor leído de la FIFO
    int parado;               // esperando interrupción
    void *fibra;              // dónde sigue este procesador cuando le toca
} Hw;

static Hw hw[2];
static LARGE_INTEGER t0, frec;
static uint64_t frame_visto;   // última VBlank entregada
static int linea_vista[2];

static Cpu *cpu_de(int id) { return id ? &g_cpu7 : &g_cpu9; }

static uint64_t ahora(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)(t.QuadPart - t0.QuadPart) * RELOJ / (double)frec.QuadPart);
}

static uint32_t reg32(Hw *h, uint32_t a) { uint32_t v; memcpy(&v, h->io + (a & 0xFFFF), 4); return v; }
static void pon_reg32(Hw *h, uint32_t a, uint32_t v) { memcpy(h->io + (a & 0xFFFF), &v, 4); }

static uint64_t ahora(void);
static uint64_t m_irq_desde[2], m_lat[2], m_lat_max[2], m_lat_n[2], m_tarde[2], m_tarde_tot[2];  // retraso entre que salta una IRQ y se atiende
static void irq(int id, uint32_t bits) {
    if (!(hw[id].if_ & hw[id].ie) && (bits & hw[id].ie) && !m_irq_desde[id]) m_irq_desde[id] = ahora();
    hw[id].if_ |= bits;
}

// MKDS_MIDE=1: cada segundo, cómo van los temporizadores y los turnos (para la música lenta)
static int mide = -1;
static uint64_t m_desb[2][4], m_perd[2][4], m_aten[2][4], m_hueco7, m_hueco7_max, m_vuelta7, m_turnos[2], m_video, m_video_max;

// ---- temporizadores --------------------------------------------------------
static const int prescaler[4] = {1, 64, 256, 1024};

static uint64_t tm_ticks(Hw *h, int n, uint64_t t) {
    uint16_t ctl = h->tm[n].ctl;
    if (!(ctl & 0x80)) return 0;
    if ((ctl & 4) && n > 0) {  // en cascada: cuenta desbordes del anterior
        uint64_t p = 0x10000 - h->tm[n - 1].recarga;
        return p ? tm_ticks(h, n - 1, t) / p : 0;
    }
    return (t - h->tm[n].inicio) / prescaler[ctl & 3];
}

static uint16_t tm_valor(Hw *h, int n, uint64_t t) {
    if (!(h->tm[n].ctl & 0x80)) return h->tm[n].recarga;
    uint64_t p = 0x10000 - h->tm[n].recarga;
    return (uint16_t)(h->tm[n].recarga + tm_ticks(h, n, t) % p);
}

static void tm_control(Hw *h, int n, uint16_t v, uint64_t t) {
    if ((v & 0x80) && !(h->tm[n].ctl & 0x80)) { h->tm[n].inicio = t; h->tm[n].desbordes = 0; }
    h->tm[n].ctl = v;
}

// ---- DMA -------------------------------------------------------------------
static void dma_ejecuta(int id, int n) {
    Hw *h = &hw[id];
    Cpu *c = cpu_de(id);
    uint32_t base = 0x040000B0 + 12 * n;
    uint32_t src = reg32(h, base), dst = reg32(h, base + 4), cnt = reg32(h, base + 8);
    uint32_t num = cnt & (id ? 0xFFFF : 0x1FFFFF);
    if (num == 0) num = id ? 0x10000 : 0x200000;
    int ancho = (cnt >> 26) & 1 ? 4 : 2;
    int dctl = (cnt >> 21) & 3, sctl = (cnt >> 23) & 3;
    int32_t ds = dctl == 1 ? -ancho : dctl == 2 ? 0 : ancho;
    int32_t ss = sctl == 1 ? -ancho : sctl == 2 ? 0 : ancho;
    for (uint32_t i = 0; i < num; i++) {
        if (ancho == 4) bwr32(c, dst, brd32(c, src));
        else bwr16(c, dst, brd16(c, src));
        src += ss; dst += ds;
    }
    pon_reg32(h, base, src);
    if (dctl != 3) pon_reg32(h, base + 4, dst);
    int cuando = id ? (cnt >> 28) & 3 : (cnt >> 27) & 7;
    if (!(cnt & 0x02000000) || cuando == 0) pon_reg32(h, base + 8, cnt & ~0x80000000u);
    if (cnt & 0x40000000) irq(id, 0x100u << n);
}

// Mira si hay DMAs esperando el momento `cuando` y las hace
static void dma_momento(int id, int cuando) {
    for (int n = 0; n < 4; n++) {
        uint32_t cnt = reg32(&hw[id], 0x040000B8 + 12 * n);
        int t = id ? (cnt >> 28) & 3 : (cnt >> 27) & 7;
        if ((cnt & 0x80000000) && t == cuando) dma_ejecuta(id, n);
    }
}

static void dma_escrito(int id, int n) {
    uint32_t cnt = reg32(&hw[id], 0x040000B8 + 12 * n);
    if (!(cnt & 0x80000000)) return;
    int t = id ? (cnt >> 28) & 3 : (cnt >> 27) & 7;
    // inmediata, y la de la FIFO 3D (que de momento se traga todo)
    if (t == 0 || (!id && t == 7)) dma_ejecuta(id, n);
}

// ---- guion de botones para pruebas (MKDS_GUION="300:A 420:START 500:TOQUE(128,96)") ----
// Cada paso pulsa durante 8 fotogramas. Botones: A B X Y L R START SELECT ARRIBA ABAJO IZQ DER
extern volatile uint16_t g_guion_in, g_guion_xy;  // ventana.c

static void guion(uint64_t frame) {
    static const char *g;
    static int mirado;
    static uint64_t suelta = 0;
    static int bit_suelta = -1, xy_suelta = -1, lapiz_suelta;
    static uint64_t ultimo;
    if (!mirado) { g = getenv("MKDS_GUION"); mirado = 1; }
    if (!g) return;
    if (suelta && frame >= suelta) {
        if (bit_suelta >= 0) g_guion_in |= (uint16_t)(1u << bit_suelta);
        if (xy_suelta >= 0) g_guion_xy |= (uint16_t)(1u << xy_suelta);
        if (lapiz_suelta) g_lapiz = 0;
        suelta = 0; bit_suelta = xy_suelta = -1; lapiz_suelta = 0;
    }
    static const char *nombres[] = {"A", "B", "SELECT", "START", "DER", "IZQ", "ARRIBA", "ABAJO", "R", "L"};
    const char *p = g;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        uint64_t f = strtoull(p, (char **)&p, 10);
        if (*p == ':') p++;
        char nom[32] = {0};
        int k = 0;
        while (*p && *p != ' ' && k < 31) nom[k++] = *p++;
        if (f <= ultimo || f > frame) continue;
        fprintf(stderr, "  [guion] fotograma %llu: %s\n", (unsigned long long)f, nom);
        for (int b = 0; b < 10; b++)
            if (!strcmp(nom, nombres[b])) { g_guion_in &= (uint16_t)~(1u << b); bit_suelta = b; }
        if (!strcmp(nom, "X")) { g_guion_xy &= ~1; xy_suelta = 0; }
        if (!strcmp(nom, "Y")) { g_guion_xy &= ~2; xy_suelta = 1; }
        int tx, ty;
        if (sscanf(nom, "TOQUE(%d,%d)", &tx, &ty) == 2) {
            g_lapiz_x = tx; g_lapiz_y = ty; g_lapiz = 1; lapiz_suelta = 1;
        }
        suelta = frame + 8;
    }
    ultimo = frame;
}

// ---- tiempo: VBlank, líneas y temporizadores ---------------------------------
static void mic_actualiza(uint64_t t);

static void actualiza(void) {
    uint64_t t = ahora();
    uint64_t frame = t / CICLOS_FRAME;
    mic_actualiza(t);
    extern void wifi_actualiza(uint64_t ms);
    wifi_actualiza((uint64_t)(t * 1000 / RELOJ));
    int linea = (int)((t / CICLOS_LINEA) % LINEAS);
    if ((reg32(&hw[0], 0x04000600) >> 30) & 3) irq(0, 1u << 21);  // la FIFO 3D siempre está vacía
    for (int id = 0; id < 2; id++) {
        Hw *h = &hw[id];
        // coincidencia de VCOUNT
        int objetivo = (h->dispstat >> 8) | ((h->dispstat & 0x80) << 1);
        if ((h->dispstat & 0x20) && linea_vista[id] != linea) {
            int a = linea_vista[id], b = linea;
            if ((a < objetivo && objetivo <= b) || (b < a && (objetivo > a || objetivo <= b)))
                irq(id, 4);
        }
        linea_vista[id] = linea;
        // temporizadores
        for (int n = 0; n < 4; n++) {
            if (!(h->tm[n].ctl & 0x80)) continue;
            uint64_t p = 0x10000 - h->tm[n].recarga;
            uint64_t d = tm_ticks(h, n, t) / p;
            if (d > h->tm[n].desbordes) {
                if (h->tm[n].ctl & 0x40) h->tm[n].pend += (uint32_t)(d - h->tm[n].desbordes);
                if (h->tm[n].ctl & 0x40) m_desb[id][n] += d - h->tm[n].desbordes;
                if (h->tm[n].pend > 16) { m_perd[id][n] += h->tm[n].pend - 16; h->tm[n].pend = 16; }  // sin ponerse al día eternamente
                h->tm[n].desbordes = d;
            }
            // una a una: la siguiente cuando el juego haya atendido la anterior
            if (h->tm[n].pend && !(h->if_ & (8u << n))) { irq(id, 8u << n); h->tm[n].pend--; }
        }
    }
    if (frame != frame_visto) {  // nueva VBlank: dibujar las pantallas
        frame_visto = frame;
        if (mide < 0) mide = getenv("MKDS_MIDE") ? 1 : 0;
        if (mide > 0 && frame % 60 == 0) {
            Hw *h7 = &hw[1];
            fprintf(stderr, "  [mide %llu] ARM7 tm:", (unsigned long long)frame);
            for (int n = 0; n < 4; n++)
                if (h7->tm[n].ctl & 0x80)
                    fprintf(stderr, " t%d(%04X/%04X) desb %llu perd %llu aten %llu;", n, h7->tm[n].recarga, h7->tm[n].ctl,
                            (unsigned long long)m_desb[1][n], (unsigned long long)m_perd[1][n], (unsigned long long)m_aten[1][n]);
            fprintf(stderr, " sin turno %.1f%% (máx %.2f ms) turnos 9:%llu 7:%llu  imagen %.1f%% (máx %.2f ms)\n",
                    m_hueco7 * 100.0 / (RELOJ), m_hueco7_max * 1000.0 / RELOJ, (unsigned long long)m_turnos[0],
                    (unsigned long long)m_turnos[1], m_video * 100.0 / RELOJ, m_video_max * 1000.0 / RELOJ);
            fprintf(stderr, "      tarde (>5,2 ms) ARM7: %llu (total %llu)\n", (unsigned long long)m_tarde[1],
                    (unsigned long long)m_tarde_tot[1]);
            m_tarde[1] = 0;
            fprintf(stderr, "      retraso IRQ ARM7: media %.3f ms, máx %.2f ms (%llu)  ARM9: media %.3f ms, máx %.2f ms\n",
                    m_lat_n[1] ? m_lat[1] * 1000.0 / RELOJ / m_lat_n[1] : 0, m_lat_max[1] * 1000.0 / RELOJ,
                    (unsigned long long)m_lat_n[1], m_lat_n[0] ? m_lat[0] * 1000.0 / RELOJ / m_lat_n[0] : 0,
                    m_lat_max[0] * 1000.0 / RELOJ);
            {
                extern volatile double g_render_ms_suma, g_render_ms_max;
                extern volatile int g_render_n, g_render_sub;
                fprintf(stderr, "      pintar escena 3D: media %.2f ms, máx %.2f ms (%d escenas, %.1f intermedias cada una)\n",
                        g_render_n ? g_render_ms_suma / g_render_n : 0.0, g_render_ms_max, g_render_n,
                        g_render_n ? (double)g_render_sub / g_render_n : 0.0);
                g_render_ms_suma = g_render_ms_max = 0; g_render_n = 0; g_render_sub = 0;
            }
            memset(m_lat, 0, sizeof m_lat); memset(m_lat_max, 0, sizeof m_lat_max); memset(m_lat_n, 0, sizeof m_lat_n);
            m_video = m_video_max = 0;
            memset(m_desb, 0, sizeof m_desb); memset(m_perd, 0, sizeof m_perd); memset(m_aten, 0, sizeof m_aten);
            m_hueco7 = m_hueco7_max = 0; m_turnos[0] = m_turnos[1] = 0;
        }
        static uint32_t arriba[256 * 192], abajo[256 * 192];
        gx_vblank(hw[0].io);  // si el juego pidió SWAP_BUFFERS, se pinta su escena 3D
        {  // contador de FPS (pantalla y escenas 3D terminadas por el juego), cada segundo
            static uint64_t t_fps, frames_fps, escenas_antes;
            frames_fps++;
            if (t >= t_fps + (uint64_t)RELOJ) {
                extern void ventana_titulo_fps(int fps, int fps3d);
                ventana_titulo_fps((int)frames_fps, (int)(gx_frames() - escenas_antes));
                escenas_antes = gx_frames();
                frames_fps = 0;
                t_fps = t;
            }
        }
        {  // MKDS_REGS="desde,hasta": registros de vídeo de cada fotograma (para la captura de pantalla)
            static int r0 = -2, r1;
            if (r0 == -2) {
                const char *e = getenv("MKDS_REGS");
                r0 = -1;
                if (e) sscanf(e, "%d,%d", &r0, &r1);
            }
            if (r0 >= 0 && (int)frame >= r0 && (int)frame <= r1) {
                Hw *a = &hw[0];
                fprintf(stderr, "  [vid %d] DISPCNT A %08X B %08X POW %04X CAP %08X VRAM %02X %02X %02X %02X"
                        " B: BG2 %04X BG3 %04X BLD %04X/%04X BRILLO A %04X B %04X 3D %d\n", (int)frame,
                        reg32(a, 0x04000000), reg32(a, 0x04001000), (uint16_t)reg32(a, 0x04000304),
                        reg32(a, 0x04000064), a->io[0x240], a->io[0x241], a->io[0x242], a->io[0x243],
                        (uint16_t)reg32(a, 0x0400100C), (uint16_t)reg32(a, 0x0400100E),
                        (uint16_t)reg32(a, 0x04001050), (uint16_t)reg32(a, 0x04001052),
                        (uint16_t)reg32(a, 0x0400006C), (uint16_t)reg32(a, 0x0400106C), (int)gx_frames());
                fprintf(stderr, "        A: BG0 %04X HOFS %04X BG1 %04X BG2 %04X BG3 %04X BLD %04X/%04X/%04X 3DCNT %04X\n",
                        (uint16_t)reg32(a, 0x04000008), (uint16_t)reg32(a, 0x04000010), (uint16_t)reg32(a, 0x0400000A),
                        (uint16_t)reg32(a, 0x0400000C), (uint16_t)reg32(a, 0x0400000E), (uint16_t)reg32(a, 0x04000050),
                        (uint16_t)reg32(a, 0x04000052), (uint16_t)reg32(a, 0x04000054), (uint16_t)reg32(a, 0x04000060));
            }
        }
        uint64_t t_video = ahora();
        // La imagen se monta en otro hilo a partir de una foto de la VRAM (video_dibuja_aparte): montarla aquí
        // tardaba hasta 6 ms con el 3D a 4x y el ARM7 se saltaba pasos del sonido (música lenta). Con la
        // captura de pantalla encendida se monta aquí mismo: el juego lee lo capturado en el fotograma siguiente.
        static int sincrono = -1;  // MKDS_VIDEO_SINCRONO=1: siempre aquí (como antes)
        if (sincrono < 0) sincrono = getenv("MKDS_VIDEO_SINCRONO") != NULL;
        if ((hw[0].io[0x67] & 0x80) || sincrono) {
            video_espera_montaje();
            video_dibuja(hw[0].io, arriba, abajo);
            hw[0].io[0x67] &= 0x7F;  // la captura (DISPCAPCNT) dura un fotograma y se apaga sola
            ventana_muestra(arriba, abajo);
        } else {
            video_dibuja_aparte(hw[0].io);
        }
        { uint64_t dv = ahora() - t_video; m_video += dv; if (dv > m_video_max) m_video_max = dv; }
        guion(frame);
        extern void nombre_por_frame(void);
        nombre_por_frame();  // el nombre del launcher en la partida (main_mkds.c)
        ventana_mensajes();
        static const char *captura;
        static int mirado;
        if (!mirado) { captura = getenv("MKDS_CAPTURA"); mirado = 1; }
        static int cada = 0;  // MKDS_CAPTURA_CADA=n: cada n fotogramas (por defecto 60, una por segundo)
        if (!cada) cada = getenv("MKDS_CAPTURA_CADA") ? atoi(getenv("MKDS_CAPTURA_CADA")) : 60;
        static int desde = -1;  // MKDS_CAPTURA_DESDE=f: no guardar antes del fotograma f
        if (desde < 0) desde = getenv("MKDS_CAPTURA_DESDE") ? atoi(getenv("MKDS_CAPTURA_DESDE")) : 0;
        if (captura && frame % (uint64_t)cada == 0 && (int)frame >= desde) {  // con %d en el nombre
            extern void ventana_guarda_bmp(const char *ruta);
            char nombre[512];
            snprintf(nombre, sizeof nombre, captura, (int)frame);
            ventana_guarda_bmp(nombre);
        }
        for (int id = 0; id < 2; id++) {
            if (hw[id].dispstat & 8) irq(id, 1);
            dma_momento(id, 1);
        }
    }
}

// ---- interrupciones ----------------------------------------------------------
// Lo que hace OS_IrqHandler_ThreadSwitch del SDK antes de cambiar de hilo:
// despertar los hilos que esperaban una interrupción.
static void despierta_hilos(Cpu *c) {
    SoConfig *s = &g_so[c->id];
    if (!s->cola_irq) return;
    uint32_t t = brd32(c, s->cola_irq);
    if (!t) return;
    while (t) {
        bwr32(c, t + s->off_estado, 1);
        bwr32(c, t + s->off_enlace, 0);
        bwr32(c, t + s->off_enlace + 4, 0);
        uint32_t sig = brd32(c, t + s->off_sig);
        bwr32(c, t + s->off_sig, 0);
        t = sig;
    }
    bwr32(c, s->cola_irq, 0);
    bwr32(c, s->cola_irq + 4, 0);
    bwr16(c, s->flag_resched, 1);
}

static int irq_lista(int id) { return (hw[id].ime & 1) && (hw[id].ie & hw[id].if_); }

// Llama al manejador de interrupciones del juego como lo haría la BIOS.
// `forzar`: la BIOS (IntrWait) las acepta aunque el CPSR las tenga desactivadas.
static void entrega(Cpu *c, int forzar) {
    if (!(hw[c->id].ie & hw[c->id].if_)) return;
    if (!forzar && (!(hw[c->id].ime & 1) || (c->ctl & 0x80))) return;
    static int sin_irq = -1;  // MKDS_TRAZA=2: solo la FIFO, sin las interrupciones
    if (sin_irq < 0) sin_irq = getenv("MKDS_TRAZA") && atoi(getenv("MKDS_TRAZA")) == 2;
    if (g_traza && !sin_irq && g_traza++ < 200000)
        fprintf(stderr, "  [IRQ %s] IE&IF = %08X\n", c->id ? "ARM7" : "ARM9", hw[c->id].ie & hw[c->id].if_);
    if (mide > 0 && m_irq_desde[c->id]) {
        uint64_t l = ahora() - m_irq_desde[c->id];
        m_lat[c->id] += l; m_lat_n[c->id]++;
        if (l > m_lat_max[c->id]) m_lat_max[c->id] = l;
        if (l > (uint64_t)(RELOJ * 0.0052)) { m_tarde[c->id]++; m_tarde_tot[c->id]++; }  // más que un paso del sonido
    }
    m_irq_desde[c->id] = 0;
    if (mide > 0)
        for (int n = 0; n < 4; n++)
            if (hw[c->id].ie & hw[c->id].if_ & (8u << n)) m_aten[c->id][n]++;
    Cpu antes = *c;
    uint32_t cpsr = cpu_get_cpsr(c);
    cpu_set_cpsr(c, (cpsr & ~0x3Fu) | 0x92);  // modo IRQ, interrupciones apagadas, ARM
    c->spsr = cpsr;
    c->r[13] -= 24;  // la BIOS guarda r0-r3, r12, lr en la pila IRQ
    c->r[14] = 0xFFFF0000;
    uint32_t vector = c->id ? rd32_7(0x0380FFFC) : rd32_9(g_dtcm_base + 0x3FFC);
    cpu_llama(c, vector);
    despierta_hilos(c);
    uint32_t func = c->func;
    *c = antes;
    c->func = func;
    // cambio de hilo pendiente: se hace ya fuera de la interrupción, pero solo al volver al hilo: si esta
    // interrupción llegó dentro de otra (el wifi del Multijugador vuelve a abrirlas en sus avisos), se cambiaría
    // de hilo a mitad de la de fuera y su pila IRQ quedaría pisada; ya lo hará la de fuera al terminar
    SoConfig *s = &g_so[c->id];
    int anidada = (cpu_get_cpsr(c) & 0x1F) == 0x12;
    if (anidada && getenv("MKDS_MIDE")) {
        static int vistas;
        if (vistas++ < 5) fprintf(stderr, "[mkds] interrupción dentro de otra (%s)\n", c->id ? "ARM7" : "ARM9");
    }
    if (!anidada && s->flag_resched && brd16(c, s->flag_resched)) {
        bwr16(c, s->flag_resched, 0);
        Cpu guarda = *c;
        cpu_llama(c, s->reschedule);
        func = c->func;
        *c = guarda;
        c->func = func;
    }
}

// ---- turnos entre procesadores -------------------------------------------------
static void cambia(Cpu *c) {
    Hw *yo = &hw[c->id], *otro = &hw[!c->id];
    yo->fibra = GetCurrentFiber();
    if (mide > 0) {  // cuánto tiempo (real) pasa el ARM7 sin turno
        uint64_t t = ahora();
        if (c->id == 1) m_vuelta7 = t;  // el ARM7 deja el turno
        else if (m_vuelta7) {
            uint64_t h = t - m_vuelta7;
            m_hueco7 += h;
            if (h > m_hueco7_max) m_hueco7_max = h;
        }
        m_turnos[c->id]++;
    }
    SwitchToFiber(otro->fibra);
}

static int otro_puede(Cpu *c) {
    Hw *o = &hw[!c->id];
    return o->fibra && (!o->parado || (o->ie & o->if_));
}

void cpu_tick(Cpu *c) {
    static int turno = 0;  // MKDS_TURNO para probar; con 20000 el ARM7 esperaba hasta 7 ms con el sonido pendiente
    if (!turno) turno = getenv("MKDS_TURNO") ? atoi(getenv("MKDS_TURNO")) : TURNO;
    c->ciclos = turno;
    actualiza();
    if (irq_lista(c->id)) entrega(c, 0);
    if (otro_puede(c)) cambia(c);
}

// Para los volcados de depuración
void ds_info(int id) {
    Hw *h = &hw[id];
    fprintf(stderr, "    IME %u  IE %08X  IF %08X  %s  DISPSTAT %04X  FIFO entrada %d\n", h->ime, h->ie,
            h->if_, h->parado ? "PARADO esperando IRQ" : "corriendo", h->dispstat, h->n);
    if (h->n) {
        fprintf(stderr, "    FIFO:");
        for (int i = 0; i < h->n; i++) fprintf(stderr, " %08X", h->cola[i]);
        fprintf(stderr, "\n");
    }
    if (id == 0) {
        fprintf(stderr, "    DISPCNT A %08X  B %08X  POWCNT1 %04X  VRAMCNT", reg32(h, 0x04000000),
                reg32(h, 0x04001000), (uint16_t)reg32(h, 0x04000304));
        for (int i = 0; i < 10; i++)
            if (i != 7) fprintf(stderr, " %02X", h->io[0x240 + i]);
        fprintf(stderr, "\n    leído de la ROM: %llu KB   escrituras al motor 3D: %llu   frames: %llu\n",
                (unsigned long long)cont_tarjeta / 1024, (unsigned long long)cont_3d,
                (unsigned long long)frame_visto);
        fprintf(stderr, "    3D: %d polígonos en la última escena, %llu escenas pintadas, DISP3DCNT %04X"
                "  CLEAR_COLOR %08X  CAPCNT %08X\n", gx_polis(), (unsigned long long)gx_frames(),
                (uint16_t)reg32(h, 0x04000060), reg32(h, 0x04000350), reg32(h, 0x04000064));
        fprintf(stderr, "    ventanas A: WIN0H %04X WIN1H %04X WIN0V %04X WIN1V %04X WININ %04X WINOUT %04X"
                "  BG0CNT %04X BLDCNT %04X\n", (uint16_t)reg32(h, 0x04000040), (uint16_t)reg32(h, 0x04000042),
                (uint16_t)reg32(h, 0x04000044), (uint16_t)reg32(h, 0x04000046), (uint16_t)reg32(h, 0x04000048),
                (uint16_t)reg32(h, 0x0400004A), (uint16_t)reg32(h, 0x04000008), (uint16_t)reg32(h, 0x04000050));
        extern void gx_depura(void), vram_depura(void);
        vram_depura();
        gx_depura();
    }
}

void ds_cede(Cpu *c) {
    actualiza();
    if (otro_puede(c)) cambia(c);
}

static void espera_evento(Cpu *c) {
    Hw *h = &hw[c->id];
    h->parado = 1;
    for (;;) {
        actualiza();
        if (h->ie & h->if_) break;
        if (otro_puede(c)) { cambia(c); continue; }
        // los dos parados: dormir un poco (1 ms) hasta el siguiente evento
        Sleep(1);
    }
    h->parado = 0;
}

void ds_espera(Cpu *c, int swi) {
    g_actividad++;
    if (swi == 4 || swi == 5) {  // IntrWait / VBlankIntrWait
        uint32_t chk = c->id ? 0x0380FFF8 : g_dtcm_base + 0x3FF8;
        uint32_t mask = swi == 5 ? 1 : c->r[1];
        int descarta = swi == 5 ? 1 : (int)c->r[0];
        hw[c->id].ime = 1;
        if (descarta) bwr32(c, chk, brd32(c, chk) & ~mask);
        for (;;) {
            uint32_t f = brd32(c, chk);
            if (f & mask) { bwr32(c, chk, f & ~mask); return; }
            espera_evento(c);
            entrega(c, 1);
        }
    }
    espera_evento(c);  // Halt
    entrega(c, 0);
}

// ---- micrófono (lo hace el port en vez del ARM7) ------------------------------------------
// Mientras el juego graba, se escriben las muestras del micrófono del PC en su búfer al ritmo que pidió y
// se deja en 0x027FFF90 la dirección de la última (ahí la busca el juego); al llenarse el búfer se avisa al
// ARM9 (orden 0x51) y, si es en bucle, se vuelve al principio.
void mic_pc_abre(void);
void mic_pc_cierra(void);
int16_t mic_pc_muestra(void);
static struct { int activo, tipo, bucle; uint32_t buf, tam, ritmo, pos; uint64_t inicio, hechas; } mic;

static void mic_a_arm9(uint32_t v) {  // un mensaje del "ARM7" al ARM9 por la FIFO
    Hw *h = &hw[0];
    if (h->n >= 16) return;
    h->cola[h->n++] = v;
    if (h->n == 1 && (h->fifocnt & 0x400)) irq(0, 1u << 18);
}
void fifo_a_arm9(uint32_t v) { mic_a_arm9(v); }  // para wifi.c

static void mic_empieza(uint32_t buf, uint32_t tam, uint32_t ritmo, int tipo, int bucle) {
    if (ritmo < 100 || !tam || (buf & 0xFF000000) != 0x02000000) return;
    mic.buf = buf; mic.tam = tam; mic.ritmo = ritmo; mic.tipo = tipo; mic.bucle = bucle;
    mic.pos = 0; mic.inicio = ahora(); mic.hechas = 0; mic.activo = 1;
    fprintf(stderr, "[mkds] micrófono: búfer %08X, %u bytes, %u muestras/s, tipo %d%s\n", buf, tam,
            (unsigned)(RELOJ / ritmo), tipo, bucle ? ", en bucle" : "");
    mic_pc_abre();
}

static void mic_para(void) {
    mic.activo = 0;
    mic_pc_cierra();
}

static void mic_actualiza(uint64_t t) {
    if (!mic.activo) return;
    static int log = -1;
    if (log < 0) log = getenv("MKDS_MICRO_LOG") != NULL;
    static uint64_t visto;
    if (log && t / CICLOS_FRAME / 30 != visto) {  // lo que saca el análisis del juego (0201EC30) cada medio segundo
        visto = t / CICLOS_FRAME / 30;
        uint32_t v[7];  // pico, cruces, saturadas, periodo más largo, fuerza, periodos por tramos, modo
        static const uint32_t dir[7] = {0x0217328C, 0x0217329C, 0x021732A4, 0x021732A8, 0x02173288, 0x021732A0, 0x02161AD8};
        for (int i = 0; i < 7; i++) memcpy(&v[i], g_main + (dir[i] & 0x3FFFFF), 4);
        fprintf(stderr, "[mkds] micrófono: fotograma %llu: pico %d, cruces %d, saturadas %d, periodo %d, "
                "tramos %d/%d/%d/%d, modo %d, fuerza %d\n", (unsigned long long)(t / CICLOS_FRAME), (int)v[0],
                (int)v[1], (int)v[2], (int)v[3], v[5] & 0xFF, (v[5] >> 8) & 0xFF, (v[5] >> 16) & 0xFF, v[5] >> 24,
                (int)(v[6] & 0xFF), (int)(v[4] & 0xFF));
    }
    uint64_t toca = (t - mic.inicio) / mic.ritmo;
    uint64_t n = toca - mic.hechas;
    if (n > mic.tam) n = mic.tam;  // tras un parón, no más de un búfer
    mic.hechas = toca;
    int dos = mic.tipo & 1, con_signo = (mic.tipo & 2) != 0;  // 12 bits (en 16) u 8; con signo o sin él
    for (uint64_t k = 0; k < n && mic.activo; k++) {
        int16_t s = mic_pc_muestra();
        uint32_t a = (mic.buf + mic.pos) & 0x3FFFFF;
        if (dos) {
            uint16_t v = (uint16_t)(con_signo ? (uint16_t)s & 0xFFF0 : ((uint16_t)(s + 32768)) & 0xFFF0);
            memcpy(g_main + a, &v, 2);
        } else {
            int b = s >> 8;
            if (b < -127) b = -127;  // el micrófono de la DS no da -128: el juego exige un pico de 127 justo
            g_main[a] = con_signo ? (uint8_t)b : (uint8_t)(b + 128);
        }
        uint32_t ultima = mic.buf + mic.pos;
        memcpy(g_main + (0x027FFF90 & 0x3FFFFF), &ultima, 4);
        mic.pos += dos ? 2 : 1;
        if (mic.pos >= mic.tam) {
            mic_a_arm9((0x3000000u | (0xD1u << 8)) << 6 | 9);  // búfer lleno
            if (mic.bucle) mic.pos = 0;
            else mic_para();
        }
    }
}

// ---- IPC: IPCSYNC y las dos FIFOs --------------------------------------------------

static uint32_t ipc_rd(int id, uint32_t a) {
    Hw *h = &hw[id], *o = &hw[!id];
    // quien mira el IPC suele estar esperando al otro procesador: que le ceda el turno ya
    if (cpu_de(id)->ciclos > 200) cpu_de(id)->ciclos = 200;
    switch (a) {
    case 0x04000180:
        return (h->sync & 0x6F00) | ((o->sync >> 8) & 15);
    case 0x04000184:
        return (h->fifocnt & 0x8404) | (o->n == 0 ? 1 : 0) | (o->n == 16 ? 2 : 0) |
               (h->n == 0 ? 0x100 : 0) | (h->n == 16 ? 0x200 : 0);
    case 0x04100000:
        if (h->n && (h->fifocnt & 0x8000)) {
            h->ultimo = h->cola[0];
            memmove(h->cola, h->cola + 1, --h->n * 4);
            if (h->n == 0 && (o->fifocnt & 4)) irq(!id, 1u << 17);  // al otro: envío vacío
        }
        return h->ultimo;
    }
    return 0;
}

static void ipc_wr(int id, uint32_t a, uint32_t v) {
    Hw *h = &hw[id], *o = &hw[!id];
    if (cpu_de(id)->ciclos > 200) cpu_de(id)->ciclos = 200;  // el otro tiene que reaccionar: cederle el turno pronto
    switch (a) {
    case 0x04000180:
        h->sync = (uint16_t)(v & 0x4F00);
        if ((v & 0x2000) && (o->sync & 0x4000)) irq(!id, 1u << 16);
        break;
    case 0x04000184:
        if (v & 8) o->n = 0;  // vaciar lo enviado
        if ((v & 4) && !(h->fifocnt & 4) && o->n == 0) irq(id, 1u << 17);
        if ((v & 0x400) && !(h->fifocnt & 0x400) && h->n) irq(id, 1u << 18);
        h->fifocnt = (uint16_t)((h->fifocnt & ~0x8404u) | (v & 0x8404));
        break;
    case 0x04000188:
        if (!(h->fifocnt & 0x8000) || o->n == 16) break;
        if (id == 0 && (v & 31) == 9 && !getenv("MKDS_MIC_ARM7")) {  // servicio 9: micrófono
            // Las órdenes del micrófono las contesta el port ("hecho") sin pasarlas al ARM7: el ARM7
            // contestaba "ocupado" al pararlo y la Batalla de globos se quedaba colgada. Sin micrófono del
            // PC todavía, soplar no hace nada. Paquete: bit 25 = primera palabra (orden en bits 8-15),
            // bit 24 = última; respuesta = (3 << 24) | ((orden | 0x80) << 8) | resultado (0 = bien).
            // Grabar (orden 0x41): primera palabra = tipo de muestra | bucle (0x10); luego palabras
            // (índice << 16) | 16 bits: 1-2 búfer, 3-4 tamaño, 5-6 ritmo (ciclos por muestra). Parar: 0x42.
            // La grabación la hace mic_actualiza con el micrófono del PC (microfono.c).
            static uint32_t orden_mic, param_mic[8];
            uint32_t d = v >> 6;
            if (d & 0x2000000) {
                orden_mic = (d >> 8) & 0xFF;
                memset(param_mic, 0, sizeof param_mic);
                param_mic[0] = d & 0xFF;
            } else {
                param_mic[(d >> 16) & 7] = d & 0xFFFF;
            }
            if (d & 0x1000000) {
                param_mic[(d >> 16) & 7] = d & 0xFFFF;
                if (orden_mic == 0x41)
                    mic_empieza(param_mic[1] << 16 | param_mic[2], param_mic[3] << 16 | param_mic[4],
                                param_mic[5] << 16 | param_mic[6], (int)(param_mic[0] & 0x0F), (param_mic[0] & 0x10) != 0);
                else if (orden_mic == 0x42)
                    mic_para();
                if (getenv("MKDS_MIDE") || getenv("MKDS_MICRO_LOG"))
                    fprintf(stderr, "[mkds] micrófono: orden %02X en el fotograma %llu\n", orden_mic,
                            (unsigned long long)(ahora() / CICLOS_FRAME));
                mic_a_arm9((0x3000000u | ((orden_mic | 0x80) << 8)) << 6 | 9);
            }
            break;
        }
        // servicio 10: el wifi (WM) lo contesta el port (wifi.c); MKDS_WIFI_ARM7=1 deja al ARM7 de antes
        static int wifi_arm7 = -1;
        if (wifi_arm7 < 0) wifi_arm7 = getenv("MKDS_WIFI_ARM7") != NULL;
        extern int wifi_orden(uint32_t dir);
        if (id == 0 && (v & 31) == 10 && !wifi_arm7 && wifi_orden(v >> 6)) break;
        o->cola[o->n++] = v;
        if (g_traza && g_traza++ < 200000)
            fprintf(stderr, "  [FIFO %s] servicio %2u datos %07X\n", id ? "ARM7->ARM9" : "ARM9->ARM7",
                    v & 31, v >> 6);
        if (o->n == 1 && (o->fifocnt & 0x400)) irq(!id, 1u << 18);
        break;
    }
}

// ---- divisor y raíz cuadrada del ARM9 ------------------------------------------
static void divisor(Hw *h) {
    uint16_t cnt = (uint16_t)reg32(h, 0x04000280);
    int64_t num, den;
    int64_t n64, d64;
    memcpy(&n64, h->io + 0x290, 8);
    memcpy(&d64, h->io + 0x298, 8);
    switch (cnt & 3) {
    case 0: num = (int32_t)n64; den = (int32_t)d64; break;
    case 1: num = n64; den = (int32_t)d64; break;
    default: num = n64; den = d64; break;
    }
    int64_t q, r;
    if (den == 0) { q = num < 0 ? 1 : -1; r = num; }
    else if (num == INT64_MIN && den == -1) { q = num; r = 0; }
    else { q = num / den; r = num % den; }
    memcpy(h->io + 0x2A0, &q, 8);
    memcpy(h->io + 0x2A8, &r, 8);
    cnt = (uint16_t)((cnt & 3) | (d64 == 0 ? 0x4000 : 0));
    memcpy(h->io + 0x280, &cnt, 2);
}

static void raiz(Hw *h) {
    uint16_t cnt = (uint16_t)reg32(h, 0x040002B0);
    uint64_t v;
    memcpy(&v, h->io + 0x2B8, 8);
    if (!(cnt & 1)) v &= 0xFFFFFFFF;
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    pon_reg32(h, 0x040002B4, (uint32_t)r);
    cnt &= 1;
    memcpy(h->io + 0x2B0, &cnt, 2);
}

// ---- tarjeta (slot 1) ----------------------------------------------------------
static const uint8_t *rom;
static size_t rom_tam;
static struct {
    uint32_t ctrl;
    uint8_t cmd[8];
    uint32_t pos, total, dir, modo;
    uint16_t spicnt;
} tarjeta;

void ds_pon_rom(const uint8_t *r, size_t n) { rom = r; rom_tam = n; }

static void tarjeta_empieza(int id, uint32_t v) {
    tarjeta.ctrl = v;
    if (!(v & 0x80000000)) return;
    int bs = (v >> 24) & 7;
    tarjeta.total = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    tarjeta.pos = 0;
    tarjeta.modo = tarjeta.cmd[0];
    tarjeta.dir = (uint32_t)tarjeta.cmd[1] << 24 | tarjeta.cmd[2] << 16 | tarjeta.cmd[3] << 8 | tarjeta.cmd[4];
    if (tarjeta.modo == 0xB7 && tarjeta.dir < 0x8000) tarjeta.dir = 0x8000 + (tarjeta.dir & 0x1FF);
    if (tarjeta.total == 0) {
        tarjeta.ctrl &= ~0x80000000u;
        if (tarjeta.spicnt & 0x4000) irq(id, 1u << 19);
        return;
    }
    tarjeta.ctrl |= 0x00800000;  // hay datos
    dma_momento(id, id ? 2 : 5);  // DMA esperando a la tarjeta
}

static uint32_t tarjeta_dato(int id) {
    if (!(tarjeta.ctrl & 0x00800000)) return 0;
    uint32_t v = 0xFFFFFFFF;
    if (tarjeta.modo == 0xB7) {
        cont_tarjeta += 4;
        uint32_t a = tarjeta.dir + tarjeta.pos;
        if (rom && a + 4 <= rom_tam) memcpy(&v, rom + a, 4);
    } else if (tarjeta.modo == 0xB8) {
        v = 0x00001FC2;  // ID del chip (32 MB)
    }
    tarjeta.pos += 4;
    if (tarjeta.pos >= tarjeta.total) {
        tarjeta.ctrl &= ~0x80800000u;
        if (tarjeta.spicnt & 0x4000) irq(id, 1u << 19);
    }
    return v;
}

// ---- memoria de guardado del cartucho (bus SPI auxiliar, 0x040001A0/A2) --------------
// EEPROM o flash serie. Se guarda en partida.sav junto al ejecutable.
static uint8_t partida[0x80000];
static uint32_t partida_tam = 0x40000;  // MKDS: flash serie de 256 KB
static int partida_flash = 1;           // 1 = flash (direcciones de 3 bytes)
static struct { int pos; uint8_t cmd, dato, wel; uint32_t dir; int escrito; } sav;
static int sav_ordenes_vistas;

static void partida_carga(void) {
    memset(partida, 0xFF, sizeof partida);
    FILE *f = fopen("partida.sav", "rb");
    if (f) { fread(partida, 1, sizeof partida, f); fclose(f); }
}

static void partida_guarda(void) {
    FILE *f = fopen("partida.sav", "wb");
    if (f) { fwrite(partida, 1, partida_tam, f); fclose(f); }
}

static uint8_t sav_byte(uint8_t b) {
    int pos = sav.pos++;
    if (pos == 0) {
        sav.cmd = b; sav.dir = 0;
        if (b == 0x06) sav.wel = 1;
        if (b == 0x04) sav.wel = 0;
        if (b == 0x0A || b == 0xDB || b == 0xD8 || b == 0x9F) partida_flash = 1;
        if (g_traza && sav_ordenes_vistas++ < 40) fprintf(stderr, "  [guardado] orden %02X\n", b);
        return 0xFF;
    }
    int ad = partida_flash ? 3 : 2;  // bytes de dirección
    switch (sav.cmd) {
    case 0x05: return (uint8_t)(sav.wel << 1);  // estado: libre; bit 1 = escritura permitida
    case 0x03: case 0x0B:  // leer
        if (pos <= ad) { sav.dir = (sav.dir << 8) | b; return 0xFF; }
        if (sav.cmd == 0x0B && pos == ad + 1) return 0xFF;
        return partida[sav.dir++ % partida_tam];
    case 0x02: case 0x0A:  // escribir
        if (pos <= ad) { sav.dir = (sav.dir << 8) | b; return 0xFF; }
        if (sav.wel) { partida[sav.dir % partida_tam] = b; sav.escrito = 1; }
        sav.dir++;
        return 0xFF;
    case 0x9F: return pos == 1 ? 0x20 : pos == 2 ? 0x40 : 0x12;  // ID (flash)
    }
    return 0xFF;
}

static void sav_dato(uint8_t v) {
    if (!(tarjeta.spicnt & 0x8000) || !(tarjeta.spicnt & 0x2000)) return;  // bit 13 = 1: modo serie (guardado)
    sav.dato = sav_byte(v);
    if (!(tarjeta.spicnt & 0x40)) {  // fin de la orden (sin mantener la selección)
        if (sav.escrito) { partida_guarda(); sav.escrito = 0; sav.wel = 0; }
        sav.pos = 0;
    }
}

// ---- SPI del ARM7: gestor de energía, firmware y pantalla táctil -----------------
static uint8_t firmware[0x40000];
static struct { uint16_t cnt; uint8_t dato; int pos; uint8_t cmd; uint32_t dir; uint8_t wel; } spi;
static uint8_t pm_regs[8] = {0x0C, 0x00, 0x01, 0x00, 0x01};
static uint8_t tsc_sal[2];
static int tsc_n;

static uint8_t spi_byte(uint8_t b) {
    int dev = (spi.cnt >> 8) & 3;
    int pos = spi.pos++;
    if (dev == 0) {  // gestor de energía: índice (bit 7 = leer) y luego dato
        if (pos == 0) { spi.cmd = b; return 0; }
        if (spi.cmd & 0x80) return pm_regs[spi.cmd & 7];
        pm_regs[spi.cmd & 7] = b;
        return 0;
    }
    if (dev == 1) {  // memoria flash del firmware (ST M45PE20)
        if (pos == 0) {
            spi.cmd = b; spi.dir = 0;
            if (b == 0x06) spi.wel = 1;       // WREN: permite escribir
            if (b == 0x04) spi.wel = 0;       // WRDI
            return 0;
        }
        switch (spi.cmd) {
        case 0x03: case 0x0B:
            if (pos <= 3) { spi.dir = (spi.dir << 8) | b; return 0; }
            if (spi.cmd == 0x0B && pos == 4) return 0;
            if (g_traza && (pos == 4 || (spi.cmd == 0x0B && pos == 5)))
                fprintf(stderr, "  [firmware] lectura en %05X\n", spi.dir & 0x3FFFF);
            return firmware[spi.dir++ & 0x3FFFF];
        case 0x05: return spi.wel << 1;  // estado: libre, bit 1 = escritura permitida
        case 0x9F: return pos == 1 ? 0x20 : pos == 2 ? 0x40 : 0x12;
        case 0x0A: case 0x02:  // escribir página
            if (pos <= 3) { spi.dir = (spi.dir << 8) | b; return 0; }
            if (spi.wel) firmware[spi.dir & 0x3FF00 | (spi.dir & 0xFF)] = b;
            spi.dir = (spi.dir & ~0xFFu) | ((spi.dir + 1) & 0xFF);
            return 0;
        case 0xDB: case 0xD8:  // borrar página / sector
            if (pos <= 3) { spi.dir = (spi.dir << 8) | b; }
            if (pos == 3 && spi.wel)
                memset(firmware + (spi.dir & (spi.cmd == 0xDB ? 0x3FF00 : 0x30000)), 0xFF,
                       spi.cmd == 0xDB ? 0x100 : 0x10000);
            return 0;
        }
        return 0xFF;
    }
    if (dev == 2) {  // pantalla táctil (TSC2046): sin tocar, todo a 0
        uint8_t r = tsc_n ? tsc_sal[0] : 0;
        if (tsc_n) { tsc_sal[0] = tsc_sal[1]; tsc_n--; }
        if (b & 0x80) {
            // canal 5 = X, 1 = Y (la calibración del firmware es píxel * 16), 3/4 = presión
            int canal = (b >> 4) & 7;
            uint16_t valor = 0;
            if (g_lapiz) {
                if (canal == 5) valor = (uint16_t)(g_lapiz_x * 16);
                else if (canal == 1) valor = (uint16_t)(g_lapiz_y * 16);
                else if (canal == 3) valor = 0x200;
                else if (canal == 4) valor = 0xE00;
            }
            tsc_sal[0] = (uint8_t)(valor >> 5);
            tsc_sal[1] = (uint8_t)(valor << 3);
            tsc_n = 2;
        }
        return r;
    }
    return 0xFF;
}

// CRC16 del firmware (el mismo que la BIOS)
// CRC-16 estándar (0xA001), el mismo resultado que la BIOS (SWI 0x0E)
static uint16_t crc16(uint16_t crc, const uint8_t *p, size_t n) {
    uint32_t c = crc;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return (uint16_t)c;
}

// Un firmware inventado con los ajustes de la consola (nombre, idioma, táctil)
static void prepara_firmware(void) {
    memset(firmware, 0xFF, sizeof firmware);
    memset(firmware, 0, 0x200);
    firmware[0x1D] = 0x20;                 // DS Lite
    firmware[0x20] = 0xC0; firmware[0x21] = 0x7F;  // ajustes en 0x3FE00 (0x7FC0 * 8)
    uint8_t mac[6];  // distinta en cada PC y cada partida: si no, dos jugadores en el modo Local chocarían
    extern void wifi_mac(uint8_t *m);
    wifi_mac(mac);
    memcpy(firmware + 0x36, mac, 6);
    firmware[0x3C] = 0xFE; firmware[0x3D] = 0x3F;  // canales wifi 1-13
    uint8_t u[0x100];
    memset(u, 0, sizeof u);
    u[0x00] = 5;                           // versión
    u[0x02] = 11;                          // color favorito
    u[0x03] = 1; u[0x04] = 1;              // cumpleaños
    // nombre de la consola: el del jugador (NOMBRE en ajustes.ini, lo pide el launcher); el juego lo copia a una
    // partida nueva ("Durante las carreras, aparecerá ...")
    wchar_t nombre[11] = L"Jugador";
    const char *nom = getenv("MKDS_NOMBRE");
    if (nom && *nom) {
        memset(nombre, 0, sizeof nombre);
        MultiByteToWideChar(CP_UTF8, 0, nom, -1, nombre, 11);
        nombre[10] = 0;
    }
    int largo = 0;
    for (; largo < 10 && nombre[largo]; largo++) memcpy(u + 0x06 + largo * 2, &nombre[largo], 2);
    u[0x1A] = (uint8_t)largo;
    // calibración de la pantalla táctil: ADC = píxel * 16
    uint16_t cal[] = {32 * 16, 32 * 16};
    memcpy(u + 0x58, cal, 4); u[0x5C] = 32; u[0x5D] = 32;
    uint16_t cal2[] = {224 * 16, 160 * 16};
    memcpy(u + 0x5E, cal2, 4); u[0x62] = 224; u[0x63] = 160;
    // idioma de la consola (el juego europeo lo usa): 1 inglés, 2 francés, 3 alemán, 4 italiano, 5 español.
    // MKDS_IDIOMA (IDIOMA en ajustes.ini, lo elige el launcher); por defecto español
    int idioma = getenv("MKDS_IDIOMA") ? atoi(getenv("MKDS_IDIOMA")) : 5;
    if (idioma < 1 || idioma > 5) idioma = 5;
    u[0x64] = (uint8_t)(idioma | (3 << 4));  // y brillo máximo
    u[0x70] = 1;                           // contador de guardado
    uint16_t crc = crc16(0xFFFF, u, 0x70);
    memcpy(u + 0x72, &crc, 2);
    memcpy(firmware + 0x3FE00, u, 0x100);
    u[0x70] = 0;
    crc = crc16(0xFFFF, u, 0x70);
    memcpy(u + 0x72, &crc, 2);
    memcpy(firmware + 0x3FF00, u, 0x100);
    // lo que el arranque de la consola deja en RAM: ajustes y datos de la tarjeta
    memcpy(g_main + 0x3FFC80, firmware + 0x3FE00, 0x70);
    uint32_t chip = 0x00001FC2;
    memcpy(g_main + 0x3FF800, &chip, 4);
    memcpy(g_main + 0x3FF804, &chip, 4);
    memcpy(g_main + 0x3FFC00, &chip, 4);
    memcpy(g_main + 0x3FFC04, &chip, 4);
    uint16_t arranque = 1;                 // arrancado desde tarjeta
    memcpy(g_main + 0x3FFC40, &arranque, 2);
}

// ---- RTC (reloj): protocolo serie por 0x04000138 --------------------------------
static struct {
    uint8_t io, cmd, in, sal[8];
    int bit_in, pos_in, bit_sal, pos_sal;
    uint8_t estado1, estado2;
} rtc = {.estado1 = 0x02};

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void rtc_byte(uint8_t b) {
    if (rtc.pos_in == 0) {
        if ((b & 0xF0) != 0x60 && (b & 0x0F) == 0x06) {  // llegó al revés
            uint8_t r = 0;
            for (int i = 0; i < 8; i++) r |= ((b >> i) & 1) << (7 - i);
            b = r;
        }
        rtc.cmd = b;
        memset(rtc.sal, 0, sizeof rtc.sal);
        if (b & 1) {  // lectura
            time_t t = time(NULL);
            struct tm *f = localtime(&t);
            switch ((b >> 1) & 7) {
            case 0: rtc.sal[0] = rtc.estado1; break;
            case 1: rtc.sal[0] = rtc.estado2; break;
            case 2:
                rtc.sal[0] = bcd(f->tm_year % 100); rtc.sal[1] = bcd(f->tm_mon + 1);
                rtc.sal[2] = bcd(f->tm_mday); rtc.sal[3] = (uint8_t)f->tm_wday;
                rtc.sal[4] = bcd(f->tm_hour) | (f->tm_hour >= 12 ? 0x40 : 0);
                rtc.sal[5] = bcd(f->tm_min); rtc.sal[6] = bcd(f->tm_sec);
                break;
            case 3:
                rtc.sal[0] = bcd(f->tm_hour) | (f->tm_hour >= 12 ? 0x40 : 0);
                rtc.sal[1] = bcd(f->tm_min); rtc.sal[2] = bcd(f->tm_sec);
                break;
            }
        }
    } else if (!(rtc.cmd & 1)) {  // escritura
        if (((rtc.cmd >> 1) & 7) == 0) rtc.estado1 = (rtc.estado1 & 0xF0) | (b & 0x0E);
        if (((rtc.cmd >> 1) & 7) == 1) rtc.estado2 = b;
    }
    rtc.pos_in++;
}

// bit 0 dato, bit 1 reloj, bit 2 selección, bit 4 dirección del dato (1 = escribe la CPU)
static void rtc_wr(uint8_t v) {
    uint8_t antes = rtc.io;
    uint8_t dato = (v & 0x10) ? (v & 1) : (rtc.io & 1);
    if (!(v & 4) || !(antes & 4)) {  // sin seleccionar o recién seleccionado: empezar
        rtc.bit_in = rtc.pos_in = rtc.bit_sal = rtc.pos_sal = 0;
        rtc.in = 0;
    } else if (!(antes & 2) && (v & 2)) {  // flanco de subida del reloj
        if (v & 0x10) {
            rtc.in |= (uint8_t)((v & 1) << rtc.bit_in);
            if (++rtc.bit_in == 8) { rtc_byte(rtc.in); rtc.in = 0; rtc.bit_in = 0; }
        } else {
            dato = (rtc.sal[rtc.pos_sal] >> rtc.bit_sal) & 1;
            if (++rtc.bit_sal == 8) { rtc.bit_sal = 0; if (rtc.pos_sal < 7) rtc.pos_sal++; }
        }
    }
    rtc.io = (uint8_t)((v & ~1) | dato);
}

// ---- registros comunes a los dos procesadores -----------------------------------
static int registro_comun(int id, uint32_t a, uint32_t *v) {
    Hw *h = &hw[id];
    uint64_t t;
    switch (a & ~3u) {
    case 0x04000004: {  // DISPSTAT + VCOUNT
        t = ahora();
        int linea = (int)((t / CICLOS_LINEA) % LINEAS);
        int objetivo = (h->dispstat >> 8) | ((h->dispstat & 0x80) << 1);
        uint32_t ds = (h->dispstat & 0xFFB8) | (linea >= 192 && linea < 262 ? 1 : 0) |
                      ((t % CICLOS_LINEA) > 1606 ? 2 : 0) | (linea == objetivo ? 4 : 0);
        *v = ds | ((uint32_t)linea << 16);
        return 1;
    }
    case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: {
        int n = (a >> 2) & 3;
        *v = tm_valor(h, n, ahora()) | ((uint32_t)h->tm[n].ctl << 16);
        return 1;
    }
    case 0x04000130: *v = g_keyinput | (reg32(h, 0x04000130) & 0xFFFF0000); return 1;  // teclas
    case 0x04000180: case 0x04000184: case 0x04100000: *v = ipc_rd(id, a & ~3u); return 1;
    case 0x040001A4: *v = tarjeta.ctrl; return 1;
    case 0x040001A0: *v = tarjeta.spicnt | (uint32_t)sav.dato << 16; return 1;
    case 0x04100010: *v = tarjeta_dato(id); return 1;
    case 0x04000208: *v = h->ime; return 1;
    case 0x04000210: *v = h->ie; return 1;
    case 0x04000214: *v = h->if_; return 1;
    }
    return 0;
}

static int escribe_comun(int id, uint32_t a, uint32_t v, int bytes) {
    Hw *h = &hw[id];
    uint32_t a4 = a & ~3u;
    // las escrituras de 8/16 bits se combinan con lo que ya había
    uint32_t viejo = 0, w;
    if (!registro_comun(id, a4, &viejo)) viejo = reg32(h, a4);
    uint32_t desp = (a & 3) * 8, m = bytes == 4 ? 0xFFFFFFFF : ((1u << (bytes * 8)) - 1) << desp;
    w = (viejo & ~m) | ((v << desp) & m);
    switch (a4) {
    case 0x04000004:
        if (m & 0xFFFF) h->dispstat = (uint16_t)((h->dispstat & 7) | (w & 0xFFB8));
        return 1;
    case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: {
        int n = (a4 >> 2) & 3;
        if (m & 0xFFFF) h->tm[n].recarga = (uint16_t)w;
        if (m & 0xFFFF0000) tm_control(h, n, (uint16_t)(w >> 16), ahora());
        return 1;
    }
    case 0x040000B8: case 0x040000C4: case 0x040000D0: case 0x040000DC:
        pon_reg32(h, a4, w);
        if (m & 0xFFFF0000) dma_escrito(id, (a4 - 0x040000B8) / 12);
        return 1;
    case 0x04000180: case 0x04000184: case 0x04000188:
        ipc_wr(id, a4, w);
        return 1;
    case 0x040001A0:
        if (m & 0xFFFF) tarjeta.spicnt = (uint16_t)w;
        if (m & 0x00FF0000) sav_dato((uint8_t)(w >> 16));
        return 1;
    case 0x040001A4: tarjeta_empieza(id, w); return 1;
    case 0x040001A8: case 0x040001AC:
        memcpy(tarjeta.cmd + (a - 0x040001A8), &v, bytes);
        return 1;
    case 0x04000208: h->ime = w & 1; return 1;
    case 0x04000210: h->ie = w; return 1;
    case 0x04000214: h->if_ &= ~(v << desp); return 1;  // escribir 1 = ya atendida
    }
    return 0;
}

// apunta las primeras veces que se usa un registro que aún no se trata
static void apunta(int id, const char *que, uint32_t a, uint32_t v, int bytes) {
    static uint8_t visto[2][0x10000 / 2][2];
    uint32_t i = (a & 0xFFFF) / 2;
    int k = que[0] == 'e';
    if (a >= 0x04010000 || visto[id][i][k]) return;
    visto[id][i][k] = 1;
    fprintf(stderr, "  io%d %s%d %08X = %08X\n", id ? 7 : 9, que, bytes * 8, a, v);
}

static uint32_t lee_io(int id, uint32_t a, int bytes) {
    uint32_t v;
    g_actividad++;
    if (registro_comun(id, a & ~3u, &v)) return (v >> ((a & 3) * 8)) & (bytes == 4 ? ~0u : (1u << (bytes * 8)) - 1);
    v = 0;
    if (a < 0x04010000) memcpy(&v, hw[id].io + (a & 0xFFFF), bytes);
    return v;
}

// ---- ARM9 --------------------------------------------------------------------
uint32_t io9_rd(uint32_t a, int bytes) {
    Hw *h = &hw[0];
    if (a >= 0x04000600 && a < 0x040006A4) {  // estado y resultados del motor 3D
        uint32_t v = gx_lee(a & ~3u);
        if ((a & ~3u) == 0x04000600) v |= reg32(h, 0x04000600) & 0xC0000000;  // modo de IRQ de la FIFO
        return v >> ((a & 3) * 8);
    }
    uint32_t v = lee_io(0, a, bytes);
    if ((a >= 0x04000280 && a < 0x040002C0) || (a >= 0x04000000 && a < 0x04000070) ||
        (a >= 0x04001000 && a < 0x04001070) || (a >= 0x040000B0 && a < 0x04000110) ||
        (a >= 0x04000240 && a < 0x04000250))
        return v;
    (void)h;
    apunta(0, "lee", a, v, bytes);
    return v;
}

void io9_wr(uint32_t a, uint32_t v, int bytes) {
    Hw *h = &hw[0];
    g_actividad++;
    if (escribe_comun(0, a, v, bytes)) return;
    if (a < 0x04010000) memcpy(h->io + (a & 0xFFFF), &v, bytes);
    if (a >= 0x04000280 && a < 0x040002A0) { divisor(h); return; }
    if (a >= 0x040002B0 && a < 0x040002C0) { raiz(h); return; }
    if (a == 0x04000247) { g_wramcnt = (uint8_t)(v & 3); return; }
    if (a >= 0x04000240 && a < 0x0400024A) {  // VRAMCNT A..I (byte a byte)
        for (int k = 0; k < bytes; k++) {
            uint32_t r = a + k - 0x04000240;
            if (r == 7) g_wramcnt = (uint8_t)((v >> (8 * k)) & 3);
            else if (r < 10) video_vramcnt(r < 7 ? (int)r : (int)r - 1, (uint8_t)(v >> (8 * k)));
        }
        return;
    }
    if (a >= 0x04000400 && a < 0x04000600) { cont_3d++; gx_escribe(a & ~3u, v); return; }  // motor 3D
    if (a >= 0x04000600 && a < 0x04000700) return;
    if ((a >= 0x04000000 && a < 0x04000070) || (a >= 0x04001000 && a < 0x04001070) ||
        (a >= 0x04000240 && a < 0x04000250) || (a >= 0x040000B0 && a < 0x040000F0) ||
        a == 0x04000204 || a == 0x04000300 || a == 0x04000304 || a == 0x04000132)
        return;
    apunta(0, "esc", a, v, bytes);
}

// ---- ARM7 --------------------------------------------------------------------
uint32_t io7_rd(uint32_t a, int bytes) {
    Hw *h = &hw[1];
    switch (a & ~3u) {
    case 0x04000134:  // RCNT y KEYXY: X/Y sin pulsar, sin lápiz, tapa abierta
        g_actividad++;
        // bit 6 = lápiz (0 = tocando), bit 7 = tapa (0 = abierta)
        return ((uint32_t)((g_keyxy & 3) | 0x3C | (g_lapiz ? 0 : 0x40)) << 16 |
                (reg32(h, 0x04000134) & 0xFFFF)) >> ((a & 3) * 8);
    case 0x04000138: g_actividad++; return rtc.io;
    case 0x040001C0:
        g_actividad++;
        return ((a & 2) ? spi.dato : (uint32_t)(spi.cnt & ~0x80) | ((uint32_t)spi.dato << 16)) &
               (bytes == 4 ? ~0u : 0xFFFF);
    case 0x04000204: g_actividad++; return reg32(&hw[0], 0x04000204) >> ((a & 3) * 8);  // EXMEMSTAT
    case 0x04000240: g_actividad++;
        return ((uint32_t)g_wramcnt << 8 | 3) >> ((a & 3) * 8);  // VRAMSTAT/WRAMSTAT
    }
    uint32_t v = lee_io(1, a, bytes);
    if (a >= 0x04000400 && a < 0x04000520 && !sonido_activo()) {  // sin sonido: los canales sin bucle ya acabaron
        if ((a & 0xF) == 3 && ((v >> 3) & 3) != 1) v &= 0x7F;
        if ((a & 0xF) == 0 && bytes == 4 && ((v >> 27) & 3) != 1) v &= 0x7FFFFFFF;
        return v;
    }
    if ((a >= 0x040000B0 && a < 0x04000110) || a == 0x04000300 || a == 0x04000304) return v;
    apunta(1, "lee", a, v, bytes);
    return v;
}

void io7_wr(uint32_t a, uint32_t v, int bytes) {
    Hw *h = &hw[1];
    g_actividad++;
    if (escribe_comun(1, a, v, bytes)) return;
    if (a >= 0x04000400 && a < 0x04000500) {  // canal de sonido: ¿se pone en marcha?
        uint32_t o = a & 0xFFFF, base = o & ~15u, antes = reg32(h, base);
        memcpy(h->io + o, &v, bytes);
        if (!(antes & 0x80000000) && (reg32(h, base) & 0x80000000)) g_snd_gen[(base - 0x400) / 16]++;
        return;
    }
    switch (a) {
    case 0x04000138: rtc_wr((uint8_t)v); return;
    case 0x040001C0:
        spi.cnt = (uint16_t)v;
        if (bytes == 4) goto dato;
        if (!(v & 0x8000)) spi.pos = 0;
        return;
    case 0x040001C2:
    dato:
        if (bytes == 4) v >>= 16;
        if (spi.cnt & 0x8000) {
            spi.dato = spi_byte((uint8_t)v);
            if (!(spi.cnt & 0x800)) {  // fin de la orden
                if (((spi.cnt >> 8) & 3) == 1 && spi.pos > 1 &&
                    (spi.cmd == 0x0A || spi.cmd == 0x02 || spi.cmd == 0xDB || spi.cmd == 0xD8))
                    spi.wel = 0;
                spi.pos = 0;
            }
            if (spi.cnt & 0x4000) irq(1, 1u << 23);
        }
        return;
    case 0x04000301:  // HALTCNT
        if ((v & 0xC0) == 0x80 || (v & 0xC0) == 0xC0) ds_espera(&g_cpu7, 6);
        return;
    }
    if (a < 0x04010000) memcpy(h->io + (a & 0xFFFF), &v, bytes);
    if ((a >= 0x04000400 && a < 0x04000520) || (a >= 0x040000B0 && a < 0x040000E0) ||
        a == 0x04000300 || a == 0x04000304 || a == 0x04000134)
        return;
    apunta(1, "esc", a, v, bytes);
}

// ---- arranque ------------------------------------------------------------------
static uint32_t entrada7;

static void WINAPI fibra_arm7(void *p) {
    (void)p;
    fprintf(stderr, "[ds] ARM7 arrancando en %08X\n", entrada7);
    dispatch7(&g_cpu7, entrada7);
    fprintf(stderr, "[ds] el arranque del ARM7 ha vuelto (no debería)\n");
    exit(1);
}

void ds_arranca(uint32_t entrada9, uint32_t e7) {
    QueryPerformanceFrequency(&frec);
    QueryPerformanceCounter(&t0);
    timeBeginPeriod(1);  // Sleep(1) de verdad de 1 ms
    g_traza = getenv("MKDS_TRAZA") != NULL;
    prepara_firmware();
    partida_carga();
    ventana_inicia();
    gx_inicia();
    sonido_inicia();
    entrada7 = e7;
    g_cpu9.id = 0; g_cpu7.id = 1;
    g_cpu9.ctl = g_cpu7.ctl = 0x1F;
    g_cpu9.r[14] = g_cpu7.r[14] = 0xFFFFFFF0;
    g_cpu9.ciclos = g_cpu7.ciclos = TURNO;
    hw[0].fibra = ConvertThreadToFiber(NULL);
    hw[1].fibra = CreateFiberEx(64 * 1024, 64 * 1024 * 1024, 0, fibra_arm7, NULL);
    fprintf(stderr, "[ds] ARM9 arrancando en %08X\n", entrada9);
    dispatch(&g_cpu9, entrada9);
    fprintf(stderr, "[ds] el arranque del ARM9 ha vuelto (no debería)\n");
    exit(1);
}

uint8_t *ds_io7(void) { return hw[1].io; }
