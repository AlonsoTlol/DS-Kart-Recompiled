// Despacho de saltos calculados, modos del procesador y coprocesador CP15.
#include "ds.h"
#include <stdio.h>
#include <stdlib.h>

static Func buscar(const FuncEntry *tabla, size_t n, uint32_t key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (tabla[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && tabla[lo].key == key ? tabla[lo].fn : NULL;
}

// Busca en los overlays la función cuya firma coincide con lo que hay en memoria
static Func buscar_ov(uint32_t key) {
    uint32_t a = key & ~1u;
    uint32_t firma = rd16_9(a) | rd16_9(a + 2) << 16;
    for (size_t k = 0; k < g_noverlays; k++) {
        const FuncEntryOv *t = g_overlays[k].t;
        size_t lo = 0, hi = *g_overlays[k].n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (t[mid].key < key) lo = mid + 1;
            else hi = mid;
        }
        if (lo < *g_overlays[k].n && t[lo].key == key && t[lo].firma == firma) return t[lo].fn;
    }
    return NULL;
}

void dispatch(Cpu *c, uint32_t t) {
    uint32_t key = (t & 1) ? t : (t & ~3u);
    Func f = buscar(g_funcs, g_nfuncs, key);
    if (!f) f = buscar_ov(key);
    if (!f) cpu_missing(c, key);
    f(c);
}

void dispatch7(Cpu *c, uint32_t t) {
    uint32_t key = (t & 1) ? t : (t & ~3u);
    Func f = buscar(g_funcs7, g_nfuncs7, key);
    if (!f) cpu_missing(c, key);
    f(c);
}

void cpu_llama(Cpu *c, uint32_t t) {
    if (c->id) dispatch7(c, t);
    else dispatch(c, t);
}

static void volcado(Cpu *c) {
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "  r%-2d %08X%s", i, c->r[i], i % 4 == 3 ? "\n" : "");
    fprintf(stderr, "  cpsr %08X   última función %08X\n", cpu_get_cpsr(c), c->func);
}

_Noreturn void cpu_missing(Cpu *c, uint32_t addr) {
    fprintf(stderr, "\n[recomp] falta código traducido del %s en %08X (%s)\n", c->id ? "ARM7" : "ARM9",
            addr & ~1u, (addr & 1) ? "Thumb" : "ARM");
    volcado(c);
    fprintf(stderr, "  en memoria:");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %04X", c->id ? rd16_7((addr & ~1u) + i * 2) : rd16_9((addr & ~1u) + i * 2));
    fprintf(stderr, "\n");
    // MKDS_VUELCA_RAM=archivo: guarda la memoria principal, la ITCM y la DTCM para estudiarlas
    const char *vr = getenv("MKDS_VUELCA_RAM");
    if (vr && *vr) {
        extern uint8_t g_main[0x400000], g_itcm[0x8000], g_dtcm[0x4000];
        FILE *m = fopen(vr, "wb");
        if (m) { fwrite(g_main, 1, sizeof g_main, m); fwrite(g_itcm, 1, sizeof g_itcm, m); fwrite(g_dtcm, 1, sizeof g_dtcm, m); fclose(m); }
    }
    if (!(addr & ~1u)) exit(2);  // saltar a 0 es un fallo, no código que falte
    // se apunta para que la siguiente traducción la incluya como función
    FILE *f = fopen(c->id ? "faltan7.txt" : "faltan.txt", "a");
    if (f) { fprintf(f, "%s%08X\n", (addr & 1) ? "T:" : "", addr & ~1u); fclose(f); }
    exit(2);
}

_Noreturn void cpu_undefined(Cpu *c, uint32_t addr, uint32_t op) {
    fprintf(stderr, "\n[recomp] instrucción no soportada %08X en %08X (%s)\n", op, addr,
            c->id ? "ARM7" : "ARM9");
    volcado(c);
    exit(3);
}

// ---- CPSR y modos ----------------------------------------------------------
// Cada modo (IRQ, SVC...) tiene su propia pila (r13), r14 y SPSR.
static int banco(uint32_t modo) {
    int m = modo & 15;
    return m == 15 ? 0 : m;  // usuario y sistema comparten registros
}

static void cambia_modo(Cpu *c, uint32_t nuevo) {
    int a = banco(c->ctl), b = banco(nuevo);
    if (a != b) {
        c->bank_r13[a] = c->r[13]; c->bank_r14[a] = c->r[14]; c->bank_spsr[a] = c->spsr;
        c->r[13] = c->bank_r13[b]; c->r[14] = c->bank_r14[b]; c->spsr = c->bank_spsr[b];
    }
    c->ctl = (c->ctl & ~0x1Fu) | (nuevo & 0x1F) | 0x10;
}

uint32_t cpu_get_cpsr(Cpu *c) {
    return (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28) | (c->q << 27) | c->ctl;
}

uint32_t cpu_get_spsr(Cpu *c) { return c->spsr; }

static uint32_t mezcla(uint32_t viejo, uint32_t nuevo, uint32_t mask) {
    uint32_t m = 0;
    for (int i = 0; i < 4; i++)
        if (mask & (1u << i)) m |= 0xFFu << (8 * i);
    return (viejo & ~m) | (nuevo & m);
}

static void pon_cpsr(Cpu *c, uint32_t v) {
    c->n = v >> 31; c->z = (v >> 30) & 1; c->c = (v >> 29) & 1; c->v = (v >> 28) & 1;
    c->q = (v >> 27) & 1;
    cambia_modo(c, v & 0x1F);
    c->ctl = v & 0xFF;
}

void cpu_msr(Cpu *c, int spsr, uint32_t mask, uint32_t val) {
    if (spsr) c->spsr = mezcla(c->spsr, val, mask);
    else pon_cpsr(c, mezcla(cpu_get_cpsr(c), val, mask));
}

void cpu_restore_spsr(Cpu *c) { pon_cpsr(c, c->spsr); }

void cpu_set_cpsr(Cpu *c, uint32_t v) { pon_cpsr(c, v); }

// ---- CP15 ------------------------------------------------------------------
static uint32_t cp15[16][16][8][8];

uint32_t cp15_read(Cpu *c, int crn, int crm, int op1, int op2) {
    (void)c;
    if (crn == 0 && crm == 0 && op2 == 0) return 0x41059461;  // ARM946E-S
    if (crn == 0 && crm == 0 && op2 == 1) return 0x0F0D2112;  // caché
    return cp15[crn][crm][op1][op2];
}

void cp15_write(Cpu *c, int crn, int crm, int op1, int op2, uint32_t v) {
    cp15[crn][crm][op1][op2] = v;
    if (crn == 9 && crm == 1 && op2 == 0) g_dtcm_base = v & 0xFFFFF000u;  // dónde va la DTCM
    if (crn == 7 && ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2)))   // esperar interrupción
        ds_espera(c, -1);
}
