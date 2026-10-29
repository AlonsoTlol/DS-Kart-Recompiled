// Mapas de memoria del ARM9 y del ARM7. La RAM principal tiene camino rápido
// en cpu.h; aquí va todo lo demás.
#include "ds.h"
#include "video.h"
#include <stdio.h>

uint8_t g_main[0x400000];
uint8_t g_itcm[0x8000];
uint8_t g_dtcm[0x4000];
uint32_t g_dtcm_base = 0x027E0000;

static uint8_t wram_comp[0x8000];      // WRAM compartida (32 KB)
static uint8_t wram7[0x10000];         // WRAM propia del ARM7 (64 KB)
static uint8_t vram7[0x40000];         // VRAM C/D prestada al ARM7 (código del wifi)
static uint8_t wifi[0x10000];          // registros y RAM del wifi (de momento, memoria)

// Mitad de la WRAM compartida que ve cada procesador según WRAMCNT
static uint8_t *wram_arm9(uint32_t a) {
    switch (g_wramcnt & 3) {
    case 0: return wram_comp + (a & 0x7FFF);
    case 1: return wram_comp + 0x4000 + (a & 0x3FFF);
    case 2: return wram_comp + (a & 0x3FFF);
    }
    return NULL;
}
static uint8_t *wram_arm7(uint32_t a) {
    switch (g_wramcnt & 3) {
    case 1: return wram_comp + (a & 0x3FFF);
    case 2: return wram_comp + 0x4000 + (a & 0x3FFF);
    case 3: return wram_comp + (a & 0x7FFF);
    }
    return wram7 + (a & 0xFFFF);  // sin WRAM compartida: espejo de la suya
}

// ---- ARM9 --------------------------------------------------------------------
static uint8_t *ptr9(uint32_t a) {
    if ((uint32_t)(a - g_dtcm_base) < 0x4000) return g_dtcm + (a - g_dtcm_base);
    switch (a >> 24) {
    case 0x00: case 0x01: return g_itcm + (a & 0x7FFF);
    case 0x02: return g_main + (a & 0x3FFFFF);
    case 0x03: return wram_arm9(a);
    case 0x05: return g_paleta + (a & 0x7FF);
    case 0x06: return vram_ptr9(a);   // según los bancos asignados (video.c)
    case 0x07: return g_oam + (a & 0x7FF);
    }
    return NULL;
}

static uint32_t lee9(uint32_t a, int n) {
    uint8_t *p = ptr9(a);
    if (p) { uint32_t v = 0; memcpy(&v, p, n); return v; }
    if ((a >> 24) == 4) return io9_rd(a, n);
    return 0;
}
// depuración: escrituras a VRAM perdidas (sin banco en esa dirección)
static uint32_t vram_perdidas, vram_perdida_min = 0xFFFFFFFF, vram_perdida_max;
void vram_depura(void) {
    if (vram_perdidas)
        fprintf(stderr, "    escrituras a VRAM perdidas: %u (de %08X a %08X)\n", vram_perdidas, vram_perdida_min,
                vram_perdida_max);
}

static void escribe9(uint32_t a, uint32_t v, int n) {
    uint8_t *p = ptr9(a);
    if (p) { memcpy(p, &v, n); return; }
    if ((a >> 24) == 4) { io9_wr(a, v, n); return; }
    if ((a >> 24) == 6) {
        vram_perdidas++;
        if (a < vram_perdida_min) vram_perdida_min = a;
        if (a > vram_perdida_max) vram_perdida_max = a;
    }
}

uint32_t mem_rd8(uint32_t a) { return lee9(a, 1); }
uint32_t mem_rd16(uint32_t a) { return lee9(a, 2); }
uint32_t mem_rd32(uint32_t a) { return lee9(a, 4); }
void mem_wr8(uint32_t a, uint32_t v) { escribe9(a, v, 1); }
void mem_wr16(uint32_t a, uint32_t v) { escribe9(a, v, 2); }
void mem_wr32(uint32_t a, uint32_t v) { escribe9(a, v, 4); }

// ---- ARM7 --------------------------------------------------------------------
static uint8_t *ptr7(uint32_t a) {
    switch (a >> 24) {
    case 0x02: return g_main + (a & 0x3FFFFF);
    case 0x03: return a >= 0x03800000 ? wram7 + (a & 0xFFFF) : wram_arm7(a);
    case 0x04: return a >= 0x04800000 ? wifi + (a & 0xFFFF) : NULL;
    case 0x06: return vram7 + (a & 0x3FFFF);
    }
    return NULL;
}

static uint32_t lee7(uint32_t a, int n) {
    uint8_t *p = ptr7(a);
    if (p) { uint32_t v = 0; memcpy(&v, p, n); return v; }
    if ((a >> 24) == 4) return io7_rd(a, n);
    return 0;
}
static void escribe7(uint32_t a, uint32_t v, int n) {
    uint8_t *p = ptr7(a);
    if (p) { memcpy(p, &v, n); return; }
    if ((a >> 24) == 4) io7_wr(a, v, n);
}

uint32_t mem7_rd8(uint32_t a) { return lee7(a, 1); }
uint32_t mem7_rd16(uint32_t a) { return lee7(a, 2); }
uint32_t mem7_rd32(uint32_t a) { return lee7(a, 4); }
void mem7_wr8(uint32_t a, uint32_t v) { escribe7(a, v, 1); }
void mem7_wr16(uint32_t a, uint32_t v) { escribe7(a, v, 2); }
void mem7_wr32(uint32_t a, uint32_t v) { escribe7(a, v, 4); }
