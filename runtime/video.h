// Vídeo de la DS: bancos de VRAM, motores 2D y la ventana de Windows.
#pragma once
#include <stdint.h>

extern uint8_t g_paleta[0x800], g_oam[0x800];

// VRAM vista por el ARM9 (0x06000000-0x06FFFFFF); NULL si no hay banco ahí
uint8_t *vram_ptr9(uint32_t a);
void video_vramcnt(int banco, uint8_t v);  // banco 0..8 = A..I

// Dibuja las dos pantallas con el estado actual de los registros del ARM9
// (io9 = los registros 0x04000000-0x0400FFFF). Deja 256x192 píxeles RGB en cada una.
void video_dibuja(const uint8_t *io9, uint32_t *arriba, uint32_t *abajo);
// Lo mismo en otro hilo: hace una foto de la VRAM, las paletas y los registros y la monta y la enseña
// (ventana_muestra) desde allí. Si el hilo aún va por la anterior, esta imagen se salta.
void video_dibuja_aparte(const uint8_t *io9);
void video_espera_montaje(void);  // antes de montar en este hilo: que el otro haya terminado

// Ventana
void ventana_inicia(void);
void ventana_muestra(const uint32_t *arriba, const uint32_t *abajo);
void ventana_muestra_en(const uint32_t *arriba, const uint32_t *abajo, int64_t ts);  // ts: cuándo (0 = ya)
uint32_t *ventana_reserva_intermedia(void);      // imágenes intermedias (MKDS_FPS)
void ventana_publica_intermedia(int64_t ts);
int64_t ventana_reloj(void);                     // QueryPerformanceCounter
void ventana_mensajes(void);

// Entrada: teclas de la DS (activas a 0, como el hardware) y lápiz
extern volatile uint16_t g_keyinput;   // A B Select Start Der Izq Arr Abj R L
extern volatile uint16_t g_keyxy;      // bit 0 X, bit 1 Y (0 = pulsado)
extern volatile int g_lapiz, g_lapiz_x, g_lapiz_y;
