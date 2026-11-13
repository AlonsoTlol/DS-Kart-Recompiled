// Vídeo de fondo del launcher de Linux y macOS (lanzador.cpp), como el portada.mp4 del de Windows: portada.mpg
// (MPEG-1, sin sonido, en bucle), decodificado con pl_mpeg (launcher/pl_mpeg.h, de Dominic Szablewski, licencia MIT).
// Es C aparte porque pl_mpeg es C y el launcher es C++ sin biblioteca de C++.
#include <stdint.h>
#include <stdio.h>  // pl_mpeg usa FILE sin incluirlo
#include <stdlib.h>
#include <string.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

static plm_t *plm;
static uint8_t *rgba;
static int ancho, alto, nuevo;

static void al_decodificar(plm_t *p, plm_frame_t *f, void *u) {
    (void)p; (void)u;
    plm_frame_to_rgba(f, rgba, ancho * 4);
    nuevo = 1;
}

// 1 si se puede abrir; deja el tamaño en *w y *h
int portada_abre(const char *ruta, int *w, int *h) {
    plm = plm_create_with_filename(ruta);
    if (!plm) return 0;
    if (!plm_probe(plm, 5000 * 1024) || plm_get_num_video_streams(plm) < 1) { plm_destroy(plm); plm = NULL; return 0; }
    ancho = plm_get_width(plm);
    alto = plm_get_height(plm);
    rgba = (uint8_t *)malloc((size_t)ancho * alto * 4);
    if (!rgba || ancho <= 0 || alto <= 0) { plm_destroy(plm); plm = NULL; return 0; }
    memset(rgba, 0xFF, (size_t)ancho * alto * 4);  // opaco: pl_mpeg no escribe el canal alfa
    plm_set_audio_enabled(plm, 0);
    plm_set_loop(plm, 1);
    plm_set_video_decode_callback(plm, al_decodificar, NULL);
    *w = ancho;
    *h = alto;
    return 1;
}

// Avanza dt segundos; si hay una imagen nueva, devuelve sus píxeles (RGBA, ancho x alto), si no NULL
const uint8_t *portada_avanza(double dt) {
    if (!plm) return NULL;
    if (dt > 0.25) dt = 0.25;  // tras una pausa (el juego abierto), sin correr para alcanzar
    nuevo = 0;
    plm_decode(plm, dt);
    return nuevo ? rgba : NULL;
}

void portada_cierra(void) {
    if (plm) plm_destroy(plm);
    plm = NULL;
    free(rgba);
    rgba = NULL;
}
