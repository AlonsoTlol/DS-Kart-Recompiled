// Hilos del sistema operativo de la DS (NitroSDK) con fibras de Windows.
//
// En la DS, cambiar de hilo es guardar todos los registros en un "contexto"
// (OS_SaveContext) y cargar los de otro (OS_LoadContext), saltando a donde se
// quedó. En C no se puede saltar a la mitad de otra función, así que cada hilo
// de la DS es una fibra (una pila propia) y:
//   - OS_SaveContext: apunta qué fibra es la dueña del contexto y devuelve 0.
//   - OS_LoadContext: cambia a la fibra dueña, que estaba parada dentro de su
//     propio OS_LoadContext; restaura los registros y vuelve (ver entra()).
//     Si el contexto es de un hilo nuevo, crea su fibra y lo arranca.
// Los dos procesadores tienen sus propios hilos (g_so[c->id]).
#include <windows.h>  // antes que cpu.h: su macro R choca con windows.h
#include "ds.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint32_t ctx;      // dirección del contexto en la memoria de la DS
    void *fibra;
    int guardado;
} Hilo;

static Hilo hilos[2][64];
static int nhilos[2];
static uint32_t cargando[2];  // contexto que se está cargando en cada procesador

static Hilo *busca(Cpu *c, uint32_t ctx) {
    Hilo *t = hilos[c->id];
    for (int i = 0; i < nhilos[c->id]; i++)
        if (t[i].ctx == ctx) return &t[i];
    if (nhilos[c->id] == 64) { fprintf(stderr, "[hilos] demasiados hilos\n"); exit(8); }
    Hilo *h = &t[nhilos[c->id]++];
    memset(h, 0, sizeof *h);
    h->ctx = ctx;
    return h;
}

// OS_InitContext: el juego prepara un hilo nuevo en este contexto. Si había un
// hilo anterior con el mismo contexto (destruido), su fibra ya no vale.
void hle_init_context(Cpu *c) {
    uint32_t ctx = c->r[0];
    Hilo *t = hilos[c->id];
    for (int i = 0; i < nhilos[c->id]; i++) {
        if (t[i].ctx != ctx) continue;
        // la fibra vieja no se borra (podría ser la del hilo principal): solo se olvida
        t[i].fibra = NULL;
        t[i].guardado = 0;
    }
}

void **hle_ctx_buf(Cpu *c, uint32_t ctx) {
    (void)c; (void)ctx;
    return NULL;  // ya no se usa setjmp (ver entra())
}

static void llama(Cpu *c, uint32_t func, uint32_t r0) {
    if (!func) return;
    uint32_t lr = c->r[14];
    c->r[0] = r0;
    cpu_llama(c, func);
    c->r[14] = lr;
}

// Formato del contexto (igual que el SDK): +0 cpsr, +4 r0-r12, +38 sp, +3C lr,
// +40 pc+4, +44 sp del modo supervisor, +48 estado del divisor/raíz (solo ARM9).
void hle_save_context(Cpu *c) {
    uint32_t ctx = c->r[0];
    llama(c, g_so[c->id].cp_save, ctx + 0x48);
    uint32_t r[13];
    memcpy(r, c->r, sizeof r);
    bwr32(c, ctx, cpu_get_cpsr(c));
    for (int i = 0; i < 13; i++) bwr32(c, ctx + 4 + i * 4, i == 0 ? 1 : r[i]);
    bwr32(c, ctx + 0x38, c->r[13]);
    bwr32(c, ctx + 0x3C, c->r[14]);
    bwr32(c, ctx + 0x40, c->r[14] + 4);
    bwr32(c, ctx + 0x44, c->bank_r13[3]);
    Hilo *h = busca(c, ctx);
    h->fibra = GetCurrentFiber();
    h->guardado = 1;
    c->r[0] = 0;
}

static void restaura(Cpu *c, uint32_t ctx) {
    llama(c, g_so[c->id].cp_restore, ctx + 0x48);
    cpu_set_cpsr(c, brd32(c, ctx));
    for (int i = 0; i < 13; i++) c->r[i] = brd32(c, ctx + 4 + i * 4);
    c->r[13] = brd32(c, ctx + 0x38);
    c->r[14] = brd32(c, ctx + 0x3C);
    c->bank_r13[3] = brd32(c, ctx + 0x44);
}

// En la fibra dueña: seguir donde se guardó el contexto. En el SDK, lo que
// hay tras OS_LoadContext es lo mismo que cuando OS_SaveContext devuelve 1
// (sacar registros de la pila y volver), así que basta con restaurar los
// registros y volver de OS_LoadContext.
static void entra(Cpu *c) {
    restaura(c, cargando[c->id]);
    c->r[0] = 1;
}

typedef struct { Cpu *c; Hilo *h; } Arranque;

static void WINAPI fibra_nueva(void *p) {
    Arranque *a = p;
    Cpu *c = a->c;
    Hilo *h = a->h;
    free(a);
    restaura(c, h->ctx);
    uint32_t pc = brd32(c, h->ctx + 0x40) - 4;
    if (brd32(c, h->ctx) & 0x20) pc |= 1;  // el hilo empieza en Thumb
    uint32_t salida = c->r[14];
    fprintf(stderr, "  [%s: hilo nuevo, contexto %08X, empieza en %08X]\n", c->id ? "ARM7" : "ARM9",
            h->ctx, pc & ~1u);
    cpu_llama(c, pc);
    cpu_llama(c, salida);  // la función del hilo terminó: va a OS_ExitThread
    fprintf(stderr, "[hilos] un hilo ha terminado sin cambiar a otro\n");
    exit(8);
}

void hle_load_context(Cpu *c) {
    uint32_t ctx = c->r[0];
    Hilo *h = busca(c, ctx);
    cargando[c->id] = ctx;
    if (h->guardado && h->fibra == GetCurrentFiber()) { entra(c); return; }
    if (!h->fibra) {
        Arranque *a = malloc(sizeof *a);
        a->c = c;
        a->h = h;
        h->fibra = CreateFiberEx(64 * 1024, 64 * 1024 * 1024, 0, fibra_nueva, a);
        if (!h->fibra) { fprintf(stderr, "[hilos] no se pudo crear la fibra\n"); exit(8); }
    }
    SwitchToFiber(h->fibra);
    entra(c);  // alguien ha cargado un contexto de esta fibra
}
