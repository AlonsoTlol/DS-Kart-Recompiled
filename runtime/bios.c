// Funciones de la BIOS del ARM9 y del ARM7 (las llamadas SWI), hechas en C.
// No se usa la BIOS de Nintendo: se reimplementa lo que hace cada una.
#include "ds.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void cpu_set(Cpu *c, int rapido) {
    uint32_t src = c->r[0], dst = c->r[1], ctl = c->r[2];
    uint32_t n = ctl & 0x1FFFFF;
    int relleno = (ctl >> 24) & 1;
    if (rapido || (ctl >> 26) & 1) {
        uint32_t v = brd32(c, src);
        for (uint32_t i = 0; i < n; i++) bwr32(c, dst + i * 4, relleno ? v : brd32(c, src + i * 4));
    } else {
        uint32_t v = brd16(c, src);
        for (uint32_t i = 0; i < n; i++) bwr16(c, dst + i * 2, relleno ? v : brd16(c, src + i * 2));
    }
}

// escribe un búfer descomprimido: de byte en byte (WRAM) o de 16 bits (VRAM)
static void saca(Cpu *c, uint32_t dst, const uint8_t *buf, uint32_t total, int vram) {
    for (uint32_t j = 0; j < total; j += vram ? 2 : 1) {
        if (vram) bwr16(c, dst + j, buf[j] | (j + 1 < total ? buf[j + 1] << 8 : 0));
        else bwr8(c, dst + j, buf[j]);
    }
}

// LZ77 (SWI 0x11 y 0x12): cabecera de 4 bytes, luego bloques de 8 con banderas
static void lz77(Cpu *c, int vram) {
    uint32_t src = c->r[0];
    uint32_t total = brd32(c, src) >> 8, hecho = 0;
    src += 4;
    uint8_t *buf = malloc(total + 1);
    while (hecho < total) {
        uint32_t flags = brd8(c, src++);
        for (int k = 0; k < 8 && hecho < total; k++, flags <<= 1) {
            if (flags & 0x80) {
                uint32_t b1 = brd8(c, src), b2 = brd8(c, src + 1);
                src += 2;
                uint32_t len = (b1 >> 4) + 3, disp = (((b1 & 15) << 8) | b2) + 1;
                for (uint32_t j = 0; j < len && hecho < total; j++, hecho++)
                    buf[hecho] = hecho >= disp ? buf[hecho - disp] : 0;
            } else {
                buf[hecho++] = (uint8_t)brd8(c, src++);
            }
        }
    }
    saca(c, c->r[1], buf, total, vram);
    free(buf);
}

// RL (SWI 0x14 y 0x15)
static void rl(Cpu *c, int vram) {
    uint32_t src = c->r[0];
    uint32_t total = brd32(c, src) >> 8, hecho = 0;
    src += 4;
    uint8_t *buf = malloc(total + 1);
    while (hecho < total) {
        uint32_t f = brd8(c, src++);
        if (f & 0x80) {
            uint32_t n = (f & 0x7F) + 3, b = brd8(c, src++);
            for (uint32_t j = 0; j < n && hecho < total; j++) buf[hecho++] = (uint8_t)b;
        } else {
            uint32_t n = (f & 0x7F) + 1;
            for (uint32_t j = 0; j < n && hecho < total; j++) buf[hecho++] = (uint8_t)brd8(c, src++);
        }
    }
    saca(c, c->r[1], buf, total, vram);
    free(buf);
}

// Huffman (SWI 0x13): árbol de 8 o 4 bits, datos en palabras de 32 bits
static void huffman(Cpu *c) {
    uint32_t src = c->r[0], hdr = brd32(c, src);
    uint32_t bits = hdr & 15, total = hdr >> 8, hecho = 0;
    uint32_t arbol = src + 4, datos = arbol + (brd8(c, arbol) + 1) * 2;
    uint8_t *buf = calloc(total + 4, 1);
    uint32_t nodo = arbol + 1, acc = 0, nacc = 0;
    while (hecho < total) {
        uint32_t w = brd32(c, datos);
        datos += 4;
        for (int i = 31; i >= 0 && hecho < total; i--) {
            uint32_t b = (w >> i) & 1, n = brd8(c, nodo);
            uint32_t sig = (nodo & ~1u) + (n & 0x3F) * 2 + 2 + b;
            if (n & (b ? 0x40 : 0x80)) {  // hoja
                acc |= brd8(c, sig) << nacc;
                nacc += bits;
                nodo = arbol + 1;
                if (nacc == 8) { buf[hecho++] = (uint8_t)acc; acc = 0; nacc = 0; }
            } else {
                nodo = sig;
            }
        }
    }
    saca(c, c->r[1], buf, total, 0);
    free(buf);
}

static uint32_t crc16(Cpu *c, uint32_t crc, uint32_t src, uint32_t len) {
    static const uint16_t val[8] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
    for (uint32_t i = 0; i < len; i++) {
        crc ^= brd8(c, src + i);
        for (int j = 0; j < 8; j++) {
            int carry = crc & 1;
            crc >>= 1;
            if (carry) crc ^= (uint32_t)val[j] << (7 - j);
        }
    }
    return crc & 0xFFFF;
}

// BitUnPack (SWI 0x10): amplía datos de N bits a M bits
static void bit_unpack(Cpu *c) {
    uint32_t src = c->r[0], dst = c->r[1], info = c->r[2];
    uint32_t len = brd16(c, info), wsrc = brd8(c, info + 2), wdst = brd8(c, info + 3);
    uint32_t off = brd32(c, info + 4), cero = off >> 31;
    off &= 0x7FFFFFFF;
    uint32_t out = 0, nout = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t b = brd8(c, src + i);
        for (uint32_t k = 0; k < 8; k += wsrc) {
            uint32_t v = (b >> k) & ((1u << wsrc) - 1);
            if (v || cero) v += off;
            out |= v << nout;
            nout += wdst;
            if (nout >= 32) { bwr32(c, dst, out); dst += 4; out = 0; nout = 0; }
        }
    }
}

void bios_swi(Cpu *c, uint32_t n) {
    switch (n) {
    case 0x03: ds_cede(c); return;  // WaitByLoop: espera activa, buen momento para ceder el turno
    case 0x04: case 0x05: case 0x06:  // IntrWait, VBlankIntrWait, Halt
        ds_espera(c, (int)n);
        return;
    case 0x07: ds_espera(c, 6); return;  // Sleep (ARM7): como Halt
    case 0x08: return;                   // SoundBias (ARM7)
    case 0x09: {  // Div
        int32_t a = (int32_t)c->r[0], b = (int32_t)c->r[1];
        if (b == 0) { c->r[0] = a < 0 ? (uint32_t)-1 : 1; c->r[1] = (uint32_t)a; c->r[3] = 1; return; }
        if (a == INT32_MIN && b == -1) { c->r[0] = (uint32_t)a; c->r[1] = 0; c->r[3] = (uint32_t)a; return; }
        int32_t q = a / b;
        c->r[0] = (uint32_t)q; c->r[1] = (uint32_t)(a % b); c->r[3] = (uint32_t)(q < 0 ? -q : q);
        return;
    }
    case 0x0B: cpu_set(c, 0); return;
    case 0x0C: cpu_set(c, 1); return;
    case 0x0D: {  // Sqrt
        uint32_t v = c->r[0], r = 0, bit = 1u << 30;
        while (bit > v) bit >>= 2;
        while (bit) {
            if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
            else r >>= 1;
            bit >>= 2;
        }
        c->r[0] = r;
        return;
    }
    case 0x0E: c->r[0] = crc16(c, c->r[0], c->r[1], c->r[2]); return;
    case 0x0F: c->r[0] = 0; return;  // IsDebugger
    case 0x10: bit_unpack(c); return;
    case 0x11: lz77(c, 0); return;
    case 0x12: lz77(c, 1); return;
    case 0x13: huffman(c); return;
    case 0x14: rl(c, 0); return;
    case 0x15: rl(c, 1); return;
    case 0x1A:  // GetSineTable (ARM7): seno de 0 a 90 grados en 64 pasos, en 1/4096
        c->r[0] = (uint32_t)(int32_t)lround(sin((c->r[0] & 63) * 3.14159265358979 / 128) * 4096);
        return;
    case 0x1B:  // GetPitchTable (ARM7): (2^(n/768) - 1) * 0x10000
        c->r[0] = (uint32_t)lround((pow(2.0, (c->r[0] % 768) / 768.0) - 1) * 65536) & 0xFFFF;
        return;
    case 0x1C: {  // GetVolumeTable (ARM7): 724 pasos de 1/8 dB, cada tramo de 6 dB repite 64..127
        uint32_t i = c->r[0] % 724;
        double db = (i - 723.0) / 8.0;
        double amp = pow(10.0, db / 20.0);
        while (amp < 0.5 && amp > 0) amp *= 2;  // el tramo lo da el divisor del canal
        c->r[0] = i == 0 ? 0 : (uint32_t)lround(amp * 127);
        return;
    }
    case 0x1F: return;  // CustomPost
    }
    fprintf(stderr, "[recomp] SWI %02X de la BIOS del %s sin hacer todavía\n", n, c->id ? "ARM7" : "ARM9");
    exit(4);
}
