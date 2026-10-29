// Hardware de la DS que comparten los dos procesadores (ds.c).
#pragma once
#include "cpu.h"

extern Cpu g_cpu9, g_cpu7;
extern uint8_t g_wramcnt;  // reparto de la WRAM compartida (32 KB) entre ARM9 y ARM7

uint32_t io9_rd(uint32_t a, int bytes);
void io9_wr(uint32_t a, uint32_t v, int bytes);
uint32_t io7_rd(uint32_t a, int bytes);
void io7_wr(uint32_t a, uint32_t v, int bytes);

// SWI Halt / IntrWait / VBlankIntrWait, o CP15 "wait for interrupt" (swi = -1)
void ds_espera(Cpu *c, int swi);

// Espera activa (WaitByLoop): deja correr ya al otro procesador
void ds_cede(Cpu *c);
// Arranca los dos procesadores (no vuelve)
void ds_arranca(uint32_t entrada9, uint32_t entrada7);

// Datos de la ROM para la tarjeta
void ds_pon_rom(const uint8_t *rom, size_t tam);

// Configuración del sistema operativo del juego (NitroSDK) para cada procesador
typedef struct {
    uint32_t cp_save, cp_restore;  // CP_SaveContext / CP_RestoreContext (0 si no hay)
    uint32_t reschedule;           // OSi_RescheduleThread
    uint32_t cola_irq;             // hilos esperando una interrupción
    uint32_t flag_resched;         // "hay que cambiar de hilo" (media palabra)
    uint32_t off_estado, off_enlace, off_sig;  // campos del hilo (estado, enlaces de cola, siguiente)
} SoConfig;
extern SoConfig g_so[2];
