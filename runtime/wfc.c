// La red de la Conexión Wi-Fi de Nintendo: el "router" del punto de acceso que imita wifi.c.
//
// En modo DCF la DS manda y recibe tramas de datos 802.11: LLC/SNAP (AA AA 03 00 00 00 + tipo) y dentro ARP o IPv4
// de verdad (la pila TCP/IP de la DS, CPS, está en el código del juego). Aquí se contesta como un router de casa:
//   - DHCP: la DS es 10.x.y.z (wfc_ip_ds, al azar), el router 10.0.0.1 (también DNS).
//   - ARP: cualquier dirección que no sea la de la DS es el router (su MAC es la del punto de acceso).
//   - DNS: los nombres de nintendowifi.net (el WFC de Nintendo, que ya no existe) van a 10.0.0.10, un servidor
//     WFC que se imita aquí mismo (wfc_servicios.c); el resto se resuelve de verdad.
//   - UDP con otra DS (10.x.y.z): por el túnel, directo al PC de ese jugador (ver más abajo).
//   - TCP y UDP: con 10.0.0.10, a esos servicios; con cualquier otra dirección, por sockets del PC (NAT).
// La DS y el PC están en la misma máquina: no se pierde nada entre los dos, así que el TCP de este lado es sencillo
// (se confirma todo al momento y no hay reenvíos; solo se respeta la ventana que anuncia la DS).
// MKDS_WIFI_LOG apunta lo que pasa.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include "compat.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wfc.h"

extern uint8_t g_main[0x400000];

static const uint8_t mac_ap[6] = {0x02, 0x44, 0x53, 0x4B, 0x52, 0x01};  // la misma que en wifi.c
int wfc_traza = -1;
uint32_t wfc_ip_ds;
static uint64_t ahora_ms;
static void tunel_abre(void);
static void tunel_udp(uint32_t ipd, uint16_t po, uint16_t pd, const uint8_t *d, int n);
static void tunel_atiende(void);

static uint32_t suma(const uint8_t *p, int n, uint32_t s) {
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    return s;
}
static uint16_t cierra(uint32_t s) {
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

// ---- cola de tramas hacia la DS -------------------------------------------------------------------------
#define NCOLA 256
static struct { int n; uint8_t t[1600]; } cola[NCOLA];
static int cola_ini, cola_fin;

static void a_la_ds(uint16_t tipo, const uint8_t *p, int n) {
    if (n > 1600 - 8 || (cola_fin + 1) % NCOLA == cola_ini) { WT("[wfc] cola llena: se pierde una trama\n"); return; }
    uint8_t *t = cola[cola_fin].t;
    static const uint8_t llc[6] = {0xAA, 0xAA, 0x03, 0, 0, 0};
    memcpy(t, llc, 6);
    p16(t + 6, tipo);
    memcpy(t + 8, p, (size_t)n);
    cola[cola_fin].n = n + 8;
    cola_fin = (cola_fin + 1) % NCOLA;
}

int wfc_recibe(uint8_t *t, int max) {
    if (cola_ini == cola_fin) return 0;
    int n = cola[cola_ini].n;
    if (n > max) n = max;
    memcpy(t, cola[cola_ini].t, (size_t)n);
    cola_ini = (cola_ini + 1) % NCOLA;
    return n;
}

// un paquete IPv4 (cabecera de 20 bytes) hacia la DS con 'datos' como contenido
static void ip_a_la_ds(uint32_t origen, uint32_t destino, int proto, const uint8_t *datos, int n) {
    static uint16_t ident;
    uint8_t p[1600];
    if (n > 1500) return;
    memset(p, 0, 20);
    p[0] = 0x45;
    p16(p + 2, (uint16_t)(20 + n));
    p16(p + 4, ident++);
    p[8] = 64;
    p[9] = (uint8_t)proto;
    p32(p + 12, origen);
    p32(p + 16, destino);
    p16(p + 10, cierra(suma(p, 20, 0)));
    memcpy(p + 20, datos, (size_t)n);
    a_la_ds(0x0800, p, 20 + n);
}

static uint32_t pseudo(uint32_t o, uint32_t d, int proto, int n) {
    uint8_t ps[12];
    p32(ps, o); p32(ps + 4, d); ps[8] = 0; ps[9] = (uint8_t)proto; p16(ps + 10, (uint16_t)n);
    return suma(ps, 12, 0);
}

void wfc_udp_a_la_ds(uint32_t origen, uint16_t po, uint16_t pd, const uint8_t *datos, int n) {
    uint8_t u[1500];
    if (n > 1472) return;
    uint32_t destino = pd == 68 ? 0xFFFFFFFFu : IP_DS;
    p16(u, po); p16(u + 2, pd); p16(u + 4, (uint16_t)(8 + n)); p16(u + 6, 0);
    memcpy(u + 8, datos, (size_t)n);
    uint16_t c = cierra(suma(u, 8 + n, pseudo(origen, destino, 17, 8 + n)));
    p16(u + 6, c ? c : 0xFFFF);
    ip_a_la_ds(origen, destino, 17, u, 8 + n);
}

// ---- ARP ---------------------------------------------------------------------------------------------------
static void arp(const uint8_t *a, int n) {
    if (n < 28 || g16(a + 6) != 1) return;  // solo preguntas
    uint32_t quien = g32(a + 24);
    if (quien == IP_DS) return;  // la DS comprobando que su IP está libre
    uint8_t r[28];
    p16(r, 1); p16(r + 2, 0x0800); r[4] = 6; r[5] = 4; p16(r + 6, 2);
    memcpy(r + 8, mac_ap, 6); p32(r + 14, quien);
    memcpy(r + 18, a + 8, 6); memcpy(r + 24, a + 14, 4);
    a_la_ds(0x0806, r, 28);
}

// ---- DHCP ----------------------------------------------------------------------------------------------------
static void dhcp(const uint8_t *b, int n) {
    if (n < 240 || b[0] != 1 || g32(b + 236) != 0x63825363) return;
    int tipo = 0;
    for (int o = 240; o + 1 < n && b[o] != 255;) {
        if (b[o] == 0) { o++; continue; }
        if (b[o] == 53 && o + 2 < n) tipo = b[o + 2];
        o += 2 + b[o + 1];
    }
    if (tipo != 1 && tipo != 3) return;  // DISCOVER o REQUEST
    uint8_t r[300];
    memset(r, 0, sizeof r);
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, b + 4, 4);              // xid
    memcpy(r + 10, b + 10, 2);            // flags
    p32(r + 16, IP_DS);                   // yiaddr
    p32(r + 20, IP_ROUTER);               // siaddr
    memcpy(r + 28, b + 28, 16);           // chaddr
    p32(r + 236, 0x63825363);
    int o = 240;
    r[o++] = 53; r[o++] = 1; r[o++] = (uint8_t)(tipo == 1 ? 2 : 5);  // OFFER / ACK
    r[o++] = 54; r[o++] = 4; p32(r + o, IP_ROUTER); o += 4;
    r[o++] = 51; r[o++] = 4; p32(r + o, 86400); o += 4;
    r[o++] = 1; r[o++] = 4; p32(r + o, 0xFF000000); o += 4;  // 10.0.0.0/8: todas las DS en la misma red
    r[o++] = 3; r[o++] = 4; p32(r + o, IP_ROUTER); o += 4;
    r[o++] = 6; r[o++] = 4; p32(r + o, IP_ROUTER); o += 4;
    r[o++] = 255;
    WT("[wfc] DHCP: %s %u.%u.%u.%u\n", tipo == 1 ? "ofrezco" : "confirmado", IP_DS >> 24, (IP_DS >> 16) & 255,
       (IP_DS >> 8) & 255, IP_DS & 255);
    wfc_udp_a_la_ds(IP_ROUTER, 67, 68, r, o < 300 ? 300 : o);
}

// ---- DNS ------------------------------------------------------------------------------------------------------
static void dns(uint16_t po, const uint8_t *q, int n) {
    if (n < 12) return;
    char nombre[256];
    int k = 0, o = 12;
    while (o < n && q[o] && k < 250) {
        int l = q[o++];
        if (k) nombre[k++] = '.';
        for (int i = 0; i < l && o < n && k < 250; i++) nombre[k++] = (char)q[o++];
    }
    nombre[k] = 0;
    int fin = o + 1 + 4;  // el 0 final, tipo y clase
    if (fin > n) return;
    uint32_t ip = 0;
    size_t ln = strlen(nombre);
    if (ln >= 16 && !strcmp(nombre + ln - 16, "nintendowifi.net")) ip = IP_WFC;
    else {
        struct addrinfo pista = {0}, *res;
        pista.ai_family = AF_INET;
        if (!getaddrinfo(nombre, NULL, &pista, &res)) {
            ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
            freeaddrinfo(res);
        }
    }
    WT("[wfc] DNS: %s -> %u.%u.%u.%u\n", nombre, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    uint8_t r[512];
    if (fin + 16 > (int)sizeof r) return;
    memcpy(r, q, (size_t)fin);
    r[2] = 0x81; r[3] = ip ? 0x80 : 0x83;  // respuesta, recursión disponible, (no existe)
    p16(r + 6, ip ? 1 : 0); p16(r + 8, 0); p16(r + 10, 0);
    int m = fin;
    if (ip) {
        r[m++] = 0xC0; r[m++] = 12;          // el nombre de la pregunta
        p16(r + m, 1); p16(r + m + 2, 1);    // A, IN
        p32(r + m + 4, 300);                 // TTL
        p16(r + m + 8, 4); p32(r + m + 10, ip);
        m += 14;
    }
    wfc_udp_a_la_ds(IP_ROUTER, 53, po, r, m);
}

// ---- TCP ---------------------------------------------------------------------------------------------------------
struct Con {
    int usada;
    uint16_t po, pd;           // puerto de la DS y de destino
    uint32_t ipd;              // destino
    uint32_t base;             // número de secuencia de sal[0] (lo más viejo que la DS aún no ha confirmado)
    uint32_t su_seq;           // siguiente número que espero de la DS
    uint16_t ventana;          // la que anuncia la DS
    int abierta;               // la DS ha confirmado el SYN ACK
    int cerrar, fin_enviado, fin_ok, fin_ds;  // cerrar cuando se haya mandado todo; FIN mío (y confirmado); FIN de la DS
    int reenvios;
    uint64_t t_envio;          // la última vez que se mandó algo sin confirmar (para reenviarlo)
    SOCKET s;                  // internet (o INVALID_SOCKET si es un servicio)
    uint8_t *sal; int nsal;    // lo que va a la DS: lo mandado sin confirmar y lo que falta por mandar
    int enviados;              // cuánto de sal ya se ha mandado
    uint8_t ent[16384]; int nent;  // lo que ha llegado de la DS (para el servicio)
    void *datos;               // del servicio (se libera con free al cerrar)
};
#define NCON 16
static Con cons[NCON];

static void tcp_segmento(Con *c, uint32_t seq, uint8_t flags, const uint8_t *d, int n) {
    uint8_t t[1500];
    memset(t, 0, 20);
    p16(t, c->pd); p16(t + 2, c->po);
    p32(t + 4, seq); p32(t + 8, c->su_seq);
    t[12] = 5 << 4; t[13] = flags;
    p16(t + 14, 8192);
    if (n) memcpy(t + 20, d, (size_t)n);
    p16(t + 16, cierra(suma(t, 20 + n, pseudo(c->ipd, IP_DS, 6, 20 + n))));
    ip_a_la_ds(c->ipd, IP_DS, 6, t, 20 + n);
}
static uint32_t siguiente_seq(const Con *c) { return c->base + (uint32_t)c->enviados + (uint32_t)c->fin_enviado; }

// lo que haya pendiente para la DS, sin pasarse de su ventana. Aunque la DS esté en el mismo PC, alguna trama se
// puede perder o adelantar a otra (wifi.c las entrega por dos búferes): lo que no confirma se reenvía (wfc_atiende)
static void tcp_vacia(Con *c) {
    if (!c->abierta) return;
    while (c->enviados < c->nsal) {
        int cabe = (int)c->ventana - c->enviados;
        if (cabe <= 0) return;
        int n = c->nsal - c->enviados;
        if (n > 600) n = 600;  // la DS recibe cada trama en medio búfer de START_DCF (0x620 / 2): ~740 bytes como mucho
        if (n > cabe) n = cabe;
        tcp_segmento(c, c->base + (uint32_t)c->enviados, 0x18, c->sal + c->enviados, n);  // PSH ACK
        c->enviados += n;
        c->t_envio = ahora_ms;
    }
    if (c->cerrar && !c->fin_enviado) {  // ya se ha dicho todo: FIN
        tcp_segmento(c, c->base + (uint32_t)c->nsal, 0x11, NULL, 0);
        c->fin_enviado = 1;
        c->t_envio = ahora_ms;
    }
}

void wfc_con_envia(Con *c, const void *d, int n) {
    c->sal = realloc(c->sal, (size_t)(c->nsal + n));
    memcpy(c->sal + c->nsal, d, (size_t)n);
    c->nsal += n;
    tcp_vacia(c);
}
void wfc_con_cierra(Con *c) { c->cerrar = 1; tcp_vacia(c); }
void **wfc_con_datos(Con *c) { return &c->datos; }

static void con_libera(Con *c) {
    if (c->s != INVALID_SOCKET) closesocket(c->s);
    wfc_servicio_cierra(c);
    free(c->sal);
    free(c->datos);
    memset(c, 0, sizeof *c);
    c->s = INVALID_SOCKET;
}

static void tcp(uint32_t ipo, uint32_t ipd, const uint8_t *t, int n) {
    (void)ipo;
    if (n < 20) return;
    uint16_t po = g16(t), pd = g16(t + 2);
    uint32_t seq = g32(t + 4), ack = g32(t + 8);
    int hl = (t[12] >> 4) * 4;
    uint8_t fl = t[13];
    if (hl < 20 || hl > n) return;
    const uint8_t *d = t + hl;
    int nd = n - hl;
    Con *c = NULL;
    for (int i = 0; i < NCON; i++)
        if (cons[i].usada && cons[i].po == po && cons[i].pd == pd && cons[i].ipd == ipd) c = &cons[i];
    if (fl & 0x02) {  // SYN: conexión nueva
        if (c) con_libera(c);
        c = NULL;
        for (int i = 0; i < NCON && !c; i++) if (!cons[i].usada) c = &cons[i];
        if (!c) return;
        memset(c, 0, sizeof *c);
        c->usada = 1; c->po = po; c->pd = pd; c->ipd = ipd;
        c->s = INVALID_SOCKET;
        c->su_seq = seq + 1;
        c->base = ((uint32_t)rand() << 8) + 1;  // el SYN ACK lleva base - 1
        c->ventana = g16(t + 14);
        c->t_envio = ahora_ms;
        WT("[wfc] TCP: la DS conecta con %u.%u.%u.%u:%u\n", ipd >> 24, (ipd >> 16) & 255, (ipd >> 8) & 255, ipd & 255, pd);
        if (ipd != IP_WFC) {  // internet de verdad
            c->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            struct sockaddr_in a = {0};
            a.sin_family = AF_INET; a.sin_port = htons(pd); a.sin_addr.s_addr = htonl(ipd);
#ifdef _WIN32
            u_long nb = 1; ioctlsocket(c->s, FIONBIO, &nb);
#else
            fcntl(c->s, F_SETFL, fcntl(c->s, F_GETFL, 0) | O_NONBLOCK);
#endif
            connect(c->s, (struct sockaddr *)&a, sizeof a);
        }
        tcp_segmento(c, c->base - 1, 0x12, NULL, 0);  // SYN ACK
        return;
    }
    if (!c) {  // nada que ver: RST
        Con x;
        memset(&x, 0, sizeof x);
        x.po = po; x.pd = pd; x.ipd = ipd; x.su_seq = seq + (uint32_t)nd;
        tcp_segmento(&x, ack, 0x14, NULL, 0);
        return;
    }
    if (fl & 0x04) { con_libera(c); return; }  // RST
    if (fl & 0x10) {
        c->ventana = g16(t + 14);
        if (!c->abierta && ack == c->base) {  // conexión hecha: ahora sí, el servicio (el saludo de gpcm, etc.)
            c->abierta = 1;
            if (c->ipd == IP_WFC) wfc_servicio_abre(c, c->pd);
        }
        int32_t avance = (int32_t)(ack - c->base);
        if (avance > 0 && avance <= c->enviados) {  // confirmado: fuera de sal
            memmove(c->sal, c->sal + avance, (size_t)(c->nsal - avance));
            c->nsal -= avance;
            c->enviados -= avance;
            c->base = ack;
            c->t_envio = ahora_ms;
            c->reenvios = 0;
        } else if (c->fin_enviado && avance == c->nsal + 1 && c->enviados == c->nsal) {  // todo y el FIN
            c->base += (uint32_t)c->nsal;
            c->nsal = c->enviados = 0;
            c->fin_ok = 1;
        }
    }
    if (nd > 0) {
        if (seq == c->su_seq) {
            c->su_seq += (uint32_t)nd;
            int cabe = (int)sizeof c->ent - c->nent;
            if (nd > cabe) nd = cabe;
            memcpy(c->ent + c->nent, d, (size_t)nd);
            c->nent += nd;
            if (c->s == INVALID_SOCKET) wfc_servicio_datos(c, c->pd, c->ent, &c->nent);
            // a internet se manda en wfc_atiende (el socket puede estar aún conectando)
        }
        tcp_segmento(c, siguiente_seq(c), 0x10, NULL, 0);  // ACK (también de lo repetido)
    }
    if ((fl & 0x01) && seq + (uint32_t)nd == c->su_seq) {  // FIN de la DS: se cierra también este lado
        if (!c->fin_ds) c->su_seq++;
        c->fin_ds = 1;
        c->cerrar = 1;
        tcp_segmento(c, siguiente_seq(c), 0x10, NULL, 0);
    }
    tcp_vacia(c);
    if (c->usada && c->fin_ok && c->fin_ds) con_libera(c);
}

// Reenvíos: lo mandado que la DS no confirma en 300 ms (o el SYN ACK) se vuelve a mandar
static void tcp_atiende(void) {
    for (int i = 0; i < NCON; i++) {
        Con *c = &cons[i];
        if (!c->usada || ahora_ms - c->t_envio < 300) continue;
        if (c->fin_ok && (c->fin_ds || ahora_ms - c->t_envio > 10000)) { con_libera(c); continue; }
        if (++c->reenvios > 40) { WT("[wfc] TCP: la DS no contesta en %u\n", c->pd); con_libera(c); continue; }
        if (!c->abierta) {
            tcp_segmento(c, c->base - 1, 0x12, NULL, 0);
            c->t_envio = ahora_ms;
        } else if (c->enviados > 0 || (c->fin_enviado && !c->fin_ok)) {
            WT("[wfc] TCP: reenvío al puerto %u (%d bytes)\n", c->pd, c->enviados);
            c->enviados = 0;
            c->fin_enviado = 0;
            tcp_vacia(c);
        } else c->reenvios = 0;
    }
}

// ---- UDP con internet (NAT): un socket del PC por cada puerto de la DS -----------------------------------------
typedef struct { int usada; uint16_t po; SOCKET s; uint64_t visto; } Nat;
#define NNAT 16
static Nat nats[NNAT];

static void udp(uint32_t ipo, uint32_t ipd, const uint8_t *u, int n) {
    (void)ipo;
    if (n < 8) return;
    uint16_t po = g16(u), pd = g16(u + 2);
    const uint8_t *d = u + 8;
    int nd = g16(u + 4) - 8;
    if (nd < 0 || nd > n - 8) nd = n - 8;
    if (pd == 67) { dhcp(d, nd); return; }
    if (ipd == IP_ROUTER && pd == 53) { dns(po, d, nd); return; }
    if (ipd == IP_WFC) { wfc_servicio_udp(po, pd, d, nd); return; }
    if (es_otra_ds(ipd)) { tunel_udp(ipd, po, pd, d, nd); return; }
    if (ipd == 0xFFFFFFFFu || (ipd >> 24) == 10) return;
    Nat *x = NULL;
    for (int i = 0; i < NNAT; i++) if (nats[i].usada && nats[i].po == po) x = &nats[i];
    if (!x) {
        for (int i = 0; i < NNAT && !x; i++) if (!nats[i].usada) x = &nats[i];
        if (!x) return;
        x->usada = 1; x->po = po;
        x->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
        u_long nb = 1; ioctlsocket(x->s, FIONBIO, &nb);
#else
        fcntl(x->s, F_SETFL, fcntl(x->s, F_GETFL, 0) | O_NONBLOCK);
#endif
    }
    x->visto = ahora_ms;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(pd); a.sin_addr.s_addr = htonl(ipd);
    sendto(x->s, (const char *)d, nd, 0, (struct sockaddr *)&a, sizeof a);
}

// ---- lo que manda la DS ---------------------------------------------------------------------------------------------
static void parchea_https(void);

static void inicia(void) {
    if (wfc_traza < 0) wfc_traza = getenv("MKDS_WIFI_LOG") != NULL;
    if (!wfc_ip_ds) {  // la IP de esta DS en la red virtual: al azar (que coincidan dos es casi imposible)
#ifdef _WIN32
        unsigned proceso = (unsigned)GetCurrentProcessId();
#else
        unsigned proceso = (unsigned)getpid();
#endif
        srand((unsigned)time(NULL) ^ (unsigned)ahora_ms * 2654435761u ^ proceso << 16);
        wfc_ip_ds = 0x0A000000u | (uint32_t)(1 + rand() % 254) << 16 | (uint32_t)(rand() & 255) << 8 | (uint32_t)(2 + rand() % 252);
        tunel_abre();
    }
}

void wfc_envia(const uint8_t *t, int n) {
    static int parcheado;
    inicia();
    if (!parcheado) { parcheado = 1; parchea_https(); }
    if (n < 8 || t[0] != 0xAA || t[1] != 0xAA || t[2] != 0x03) return;
    uint16_t tipo = g16(t + 6);
    const uint8_t *p = t + 8;
    n -= 8;
    if (tipo == 0x0806) { arp(p, n); return; }
    if (tipo != 0x0800 || n < 20 || (p[0] >> 4) != 4) return;
    int hl = (p[0] & 15) * 4, total = g16(p + 2);
    if (total > n) total = n;
    uint32_t ipo = g32(p + 12), ipd = g32(p + 16);
    if (p[9] == 17) udp(ipo, ipd, p + hl, total - hl);
    else if (p[9] == 6) tcp(ipo, ipd, p + hl, total - hl);
    else if (p[9] == 1 && total - hl >= 8 && p[hl] == 8) {  // ping: contesta el router
        uint8_t r[1500];
        int m = total - hl;
        memcpy(r, p + hl, (size_t)m);
        r[0] = 0; r[2] = r[3] = 0;
        p16(r + 2, cierra(suma(r, m, 0)));
        ip_a_la_ds(ipd, ipo, 1, r, m);
    }
}

// Cada poco: lo que llegue de internet por los sockets del PC
void wfc_atiende(uint64_t ms) {
    ahora_ms = ms;
    inicia();
    tunel_atiende();
    tcp_atiende();
    uint8_t b[1500];
    for (int i = 0; i < NCON; i++) {
        Con *c = &cons[i];
        if (!c->usada || c->s == INVALID_SOCKET) continue;
        if (c->nent > 0) {
            int n = send(c->s, (const char *)c->ent, c->nent, 0);
            if (n > 0) { memmove(c->ent, c->ent + n, (size_t)(c->nent - n)); c->nent -= n; }
        }
        for (int k = 0; k < 8 && c->nsal < 65536; k++) {
            int n = recv(c->s, (char *)b, sizeof b, 0);
            if (n > 0) { wfc_con_envia(c, b, n); continue; }
            if (n == 0) { wfc_con_cierra(c); closesocket(c->s); c->s = INVALID_SOCKET; }
            break;
        }
        tcp_vacia(c);
    }
    for (int i = 0; i < NNAT; i++) {
        Nat *x = &nats[i];
        if (!x->usada) continue;
        for (int k = 0; k < 8; k++) {
            struct sockaddr_in de;
            socklen_t l = sizeof de;
            int n = recvfrom(x->s, (char *)b, sizeof b, 0, (struct sockaddr *)&de, &l);
            if (n <= 0) break;
            wfc_udp_a_la_ds(ntohl(de.sin_addr.s_addr), ntohs(de.sin_port), x->po, b, n);
        }
        if (ms - x->visto > 120000) { closesocket(x->s); x->usada = 0; }
    }
    wfc_servicio_atiende(ms);
}

// ---- el túnel: lo que una DS manda a otra (10.x.y.z) va directo al PC de ese jugador --------------------------------
// Un socket UDP propio. Su dirección pública (STUN) y la de la red de casa se apuntan en el servidor de Cloudflare
// junto con la IP virtual de la DS (wfc_servicios.c), y así cada juego sabe dónde está cada DS. Los dos lados se mandan
// HOLA unas cuantas veces ("hole punching": los routers dejan pasar lo que llega de vuelta) y después los paquetes:
//   "DSKW" 1 vip(4)                                       HOLA
//   "DSKW" 2 vip_origen(4) puerto(2) vip_destino(4) puerto(2) datos   un paquete UDP de una DS a otra
// Como en las salas, con algunos routers (CG-NAT, datos móviles) no se puede.
enum { T_HOLA = 1, T_DATOS = 2, T_PING = 3, T_PONG = 4 };  // PING/PONG: "DSKW" tipo vip(4) hora(4), para el ping
typedef struct {
    uint32_t vip;
    struct sockaddr_in pub, loc, buena;  // la pública, la de casa y por la que ha llegado algo de verdad
    int hay_pub, hay_loc, hay_buena, holas;
    uint64_t visto, ultimo_hola, ultimo_ping, ultimo_pong;
    int ping;                            // ms (media), con lo que tarda en volver un PING
} Par;
#define NPARES 64
static Par pares[NPARES];
static SOCKET tunel = INVALID_SOCKET;
static uint16_t tunel_puerto;
static volatile int stun_ok;
static volatile uint32_t stun_ip;
static volatile uint16_t stun_puerto;
static uint8_t stun_id[12];
uint32_t wifi_ip_local(void);  // wifi.c

static void tunel_abre(void) {
    if (tunel != INVALID_SOCKET) return;
#ifdef _WIN32
    WSADATA wsa;  // puede que aún no lo haya hecho nadie (wifi.c lo hace al abrir el modo local)
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    tunel = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(tunel, (struct sockaddr *)&a, sizeof a);
    socklen_t l = sizeof a;
    getsockname(tunel, (struct sockaddr *)&a, &l);
    tunel_puerto = ntohs(a.sin_port);
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(tunel, FIONBIO, &nb);
    int no = 0;  // que un "puerto inalcanzable" de otro no cierre el socket (WSAECONNRESET)
    DWORD x;
    WSAIoctl(tunel, _WSAIOW(IOC_VENDOR, 12), &no, sizeof no, NULL, 0, &x, NULL, NULL);
#else
    fcntl(tunel, F_SETFL, fcntl(tunel, F_GETFL, 0) | O_NONBLOCK);
#endif
    WT("[wfc] túnel en el puerto %u; esta DS es %u.%u.%u.%u\n", tunel_puerto, IP_DS >> 24, (IP_DS >> 16) & 255,
       (IP_DS >> 8) & 255, IP_DS & 255);
}

static Par *par_de(uint32_t vip, int crea) {
    Par *libre = NULL, *viejo = &pares[0];
    for (int i = 0; i < NPARES; i++) {
        if (pares[i].vip == vip) return &pares[i];
        if (!pares[i].vip && !libre) libre = &pares[i];
        if (pares[i].visto < viejo->visto) viejo = &pares[i];
    }
    if (!crea) return NULL;
    Par *p = libre ? libre : viejo;
    memset(p, 0, sizeof *p);
    p->vip = vip;
    p->visto = ahora_ms;
    return p;
}

static void pon_dir(struct sockaddr_in *a, uint32_t ip, uint16_t puerto) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(ip);
    a->sin_port = htons(puerto);
}

// desde el hilo del servidor no: lo llama wfc_servicio_atiende, en el del juego
void wfc_tunel_pon(uint32_t vip, uint32_t ip, uint16_t puerto, uint32_t ipl, uint16_t puertol) {
    if (!es_otra_ds(vip)) return;
    Par *p = par_de(vip, 1);
    p->hay_pub = ip && puerto;
    if (p->hay_pub) pon_dir(&p->pub, ip, puerto);
    p->hay_loc = ipl && puertol;
    if (p->hay_loc) pon_dir(&p->loc, ipl, puertol);
    if (!p->hay_buena && p->holas < 10) p->holas = 10;  // a abrir camino
}

static void tunel_manda(Par *p, const uint8_t *b, int n) {
    if (tunel == INVALID_SOCKET) return;
    if (p->hay_buena) { sendto(tunel, (const char *)b, n, 0, (struct sockaddr *)&p->buena, sizeof p->buena); return; }
    if (p->hay_pub) sendto(tunel, (const char *)b, n, 0, (struct sockaddr *)&p->pub, sizeof p->pub);
    if (p->hay_loc) sendto(tunel, (const char *)b, n, 0, (struct sockaddr *)&p->loc, sizeof p->loc);
}

static void tunel_hola(Par *p) {
    uint8_t b[9] = {'D', 'S', 'K', 'W', T_HOLA};
    p32(b + 5, IP_DS);
    tunel_manda(p, b, 9);
    p->ultimo_hola = ahora_ms;
}

static unsigned t_env, t_rec, t_env_b, t_rec_b;  // lo que pasa por el túnel (para MKDS_WIFI_LOG)
static int t_env_max, t_rec_max;

static void tunel_udp(uint32_t ipd, uint16_t po, uint16_t pd, const uint8_t *d, int n) {
    Par *p = par_de(ipd, 0);
    if (!p || (!p->hay_buena && !p->hay_pub && !p->hay_loc)) {
        WT("[wfc] túnel: no sé dónde está %u.%u.%u.%u\n", ipd >> 24, (ipd >> 16) & 255, (ipd >> 8) & 255, ipd & 255);
        return;
    }
    uint8_t b[1600];
    if (n > 1500) return;
    memcpy(b, "DSKW", 4);
    b[4] = T_DATOS;
    p32(b + 5, IP_DS); p16(b + 9, po); p32(b + 11, ipd); p16(b + 15, pd);
    memcpy(b + 17, d, (size_t)n);
    tunel_manda(p, b, 17 + n);
    t_env++;
    t_env_b += (unsigned)n;
    if (n > t_env_max) t_env_max = n;
    static unsigned enviados;
    if (enviados++ < 40 || !(enviados & 255)) {
        WT("[wfc] túnel -> %u.%u.%u.%u: %u -> %u, %d bytes (%u en total)%s:", ipd >> 24, (ipd >> 16) & 255,
           (ipd >> 8) & 255, ipd & 255, po, pd, n, enviados, p->hay_buena ? "" : " (aún sin camino)");
        for (int i = 0; i < n && i < 80; i++) WT(" %02X", d[i]);
        WT("\n");
    }
}

static int stun_llega(const uint8_t *b, int n) {  // 1 si era una respuesta de STUN (y la apunta)
    if (n < 20 || b[0] != 0x01 || b[1] != 0x01 || memcmp(b + 4, "\x21\x12\xA4\x42", 4) || memcmp(b + 8, stun_id, 12))
        return 0;
    for (int o = 20; o + 4 <= n;) {
        int tipo = b[o] << 8 | b[o + 1], largo = b[o + 2] << 8 | b[o + 3];
        if ((tipo == 0x0020 || tipo == 0x0001) && largo >= 8 && o + 4 + largo <= n && b[o + 5] == 1) {
            uint16_t puerto = g16(b + o + 6);
            uint32_t ip = g32(b + o + 8);
            if (tipo == 0x0020) { puerto ^= 0x2112; ip ^= 0x2112A442; }  // XOR-MAPPED-ADDRESS
            stun_ip = ip;
            stun_puerto = puerto;
            stun_ok = 1;
            return 1;
        }
        o += 4 + ((largo + 3) & ~3);
    }
    return 1;
}

// Las direcciones de este juego (STUN). Se llama desde el hilo del servidor (wfc_servicios.c): puede tardar.
int wfc_tunel_direcciones(uint32_t *ip, uint16_t *puerto, uint32_t *ipl, uint16_t *puertol) {
    for (int t = 0; t < 100 && tunel == INVALID_SOCKET; t++) Sleep(20);
    if (tunel == INVALID_SOCKET) return 0;
    if (!stun_ok) {
        struct addrinfo pista = {0}, *res;
        pista.ai_family = AF_INET;
        pista.ai_socktype = SOCK_DGRAM;
        const char *servidores[] = {"stun.l.google.com", "stun.cloudflare.com"};
        const char *puertos[] = {"19302", "3478"};
        for (int s = 0; s < 2 && !stun_ok; s++) {
            if (getaddrinfo(servidores[s], puertos[s], &pista, &res)) continue;
            for (int intento = 0; intento < 3 && !stun_ok; intento++) {
                uint8_t p[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
                for (int i = 0; i < 12; i++) stun_id[i] = p[8 + i] = (uint8_t)(rand() ^ (intento * 31 + i));
                sendto(tunel, (const char *)p, 20, 0, res->ai_addr, (int)res->ai_addrlen);
                for (int t = 0; t < 20 && !stun_ok; t++) Sleep(50);  // la respuesta la lee tunel_atiende
            }
            freeaddrinfo(res);
        }
    }
    *ip = stun_ok ? stun_ip : 0;
    *puerto = stun_ok ? stun_puerto : 0;
    *ipl = wifi_ip_local();
    *puertol = tunel_puerto;
    WT("[wfc] túnel: %s %u.%u.%u.%u:%u\n", stun_ok ? "STUN dice" : "STUN no contesta;", *ip >> 24, (*ip >> 16) & 255,
       (*ip >> 8) & 255, *ip & 255, *puerto);
    return stun_ok;
}

int wfc_ping_ms(void);
static void tunel_atiende(void) {
    if (tunel == INVALID_SOCKET) return;
    uint8_t b[1600];
    for (int k = 0; k < 64; k++) {
        struct sockaddr_in de;
        socklen_t l = sizeof de;
        int n = recvfrom(tunel, (char *)b, sizeof b, 0, (struct sockaddr *)&de, &l);
        if (n <= 0) break;
        if (stun_llega(b, n) || n < 9 || memcmp(b, "DSKW", 4)) continue;
        uint32_t vip = g32(b + 5);
        if (!es_otra_ds(vip)) continue;
        Par *p = par_de(vip, 1);
        if (!p->hay_buena) WT("[wfc] túnel: %u.%u.%u.%u está en %s:%u\n", vip >> 24, (vip >> 16) & 255, (vip >> 8) & 255,
                              vip & 255, inet_ntoa(de.sin_addr), ntohs(de.sin_port));
        p->buena = de;
        p->hay_buena = 1;
        p->visto = ahora_ms;
        if (b[4] == T_HOLA) { if (ahora_ms - p->ultimo_hola > 300) tunel_hola(p); }  // que el otro también sepa
        else if (b[4] == T_PING && n >= 13) {  // se devuelve tal cual (con mi vip)
            b[4] = T_PONG;
            p32(b + 5, IP_DS);
            sendto(tunel, (const char *)b, 13, 0, (struct sockaddr *)&de, sizeof de);
        } else if (b[4] == T_PONG && n >= 13) {
            int rtt = (int)((uint32_t)ahora_ms - g32(b + 9));
            if (rtt >= 0 && rtt < 10000) p->ping = p->ultimo_pong && p->ping ? (p->ping * 3 + rtt) / 4 : rtt;
            p->ultimo_pong = ahora_ms;
        }
        else if (b[4] == T_DATOS && n >= 17 && g32(b + 11) == IP_DS) {
            t_rec++;
            t_rec_b += (unsigned)(n - 17);
            if (n - 17 > t_rec_max) t_rec_max = n - 17;
            static unsigned recibidos;
            if (recibidos++ < 40 || !(recibidos & 255))
                WT("[wfc] túnel <- %u.%u.%u.%u: %u -> %u, %d bytes (%u en total)\n", vip >> 24, (vip >> 16) & 255,
                   (vip >> 8) & 255, vip & 255, g16(b + 9), g16(b + 15), n - 17, recibidos);
            wfc_udp_a_la_ds(vip, g16(b + 9), g16(b + 15), b + 17, n - 17);
        }
    }
    static uint64_t t_ultimo;
    if (ahora_ms - t_ultimo >= 2000) {  // cada 2 s: cuánto ha pasado por el túnel
        if (t_env || t_rec)
            WT("[wfc] túnel en 2 s: %u enviados (%u bytes, el mayor %d), %u recibidos (%u bytes, el mayor %d), ping %d (%llu ms)\n",
               t_env, t_env_b, t_env_max, t_rec, t_rec_b, t_rec_max, wfc_ping_ms(), (unsigned long long)ahora_ms);
        t_env = t_rec = t_env_b = t_rec_b = 0;
        t_env_max = t_rec_max = 0;
        t_ultimo = ahora_ms;
    }
    for (int i = 0; i < NPARES; i++) {  // abrir camino, y mantenerlo abierto mientras se juega
        Par *p = &pares[i];
        if (!p->vip) continue;
        if (p->hay_buena && ahora_ms - p->visto < 10000 && ahora_ms - p->ultimo_ping >= 1000) {  // el ping, cada segundo
            uint8_t q[13] = {'D', 'S', 'K', 'W', T_PING};
            p32(q + 5, IP_DS);
            p32(q + 9, (uint32_t)ahora_ms);
            sendto(tunel, (const char *)q, 13, 0, (struct sockaddr *)&p->buena, sizeof p->buena);
            p->ultimo_ping = ahora_ms;
        }
        if (p->holas > 0 && ahora_ms - p->ultimo_hola >= 250) { tunel_hola(p); p->holas--; }
        else if (p->hay_buena && ahora_ms - p->visto < 60000 && ahora_ms - p->ultimo_hola >= 5000) tunel_hola(p);
    }
}

// El WFC de Nintendo usaba https para iniciar sesión (nas.nintendowifi.net/ac), con certificados de Nintendo que
// no se pueden imitar. La DS también sabe hablar http normal: se cambia la dirección en su memoria (como hacen los
// parches de Wiimmfi para DS).
static void parchea_https(void) {
    static const char viejo[] = "https://nas.nintendowifi.net";
    for (uint32_t a = 0x180000; a + sizeof viejo < 0x200000; a++) {
        if (memcmp(g_main + a, viejo, sizeof viejo - 1)) continue;
        size_t l = strlen((char *)g_main + a);
        memmove(g_main + a + 4, g_main + a + 5, l - 4);  // "https" -> "http"
        WT("[wfc] parcheado https -> http en %08X\n", 0x02000000 + a);
    }
}

// El ping con los rivales del online (el peor de los que han contestado hace poco), o -1 si no se está jugando online.
// Lo lee el contador de FPS del menú (menu.cpp), desde otro hilo: solo lee enteros.
int wfc_ping_ms(void) {
    int r = -1;
    for (int i = 0; i < NPARES; i++)
        if (pares[i].vip && pares[i].ultimo_pong && ahora_ms - pares[i].ultimo_pong < 5000 && pares[i].ping > r) r = pares[i].ping;
    return r;
}
