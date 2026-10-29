// Estado de los procesadores (ARM9 y ARM7) y ayudas que usa el código traducido.
// El código del ARM7 se compila con -DCPU7: así sus rd32/wr32... usan el mapa
// de memoria del ARM7.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct Cpu {
    uint32_t r[16];
    uint32_t n, z, c, v, q;      // banderas del CPSR, cada una 0 o 1
    uint32_t ctl;                // bits 0-7 del CPSR: modo, I, F, T
    uint32_t spsr;
    uint32_t bank_r13[16], bank_r14[16], bank_spsr[16];  // por modo (ctl & 15)
    int id;                      // 0 = ARM9, 1 = ARM7
    int32_t ciclos;              // "presupuesto": al acabarse se mira el hardware (cpu_tick)
    volatile uint32_t func;      // función traducida que se está ejecutando (para depurar)
} Cpu;

typedef void (*Func)(Cpu *c);
typedef struct { uint32_t key; Func fn; } FuncEntry;  // key = dirección | 1 si es Thumb
extern const FuncEntry g_funcs[], g_funcs7[];
extern const size_t g_nfuncs, g_nfuncs7;

// Overlays del ARM9: varias funciones pueden compartir dirección; la "firma"
// (sus 4 primeros bytes) dice cuál es la que está cargada en memoria ahora.
typedef struct { uint32_t key; Func fn; uint32_t firma; } FuncEntryOv;
typedef struct { const FuncEntryOv *t; const size_t *n; } TablaOv;
extern const TablaOv g_overlays[];
extern const size_t g_noverlays;

#define R (c->r)

void dispatch(Cpu *c, uint32_t target);   // ARM9
void dispatch7(Cpu *c, uint32_t target);  // ARM7
void cpu_llama(Cpu *c, uint32_t target);  // cualquiera de los dos según c->id
void cpu_tick(Cpu *c);                    // hardware, interrupciones y turno del otro procesador
_Noreturn void cpu_undefined(Cpu *c, uint32_t addr, uint32_t op);
_Noreturn void cpu_missing(Cpu *c, uint32_t addr);
uint32_t cpu_get_cpsr(Cpu *c);
uint32_t cpu_get_spsr(Cpu *c);
void cpu_msr(Cpu *c, int spsr, uint32_t mask, uint32_t val);
void cpu_restore_spsr(Cpu *c);
void cpu_set_cpsr(Cpu *c, uint32_t v);
uint32_t cp15_read(Cpu *c, int crn, int crm, int op1, int op2);
void cp15_write(Cpu *c, int crn, int crm, int op1, int op2, uint32_t v);
void bios_swi(Cpu *c, uint32_t n);

// registro r del modo usuario (para ldm/stm con "^")
static inline uint32_t *cpu_usr(Cpu *c, int r) {
    uint32_t m = c->ctl & 15;
    if (r < 13 || m == 0 || m == 15) return &c->r[r];
    return r == 13 ? &c->bank_r13[0] : &c->bank_r14[0];
}

// hilos del sistema operativo de la DS (hilos.c)
void hle_save_context(Cpu *c);
void hle_load_context(Cpu *c);
void hle_init_context(Cpu *c);  // un hilo nuevo reutiliza este contexto

// trucos (TODO_DESBLOQUEADO=1 -> bit 0)
extern uint32_t g_trucos;
int truco_desbloqueado(Cpu *c);
int truco_cilindradas(Cpu *c);
int truco_niveles_mision(Cpu *c);
int truco_jefe_mision(Cpu *c);
void **hle_ctx_buf(Cpu *c, uint32_t ctx);

// ---- memoria ---------------------------------------------------------------
extern uint8_t g_main[0x400000];
extern uint8_t g_itcm[0x8000];
extern uint8_t g_dtcm[0x4000];
extern uint32_t g_dtcm_base;
uint32_t mem_rd8(uint32_t a);  uint32_t mem_rd16(uint32_t a);  uint32_t mem_rd32(uint32_t a);
void mem_wr8(uint32_t a, uint32_t v);  void mem_wr16(uint32_t a, uint32_t v);
void mem_wr32(uint32_t a, uint32_t v);
uint32_t mem7_rd8(uint32_t a); uint32_t mem7_rd16(uint32_t a); uint32_t mem7_rd32(uint32_t a);
void mem7_wr8(uint32_t a, uint32_t v); void mem7_wr16(uint32_t a, uint32_t v);
void mem7_wr32(uint32_t a, uint32_t v);

// Camino rápido: la RAM principal (0x02xxxxxx); en el ARM9, salvo donde está la DTCM.
#define EN_MAIN9(a) (((a) >> 24) == 2 && (uint32_t)((a) - g_dtcm_base) >= 0x4000)
#define EN_MAIN7(a) (((a) >> 24) == 2)

#define ACCESOS(suf, EN, slow)                                                         \
    static inline uint32_t rd8##suf(uint32_t a) {                                      \
        if (EN(a)) return g_main[a & 0x3FFFFF];                                        \
        return slow##rd8(a);                                                           \
    }                                                                                  \
    static inline uint32_t rd16##suf(uint32_t a) {                                     \
        a &= ~1u;                                                                      \
        if (EN(a)) { uint16_t v; memcpy(&v, g_main + (a & 0x3FFFFF), 2); return v; }   \
        return slow##rd16(a);                                                          \
    }                                                                                  \
    static inline uint32_t rd32##suf(uint32_t a) {                                     \
        a &= ~3u;                                                                      \
        if (EN(a)) { uint32_t v; memcpy(&v, g_main + (a & 0x3FFFFF), 4); return v; }   \
        return slow##rd32(a);                                                          \
    }                                                                                  \
    static inline void wr8##suf(uint32_t a, uint32_t v) {                              \
        if (EN(a)) { g_main[a & 0x3FFFFF] = (uint8_t)v; return; }                      \
        slow##wr8(a, v);                                                               \
    }                                                                                  \
    static inline void wr16##suf(uint32_t a, uint32_t v) {                             \
        a &= ~1u;                                                                      \
        if (EN(a)) { uint16_t x = (uint16_t)v; memcpy(g_main + (a & 0x3FFFFF), &x, 2); return; } \
        slow##wr16(a, v);                                                              \
    }                                                                                  \
    static inline void wr32##suf(uint32_t a, uint32_t v) {                             \
        a &= ~3u;                                                                      \
        if (EN(a)) { memcpy(g_main + (a & 0x3FFFFF), &v, 4); return; }                 \
        slow##wr32(a, v);                                                              \
    }

ACCESOS(_9, EN_MAIN9, mem_)
ACCESOS(_7, EN_MAIN7, mem7_)

// El código traducido usa rd32() etc.: del ARM9 o del ARM7 según cómo se compile
#ifdef CPU7
#define rd8 rd8_7
#define rd16 rd16_7
#define rd32 rd32_7
#define wr8 wr8_7
#define wr16 wr16_7
#define wr32 wr32_7
#else
#define rd8 rd8_9
#define rd16 rd16_9
#define rd32 rd32_9
#define wr8 wr8_9
#define wr16 wr16_9
#define wr32 wr32_9
#endif

// Para el runtime, que trabaja con los dos procesadores
static inline uint32_t brd8(Cpu *c, uint32_t a) { return c->id ? rd8_7(a) : rd8_9(a); }
static inline uint32_t brd16(Cpu *c, uint32_t a) { return c->id ? rd16_7(a) : rd16_9(a); }
static inline uint32_t brd32(Cpu *c, uint32_t a) { return c->id ? rd32_7(a) : rd32_9(a); }
static inline void bwr8(Cpu *c, uint32_t a, uint32_t v) { if (c->id) wr8_7(a, v); else wr8_9(a, v); }
static inline void bwr16(Cpu *c, uint32_t a, uint32_t v) { if (c->id) wr16_7(a, v); else wr16_9(a, v); }
static inline void bwr32(Cpu *c, uint32_t a, uint32_t v) { if (c->id) wr32_7(a, v); else wr32_9(a, v); }

// ---- operaciones -----------------------------------------------------------
static inline uint32_t ror32(uint32_t v, uint32_t s) {
    s &= 31;
    return s ? (v >> s) | (v << (32 - s)) : v;
}
// LDR con dirección desalineada: lee alineado y rota
static inline uint32_t ldr32(uint32_t a) { return ror32(rd32(a), (a & 3) * 8); }
static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }

static inline uint32_t nz(Cpu *c, uint32_t r) { c->n = r >> 31; c->z = r == 0; return r; }
static inline uint32_t add_f(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->n = r >> 31; c->z = r == 0; c->c = r < a; c->v = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t adc_f(Cpu *c, uint32_t a, uint32_t b) {
    uint64_t t = (uint64_t)a + b + c->c;
    uint32_t r = (uint32_t)t;
    c->n = r >> 31; c->z = r == 0; c->c = (uint32_t)(t >> 32); c->v = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t sub_f(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->n = r >> 31; c->z = r == 0; c->c = a >= b; c->v = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t sbc_f(Cpu *c, uint32_t a, uint32_t b) {
    uint32_t borrow = c->c ^ 1;
    uint32_t r = a - b - borrow;
    c->n = r >> 31; c->z = r == 0; c->c = (uint64_t)a >= (uint64_t)b + borrow;
    c->v = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}

// desplazamientos por registro, con el bit que sale (carry)
static inline uint32_t lsl_c(uint32_t v, uint32_t s, uint32_t *cy) {
    if (s == 0) return v;
    if (s < 32) { *cy = (v >> (32 - s)) & 1; return v << s; }
    *cy = s == 32 ? (v & 1) : 0;
    return 0;
}
static inline uint32_t lsr_c(uint32_t v, uint32_t s, uint32_t *cy) {
    if (s == 0) return v;
    if (s < 32) { *cy = (v >> (s - 1)) & 1; return v >> s; }
    *cy = s == 32 ? (v >> 31) : 0;
    return 0;
}
static inline uint32_t asr_c(uint32_t v, uint32_t s, uint32_t *cy) {
    if (s == 0) return v;
    if (s < 32) { *cy = ((uint32_t)((int32_t)v >> (s - 1))) & 1; return (uint32_t)((int32_t)v >> s); }
    *cy = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t ror_c(uint32_t v, uint32_t s, uint32_t *cy) {
    if (s == 0) return v;
    s &= 31;
    if (s == 0) { *cy = v >> 31; return v; }
    *cy = (v >> (s - 1)) & 1;
    return (v >> s) | (v << (32 - s));
}

// aritmética saturada (QADD y compañía) del ARMv5TE
static inline uint32_t qadd(Cpu *c, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a + (int32_t)b;
    if (r > INT32_MAX) { c->q = 1; return INT32_MAX; }
    if (r < INT32_MIN) { c->q = 1; return (uint32_t)INT32_MIN; }
    return (uint32_t)r;
}
static inline uint32_t qsub(Cpu *c, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a - (int32_t)b;
    if (r > INT32_MAX) { c->q = 1; return INT32_MAX; }
    if (r < INT32_MIN) { c->q = 1; return (uint32_t)INT32_MIN; }
    return (uint32_t)r;
}
