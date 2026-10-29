// Motor 3D de la DS (gpu3d.c)
#pragma once
#include <stdint.h>

void gx_inicia(void);
void gx_escribe(uint32_t a, uint32_t v);   // 0x04000400-0x040005FF: FIFO y puertos de órdenes
uint32_t gx_lee(uint32_t a);               // 0x04000600-0x040006A3: estado y resultados
void gx_vblank(const uint8_t *io9);        // si se pidió SWAP_BUFFERS, pinta la escena

// Última imagen 3D (256x192): color RGB555 y alfa 0..31 (0 = transparente)
const uint16_t *gx_color(void);
const uint8_t *gx_alfa(void);
// Lo mismo a tamaño grande (256*esc x 192*esc), con el reescalado (video_escala() > 1)
const uint16_t *gx_hd_color(void);
const uint8_t *gx_hd_alfa(void);
// Imágenes intermedias (MKDS_FPS): juego de salidas que se enseña ahora y, de cada juego, cuántas
// intermedias tiene (de la escena anterior a la suya) y cada una (a tamaño 256*esc x 192*esc)
int gx_indice(void);
int gx_nsub(int i);
const uint16_t *gx_sub_color(int i, int j);
const uint8_t *gx_sub_alfa(int i, int j);
int gx_polis(void);                        // polígonos de la última escena (para depurar)
uint64_t gx_frames(void);
