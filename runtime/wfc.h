// Lo que comparten el router de la Conexión Wi-Fi de Nintendo (wfc.c) y el servidor WFC que se imita (wfc_servicios.c)
#pragma once
#include <stdint.h>
#include <stdio.h>

// Las DS de todos los jugadores están en una red virtual 10.0.0.0/8: cada una con su IP (al azar, 10.1.0.2 a
// 10.254.255.254), y lo que una manda a otra va por el túnel entre los dos PCs (wfc.c).
#define IP_ROUTER 0x0A000001u  // 10.0.0.1: el router (DHCP y DNS)
#define IP_WFC    0x0A00000Au  // 10.0.0.10: todo *.nintendowifi.net
extern uint32_t wfc_ip_ds;     // la de esta DS
#define IP_DS wfc_ip_ds
static inline int es_otra_ds(uint32_t ip) { return (ip >> 24) == 10 && (ip >> 16 & 255) != 0 && ip != wfc_ip_ds; }

// el túnel (wfc.c)
void wfc_tunel_pon(uint32_t vip, uint32_t ip, uint16_t puerto, uint32_t ipl, uint16_t puertol);  // dónde está esa DS
int wfc_tunel_direcciones(uint32_t *ip, uint16_t *puerto, uint32_t *ipl, uint16_t *puertol);      // las mías (STUN)

extern int wfc_traza;
#define WT(...) do { if (wfc_traza > 0) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)

static inline uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void p32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// wfc.c: una conexión TCP de la DS con el servidor WFC
typedef struct Con Con;
void wfc_con_envia(Con *c, const void *d, int n);  // a la DS
void wfc_con_cierra(Con *c);                       // cuando se haya mandado todo
void **wfc_con_datos(Con *c);                      // para el servicio (se libera con free)
void wfc_udp_a_la_ds(uint32_t origen, uint16_t po, uint16_t pd, const uint8_t *datos, int n);

// wfc_servicios.c
void wfc_servicio_abre(Con *c, uint16_t puerto);
void wfc_servicio_datos(Con *c, uint16_t puerto, uint8_t *ent, int *nent);  // quita de ent lo que use
void wfc_servicio_cierra(Con *c);
void wfc_servicio_udp(uint16_t po, uint16_t pd, const uint8_t *d, int n);
void wfc_servicio_atiende(uint64_t ms);
