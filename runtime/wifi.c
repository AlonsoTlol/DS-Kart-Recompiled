// Wifi de la DS (librería WM del NitroSDK) contestado por el port en vez del ARM7.
//
// El ARM7 de verdad mueve el chip wifi (registros 0x04800000), que el port no tiene: al entrar en Multijugador
// contestaba "fallo" a la primera orden (INITIALIZE, wlCmd 0x0304) y el juego reintentaba sin fin dentro de la
// interrupción hasta desbordar la pila. Aquí se hacen las órdenes nosotros, como con el micrófono, y los datos
// irán por UDP (modo Local por Radmin).
//
// Cómo habla el ARM9 (visto en su código, 02142AB8 y 021425E8):
//   - Cada orden es un búfer de 0x100 bytes en la memoria principal: u16 apiid en +0 y los parámetros en +4
//     (o los que ponga la función de esa orden). El ARM9 manda su dirección por la FIFO (servicio 10) y lo
//     vuelve a usar 10 órdenes después: hay que marcarlo como leído con el bit 0x8000 del apiid.
//   - La respuesta es otro búfer en la memoria principal: u16 apiid, u16 código de error (0 = bien,
//     1 = fallo, 2 = en marcha, 3 = estado no válido...) y lo que lleve esa orden. Se le manda la dirección por
//     la FIFO; el ARM9 llama a la función que el juego dejó para ese apiid y, si el búfer no es el suyo de
//     respuestas, lo devuelve poniendo el bit 0x8000.
//   - El estado compartido (WMStatus) lo escribe el ARM7: u16 state en +0 (0 READY, 1 STOP, 2 IDLE, 3 CLASS1,
//     5 SCAN, 6 CONNECT, 7 PARENT, 8 CHILD, 9 MP_PARENT, 10 MP_CHILD).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
// en Linux: sockets de siempre (BSD), con los nombres de Winsock que usa este archivo
#include "compat.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define FIONBIO 0
typedef unsigned long u_long;
static int ioctlsocket(SOCKET s, long orden, u_long *v) {
    (void)orden;
    int f = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, *v ? f | O_NONBLOCK : f & ~O_NONBLOCK);
}
#define GetCurrentProcessId getpid
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t g_main[0x400000];
void fifo_a_arm9(uint32_t v);

static uint16_t lee16(uint32_t a) { uint16_t v; memcpy(&v, g_main + (a & 0x3FFFFF), 2); return v; }
static uint32_t lee32(uint32_t a) { uint32_t v; memcpy(&v, g_main + (a & 0x3FFFFF), 4); return v; }
static void pon16(uint32_t a, uint16_t v) { memcpy(g_main + (a & 0x3FFFFF), &v, 2); }
static void pon32(uint32_t a, uint32_t v) { memcpy(g_main + (a & 0x3FFFFF), &v, 4); }

static uint32_t buf7, estado, respuestas;  // los tres búferes que da INITIALIZE
static int siguiente;
static int traza = -1;

enum { READY = 0, STOP = 1, IDLE = 2, SCAN = 5, CONNECT = 6, PARENT = 7, CHILD = 8, MP_PARENT = 9, MP_CHILD = 10 };
// lo que va en +8 de las respuestas de búsqueda y de anfitrión (WMStateCode)
enum { PARENT_START = 0, PARENT_NOT_FOUND = 4, PARENT_FOUND = 5 };

// respuestas que tienen que esperar (buscar dura lo que pide el juego en cada canal)
static struct { uint64_t cuando; uint32_t buf; } espera[8];
static uint64_t ms_ahora;

static void responde_luego(uint32_t b, unsigned ms) {
    for (int i = 0; i < 8; i++)
        if (!espera[i].buf) { espera[i].cuando = ms_ahora + ms; espera[i].buf = b; return; }
    fifo_a_arm9((b << 6) | 10);  // no cabe: ya
}

static void red_atiende(void);

// Cada poco desde ds.c (actualiza), con el tiempo de la DS en milisegundos
void wifi_actualiza(uint64_t ms) {
    if (ms != ms_ahora) { ms_ahora = ms; red_atiende(); }
    for (int i = 0; i < 8; i++)
        if (espera[i].buf && ms >= espera[i].cuando) {
            fifo_a_arm9((espera[i].buf << 6) | 10);
            espera[i].buf = 0;
        }
}

static uint32_t param_padre;  // WMParentParam del anfitrión (SET_P_PARAM)
static uint16_t canal = 1;

static const char *nombre(int api) {
    static const char *n[] = {"INITIALIZE", "RESET", "END", "ENABLE", "DISABLE", "POWER_ON", "POWER_OFF",
        "SET_P_PARAM", "START_PARENT", "END_PARENT", "START_SCAN", "END_SCAN", "START_CONNECT", "DISCONNECT",
        "START_MP", "SET_MP_DATA", "END_MP", "START_DCF", "SET_DCF_DATA", "END_DCF", "SET_WEPKEY", "START_KS",
        "END_KS", "GET_KEYSET", "SET_GAMEINFO", "SET_BEACON_IND", "START_TESTMODE", "STOP_TESTMODE",
        "VALARM_MP", "SET_LIFETIME", "MEASURE_CHANNEL", "INIT_W_COUNTER", "GET_W_COUNTER", "SET_ENTRY",
        "AUTO_DEAUTH", "SET_MP_PARAMETER", "SET_BEACON_PERIOD", "AUTO_DISCONNECT", "START_SCAN_EX",
        "SET_WEPKEY_EX", "SET_PS_MODE", "START_TESTRXMODE", "STOP_TESTRXMODE"};
    return api < (int)(sizeof n / sizeof *n) ? n[api] : "?";
}

// Un búfer libre para una respuesta: los tres de 0x100 del búfer del "ARM7" (nadie más los usa), por turnos.
// Antes de INITIALIZE solo existe el de respuestas del ARM9.
// El ARM9 devuelve cada búfer poniendo el bit 0x8000 del apiid: solo se reusan los devueltos.
// Los búferes 0 y 1 son los preferidos para entregar datos recibidos (entrega_datos); las órdenes usan antes el 2.
static uint32_t bufer_respuesta(void) {
    if (!buf7) return respuestas;
    static const int orden[3] = {2, 0, 1};
    for (int k = 0; k < 3; k++) {
        uint32_t b = buf7 + 0x100u * (uint32_t)orden[k];
        if (lee16(b) & 0x8000) {
            memset(g_main + (b & 0x3FFFFF), 0, 0x100);
            return b;
        }
    }
    memset(g_main + (respuestas & 0x3FFFFF), 0, 0x100);  // todos ocupados: el del ARM9
    return respuestas;
}

static void responde(uint32_t b) { fifo_a_arm9((b << 6) | 10); }

// ---- la red (modo Local por Radmin o en la misma red) ---------------------------------------
// Cada juego abre dos sockets UDP: uno en el puerto común 28470 (MKDS_LOCAL_PUERTO), compartido, donde se oyen
// los anuncios, y otro propio (puerto libre) para hablar con cada uno. Mensajes: "DSKR", tipo y datos.
//   1 ANUNCIO (anfitrión, cada 300 ms, a la difusión de cada red y a LOCAL_IP): puerto propio, MAC,
//     WMParentParam (0x40) y userGameInfo.
//   2 UNIRSE (invitado → anfitrión): MAC y los 24 bytes que da el juego en START_CONNECT.
//   3 ACEPTADO (anfitrión → invitado): aid.
//   4 ADIOS (cualquiera): se va.
//   5 DATOS (intercambio MP): puerto, aid de quien lo manda, largo y los datos.
enum { ANUNCIO = 1, UNIRSE = 2, ACEPTADO = 3, ADIOS = 4, DATOS = 5 };
static SOCKET comun = INVALID_SOCKET, propio = INVALID_SOCKET;
static uint16_t puerto_comun = 28470, puerto_propio;
static uint8_t mi_mac[6];
static int red_lista;

typedef struct {
    struct sockaddr_in dir;  // su socket propio
    uint8_t mac[6];
    uint8_t param[0x40];
    uint8_t info[112];
    int info_largo;
    uint64_t visto;
} Grupo;
static Grupo grupos[4];

typedef struct { int usado; struct sockaddr_in dir; uint8_t mac[6]; } Invitado;
static Invitado invitados[16];  // por aid (1-15)

static struct { int esperando; uint32_t bss; uint8_t ssid[24]; struct sockaddr_in dir; uint64_t desde; } conecta;
static struct sockaddr_in mi_anfitrion;

// ---- intercambio MP -----------------------------------------------------------------------
// Campos del WMStatus que mira el ARM9 antes de enviar (WM_SetMPDataToPortEx, 02144048): +0x184 aid (0 =
// anfitrión), +0x17E invitados conectados (bit por aid), +0x18E tamaño máximo de envío (datos + 4 el
// anfitrión, + 2 el invitado), +0xC2 = 1 en el invitado para poder empezar MP.
// +0x86: invitados listos para el intercambio; los datos compartidos del anfitrión (02144D34) solo van a ellos.
enum { ST_AID = 0x184, ST_HIJOS = 0x17E, ST_MAX_ENVIO = 0x18E, ST_LISTO_HIJO = 0xC2, ST_LISTOS = 0x86 };
static uint32_t mp_recibe, mp_recibe_tam;  // búfer de recepción que dio START_MP
static uint16_t max_hijo = 0x36;           // tamaño de datos del invitado (del grupo al que se une)
typedef struct { uint16_t puerto, largo, aid; uint8_t mac[6]; uint8_t datos[512]; } Paquete;
static Paquete cola[32];
static int cola_ini, cola_fin;

static void pon_hijos(void) {
    uint16_t m = 0;
    for (int i = 1; i < 16; i++) if (invitados[i].usado) m |= (uint16_t)(1u << i);
    if (estado) { pon16(estado + ST_HIJOS, m); pon16(estado + ST_LISTOS, (uint16_t)(m | 1)); }
}

// Entrega al juego un paquete recibido como aviso de puerto (apiid 0x82, estado 0x15): +6 puerto, +8 búfer de
// recepción, +0C datos, +10 largo, +12 aid de quien lo manda, +14 su MAC, +20 mi aid. Cada uno usa uno de los
// búferes 0 y 1 del "ARM7" y la mitad correspondiente del búfer de recepción; si están ocupados, espera.
static void entrega_datos(void) {
    while (cola_ini != cola_fin && buf7 && mp_recibe) {
        int k = -1;
        for (int i = 0; i < 2 && k < 0; i++) if (lee16(buf7 + 0x100u * (uint32_t)i) & 0x8000) k = i;
        if (k < 0) return;
        Paquete *q = &cola[cola_ini];
        uint32_t mitad = mp_recibe_tam / 2 & ~3u;
        uint32_t zona = mp_recibe + mitad * (uint32_t)k;
        uint16_t largo = q->largo;
        if (largo > mitad) largo = (uint16_t)mitad;
        memcpy(g_main + (zona & 0x3FFFFF), q->datos, largo);
        uint32_t c = buf7 + 0x100u * (uint32_t)k;
        memset(g_main + (c & 0x3FFFFF), 0, 0x100);
        pon16(c, 0x82);
        pon16(c + 4, 0x15);
        pon16(c + 6, q->puerto);
        pon32(c + 8, zona);
        pon32(c + 0xC, zona);
        pon16(c + 0x10, largo);
        pon16(c + 0x12, q->aid);
        memcpy(g_main + ((c + 0x14) & 0x3FFFFF), q->mac, 6);
        pon16(c + 0x20, (uint16_t)(estado ? lee16(estado + ST_AID) : 0));
        responde(c);
        cola_ini = (cola_ini + 1) % 32;
    }
}
static int mi_aid;

void wifi_mac(uint8_t *m) {  // la MAC de esta consola (distinta en cada PC y cada vez): ds.c la pone en el firmware
    static int hecha;
    if (!hecha) {
        hecha = 1;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        uint32_t x = (uint32_t)t.QuadPart ^ (GetCurrentProcessId() << 16) ^ GetTickCount();
        x ^= x >> 13; x *= 0x5BD1E995u; x ^= x >> 15;
        mi_mac[0] = 0x00; mi_mac[1] = 0x09; mi_mac[2] = 0xBF;
        mi_mac[3] = (uint8_t)x; mi_mac[4] = (uint8_t)(x >> 8); mi_mac[5] = (uint8_t)(x >> 16);
    }
    memcpy(m, mi_mac, 6);
}

static void red_abre(void) {
    if (red_lista) return;
    red_lista = 1;
    uint8_t m[6];
    wifi_mac(m);
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) return;
#endif
    const char *p = getenv("MKDS_LOCAL_PUERTO");
    if (p && atoi(p) > 0) puerto_comun = (uint16_t)atoi(p);
    BOOL si = TRUE;
    u_long nobloquea = 1;
    comun = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    setsockopt(comun, SOL_SOCKET, SO_REUSEADDR, (char *)&si, sizeof si);
    setsockopt(comun, SOL_SOCKET, SO_BROADCAST, (char *)&si, sizeof si);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(puerto_comun);
    if (bind(comun, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "[wifi] no se pudo abrir el puerto %u\n", puerto_comun);
        closesocket(comun); comun = INVALID_SOCKET;
    } else ioctlsocket(comun, FIONBIO, &nobloquea);
    propio = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    setsockopt(propio, SOL_SOCKET, SO_BROADCAST, (char *)&si, sizeof si);
    a.sin_port = 0;
    bind(propio, (struct sockaddr *)&a, sizeof a);
    int l = sizeof a;
    getsockname(propio, (struct sockaddr *)&a, &l);
    puerto_propio = ntohs(a.sin_port);
    ioctlsocket(propio, FIONBIO, &nobloquea);
    fprintf(stderr, "[wifi] red: puerto común %u, propio %u, MAC %02X:%02X:%02X:%02X:%02X:%02X\n", puerto_comun,
            puerto_propio, m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int cabecera(uint8_t *b, int tipo) {
    memcpy(b, "DSKR", 4);
    b[4] = (uint8_t)tipo;
    b[5] = 1;  // versión
    memcpy(b + 6, &puerto_propio, 2);
    memcpy(b + 8, mi_mac, 6);
    return 14;
}

// MKDS_LOCAL_RETRASO=ms: retrasa lo que se envía (para probar en un solo PC cómo va con la latencia de internet)
static struct { uint64_t cuando; struct sockaddr_in d; int n; uint8_t b[600]; } retrasados[64];
static int retraso_ms = -1;

static void envia(const struct sockaddr_in *d, const uint8_t *b, int n) {
    if (propio == INVALID_SOCKET) return;
    if (n > 600) { sendto(propio, (const char *)b, n, 0, (const struct sockaddr *)d, sizeof *d); return; }
    if (retraso_ms < 0) retraso_ms = getenv("MKDS_LOCAL_RETRASO") ? atoi(getenv("MKDS_LOCAL_RETRASO")) : 0;
    if (retraso_ms > 0 && n <= 600)
        for (int i = 0; i < 64; i++)
            if (!retrasados[i].n) {
                retrasados[i].cuando = ms_ahora + (uint64_t)retraso_ms;
                retrasados[i].d = *d;
                retrasados[i].n = n;
                memcpy(retrasados[i].b, b, (size_t)n);
                return;
            }
    sendto(propio, (const char *)b, n, 0, (const struct sockaddr *)d, sizeof *d);
}

static void envia_retrasados(void) {
    for (int i = 0; i < 64; i++)
        if (retrasados[i].n && ms_ahora >= retrasados[i].cuando) {
            sendto(propio, (const char *)retrasados[i].b, retrasados[i].n, 0, (const struct sockaddr *)&retrasados[i].d,
                   sizeof retrasados[i].d);
            retrasados[i].n = 0;
        }
}

// A todas las redes del PC (la de casa, la de Radmin 26.x.x.x...) por difusión, y a LOCAL_IP si se puso
static void envia_a_todos(const uint8_t *b, int n) {
    static uint32_t difusiones[16];
    static int nd = -1;
    static uint64_t mirado;
    if (nd < 0 || ms_ahora - mirado > 10000) {  // las redes pueden cambiar (Radmin se conecta después)
        mirado = ms_ahora;
        nd = 0;
#ifndef _WIN32
        struct ifaddrs *lista;
        if (!getifaddrs(&lista)) {
            for (struct ifaddrs *x = lista; x && nd < 16; x = x->ifa_next) {
                if (!x->ifa_addr || x->ifa_addr->sa_family != AF_INET || !x->ifa_netmask) continue;
                if (!(x->ifa_flags & IFF_UP) || (x->ifa_flags & IFF_LOOPBACK)) continue;
                uint32_t ip = ntohl(((struct sockaddr_in *)x->ifa_addr)->sin_addr.s_addr);
                uint32_t mascara = ntohl(((struct sockaddr_in *)x->ifa_netmask)->sin_addr.s_addr);
                if (!mascara || mascara == 0xFFFFFFFFu) continue;
                difusiones[nd++] = ip | ~mascara;
            }
            freeifaddrs(lista);
        }
#else
        ULONG tam = 16384;
        IP_ADAPTER_ADDRESSES *ad = malloc(tam);
        if (ad && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                       NULL, ad, &tam) == NO_ERROR) {
            for (IP_ADAPTER_ADDRESSES *x = ad; x && nd < 16; x = x->Next) {
                if (x->OperStatus != IfOperStatusUp) continue;
                for (IP_ADAPTER_UNICAST_ADDRESS *u = x->FirstUnicastAddress; u && nd < 16; u = u->Next) {
                    struct sockaddr_in *s = (struct sockaddr_in *)u->Address.lpSockaddr;
                    uint32_t ip = ntohl(s->sin_addr.s_addr);
                    int pref = u->OnLinkPrefixLength;
                    if (pref <= 0 || pref >= 32) continue;
                    difusiones[nd++] = ip | (0xFFFFFFFFu >> pref);
                }
            }
        }
        free(ad);
#endif
    }
    struct sockaddr_in d = {0};
    d.sin_family = AF_INET;
    d.sin_port = htons(puerto_comun);
    for (int i = 0; i < nd; i++) {
        d.sin_addr.s_addr = htonl(difusiones[i]);
        envia(&d, b, n);
    }
    d.sin_addr.s_addr = htonl(0x7F000001);  // este mismo PC (dos copias para probar)
    envia(&d, b, n);
    const char *ip = getenv("MKDS_LOCAL_IP");  // LOCAL_IP en ajustes.ini: la IP del amigo (p. ej. la de Radmin)
    if (ip && *ip && inet_pton(AF_INET, ip, &d.sin_addr) == 1) envia(&d, b, n);
}

static void anuncia(void) {
    uint8_t b[256];
    int n = cabecera(b, ANUNCIO);
    memcpy(b + n, g_main + (param_padre & 0x3FFFFF), 0x40);
    n += 0x40;
    uint32_t info;
    memcpy(&info, b + 14, 4);
    int largo = b[14 + 4] | b[14 + 5] << 8;
    if (largo > 112) largo = 112;
    b[n++] = (uint8_t)largo;
    if ((info & 0xFF000000) == 0x02000000) memcpy(b + n, g_main + (info & 0x3FFFFF), (size_t)largo);
    n += largo;
    envia_a_todos(b, n);
}

static void responde_simple(int api, int error);
static void pon_estado(int s);

// ---- intercambio de cada fotograma en carrera (DataSharing) con adelanto --------------------------------
// En carrera el juego usa el DataSharing del SDK (puerto 12): cada fotograma cada consola pone sus 8 bytes
// (los botones) con WM_StepDataSharing (02144C6C) y recibe el conjunto con los de todas para ese paso; todas
// simulan la carrera entera con los mismos conjuntos. El del SDK hace un viaje de ida y vuelta por paso: en una
// DS de verdad tarda microsegundos, pero por internet (Radmin) con 15 ms de ida el juego iba al 67 % (cámara
// lenta) y con 20 ms ni salía del vestíbulo.
// Aquí se sustituye: cada consola pone sus datos con 'adelanto' pasos de ventaja (los del paso t los manda
// cuando el juego va por el t - adelanto), así que llegan antes de que hagan falta. El anfitrión junta los de
// todos, cierra cada conjunto una sola vez y se lo manda a los invitados: todos leen exactamente lo mismo.
// UDP puede perder paquetes: cada mensaje lleva todo lo que el otro aún no ha confirmado. Si aun así hay que
// esperar a menudo, el adelanto sube solo (cada paso de adelanto son 16,7 ms más de retraso en los mandos).
//   6 DS_HIJO (invitado → anfitrión): sesión, siguiente conjunto que necesita, primer paso, cuántos, sus datos.
//   7 DS_PADRE (anfitrión → invitado): sesión, siguiente paso de sus datos que le falta, primer conjunto,
//     cuántos y cada conjunto (de quién trae datos + los datos de cada jugador del aidBitmap).
enum { DS_HIJO = 6, DS_PADRE = 7 };
#define DS_PASOS 64
#define DS_MAX 64
static struct {
    uint32_t info;              // WMDataSharingInfo del juego (0 = sin carrera)
    int aid, largo, iniciado;
    uint16_t bitmap;            // aidBitmap del juego
    uint32_t leidos, hechos;    // conjuntos que ha leído el juego; pasos con mis datos ya puestos
    uint32_t cierre;            // anfitrión: siguiente conjunto por cerrar
    int adelanto, atascos, buenos;
    uint32_t sesion;
    uint32_t paso[DS_PASOS];    // qué paso guarda cada hueco del anillo (+1; 0 = vacío)
    uint16_t recibido[DS_PASOS], cerrado_con[DS_PASOS];
    uint8_t completo[DS_PASOS];
    uint8_t datos[DS_PASOS][16][DS_MAX];
    uint32_t hijo_lee[16], hijo_hecho[16];  // anfitrión: lo que necesita cada invitado y lo que ya tengo suyo
    uint32_t padre_tiene;                   // invitado: siguiente paso mío que le falta al anfitrión
} dsh;

static int ds_hueco(uint32_t t) {
    int k = (int)(t % DS_PASOS);
    if (dsh.paso[k] != t + 1) {
        dsh.paso[k] = t + 1;
        dsh.recibido[k] = dsh.cerrado_con[k] = 0;
        dsh.completo[k] = 0;
    }
    return k;
}

// lo más antiguo que alguien puede necesitar todavía (lo de antes se puede pisar)
static uint32_t ds_base(void) {
    uint32_t b = dsh.leidos;
    if (dsh.aid == 0)
        for (int i = 1; i < 16; i++)
            if (invitados[i].usado && dsh.hijo_lee[i] < b) b = dsh.hijo_lee[i];
    return b;
}

static int ds_en_ventana(uint32_t t) { return t >= ds_base() && t < dsh.leidos + DS_PASOS - 8; }

static void ds_envia_hijo(int i);

// anfitrión: cierra los conjuntos que ya tienen los datos de todos y se los manda a los invitados
static void ds_cierra(void) {
    uint16_t esperados = (uint16_t)(dsh.bitmap & (lee16(estado + ST_LISTOS) | 1));
    int nuevos = 0;
    for (;;) {
        uint32_t t = dsh.cierre;
        int k = (int)(t % DS_PASOS);
        if (dsh.paso[k] != t + 1 || (dsh.recibido[k] & esperados) != esperados) break;
        dsh.completo[k] = 1;
        dsh.cerrado_con[k] = (uint16_t)(dsh.recibido[k] & dsh.bitmap);
        dsh.cierre++;
        nuevos = 1;
    }
    if (nuevos)
        for (int i = 1; i < 16; i++) if (invitados[i].usado) ds_envia_hijo(i);
}

static void ds_envia_hijo(int i) {
    static uint8_t b[1400];
    int n = cabecera(b, DS_PADRE);
    uint32_t primero = dsh.hijo_lee[i];
    if (dsh.cierre > 12 && primero < dsh.cierre - 12) primero = dsh.cierre - 12;
    uint8_t cuantos = (uint8_t)(dsh.cierre > primero ? dsh.cierre - primero : 0);
    memcpy(b + n, &dsh.sesion, 4); memcpy(b + n + 4, &dsh.hijo_hecho[i], 4); memcpy(b + n + 8, &primero, 4);
    b[n + 12] = cuantos;
    n += 13;
    for (uint32_t t = primero; t < primero + cuantos; t++) {
        int k = (int)(t % DS_PASOS);
        memcpy(b + n, &dsh.cerrado_con[k], 2);
        n += 2;
        for (int a = 0; a < 16; a++)
            if (dsh.bitmap & (1u << a)) { memcpy(b + n, dsh.datos[k][a], (size_t)dsh.largo); n += dsh.largo; }
    }
    envia(&invitados[i].dir, b, n);
}

static void ds_envia_padre(void) {
    uint8_t b[600];
    int n = cabecera(b, DS_HIJO);
    uint32_t primero = dsh.padre_tiene;
    if (dsh.hechos > 16 && primero < dsh.hechos - 16) primero = dsh.hechos - 16;
    uint8_t cuantos = (uint8_t)(dsh.hechos > primero ? dsh.hechos - primero : 0);
    memcpy(b + n, &dsh.sesion, 4); memcpy(b + n + 4, &dsh.leidos, 4); memcpy(b + n + 8, &primero, 4);
    b[n + 12] = cuantos;
    n += 13;
    for (uint32_t t = primero; t < primero + cuantos; t++) {
        memcpy(b + n, dsh.datos[t % DS_PASOS][dsh.aid], (size_t)dsh.largo);
        n += dsh.largo;
    }
    envia(&mi_anfitrion, b, n);
}

// del invitado (anfitrión)
static void ds_de_hijo(const uint8_t *b, int n, int aid) {
    if (!dsh.iniciado || dsh.aid != 0 || n < 14 + 13) return;
    uint32_t ses, lee, primero;
    memcpy(&ses, b + 14, 4); memcpy(&lee, b + 18, 4); memcpy(&primero, b + 22, 4);
    int cuantos = b[26];
    if (ses != dsh.sesion && ses != 0) return;  // de una carrera anterior
    if (14 + 13 + cuantos * dsh.largo > n) return;
    if (lee > dsh.hijo_lee[aid] && lee <= dsh.cierre) dsh.hijo_lee[aid] = lee;
    for (int j = 0; j < cuantos; j++) {
        uint32_t t = primero + (uint32_t)j;
        if (!ds_en_ventana(t)) continue;
        int k = ds_hueco(t);
        if (dsh.completo[k]) continue;
        memcpy(dsh.datos[k][aid], b + 27 + j * dsh.largo, (size_t)dsh.largo);
        dsh.recibido[k] |= (uint16_t)(1u << aid);
    }
    for (;;) {  // hasta dónde tengo seguidos sus datos
        uint32_t t = dsh.hijo_hecho[aid];
        int k = (int)(t % DS_PASOS);
        if (dsh.paso[k] != t + 1 || !(dsh.recibido[k] & (1u << aid))) break;
        dsh.hijo_hecho[aid]++;
    }
    ds_cierra();
}

// del anfitrión (invitado)
static void ds_de_padre(const uint8_t *b, int n) {
    if (!dsh.iniciado || dsh.aid == 0 || n < 14 + 13) return;
    uint32_t ses, tiene, primero;
    memcpy(&ses, b + 14, 4); memcpy(&tiene, b + 18, 4); memcpy(&primero, b + 22, 4);
    int cuantos = b[26];
    if (!dsh.sesion) {
        if (primero >= DS_PASOS) return;  // de la carrera anterior: aún no ha empezado esta
        dsh.sesion = ses;
    }
    if (ses != dsh.sesion) return;
    int est = 0;
    for (int a = 0; a < 16; a++) if (dsh.bitmap & (1u << a)) est++;
    int tam = 2 + est * dsh.largo;
    if (14 + 13 + cuantos * tam > n) return;
    if (tiene > dsh.padre_tiene && tiene <= dsh.hechos) dsh.padre_tiene = tiene;
    for (int j = 0; j < cuantos; j++) {
        uint32_t t = primero + (uint32_t)j;
        if (t < dsh.leidos || !ds_en_ventana(t)) continue;
        int k = ds_hueco(t);
        if (dsh.completo[k]) continue;
        const uint8_t *c = b + 27 + j * tam;
        memcpy(&dsh.cerrado_con[k], c, 2);
        c += 2;
        for (int a = 0; a < 16; a++)
            if (dsh.bitmap & (1u << a)) { memcpy(dsh.datos[k][a], c, (size_t)dsh.largo); c += dsh.largo; }
        dsh.recibido[k] = dsh.cerrado_con[k];
        dsh.completo[k] = 1;
    }
}

// WM_StartDataSharing (021450EC): empieza otra carrera. El original sigue (inicializa el WMDataSharingInfo).
void wifi_ds_empieza(uint32_t info) {
    static int desactivado = -1;
    if (desactivado < 0) desactivado = getenv("MKDS_DS_ORIGINAL") != NULL;
    memset(&dsh, 0, sizeof dsh);
    if (desactivado) return;
    dsh.info = info;
    const char *a = getenv("MKDS_LOCAL_ADELANTO");
    dsh.adelanto = a && atoi(a) > 0 ? atoi(a) : 2;
    if (dsh.adelanto > 12) dsh.adelanto = 12;
}

static uint64_t espera_desde;  // desde cuándo espera el conjunto que toca (0 = no espera)

// WM_StepDataSharing (02144C6C): devuelve lo que devolvería el original (0 = conjunto leído, 5 = aún no) o -1
// para dejar que lo haga el original.
int wifi_ds_paso(uint32_t info, uint32_t envio, uint32_t recibe) {
    if (!dsh.info || info != dsh.info || !estado) return -1;
    if (!dsh.iniciado) {  // los campos los ha puesto ya el WM_StartDataSharing original
        dsh.bitmap = lee16(info + 0x80E);
        dsh.largo = lee16(info + 0x810);
        dsh.aid = lee16(estado + ST_AID);
        if (dsh.largo <= 0 || dsh.largo > DS_MAX || dsh.aid > 15) { dsh.info = 0; return -1; }
        if (dsh.aid == 0) {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            dsh.sesion = (uint32_t)t.QuadPart | 1;
        }
        dsh.iniciado = 1;
        if (traza) fprintf(stderr, "[wifi] carrera: DataSharing con adelanto %d (aid %d, %d bytes, jugadores %04X)\n",
                           dsh.adelanto, dsh.aid, dsh.largo, dsh.bitmap);
    }
    // mis datos para el siguiente paso, con 'adelanto' de ventaja (uno por llamada: el juego no avanza sin leer)
    if (dsh.hechos <= dsh.leidos + (uint32_t)dsh.adelanto) {
        int k = ds_hueco(dsh.hechos);
        memcpy(dsh.datos[k][dsh.aid], g_main + (envio & 0x3FFFFF), (size_t)dsh.largo);
        dsh.recibido[k] |= (uint16_t)(1u << dsh.aid);
        dsh.hechos++;
    }
    if (dsh.aid == 0) {
        ds_cierra();
        for (int i = 1; i < 16; i++) if (invitados[i].usado) ds_envia_hijo(i);  // reenvía lo no confirmado
    } else ds_envia_padre();
    int k = (int)(dsh.leidos % DS_PASOS);
    if (dsh.paso[k] != dsh.leidos + 1 || !dsh.completo[k]) {
        if (dsh.leidos > 0) dsh.atascos++;
        static uint64_t avisado;  // MKDS_WIFI_LOG: si se queda esperando más de 1 s, qué falta
        if (!espera_desde) espera_desde = ms_ahora;
        if (traza && ms_ahora - espera_desde > 1000 && ms_ahora - avisado > 1000) {
            avisado = ms_ahora;
            fprintf(stderr, "[wifi] carrera ATASCADA en el paso %u: hechos %u, cierre %u, hueco paso %u recibido %04X completo %d, padre_tiene %u",
                    dsh.leidos, dsh.hechos, dsh.cierre, dsh.paso[k] - 1, dsh.recibido[k], dsh.completo[k], dsh.padre_tiene);
            if (dsh.aid == 0) {
                fprintf(stderr, " esperados %04X;", (unsigned)(dsh.bitmap & (lee16(estado + ST_LISTOS) | 1)));
                for (int i = 1; i < 16; i++)
                    if (invitados[i].usado) fprintf(stderr, " [%d lee %u hecho %u]", i, dsh.hijo_lee[i], dsh.hijo_hecho[i]);
            }
            fprintf(stderr, "\n");
        }
        return 5;
    }
    espera_desde = 0;
    uint8_t *p = g_main + (recibe & 0x3FFFFF);
    memset(p, 0, 0x200);
    pon16(recibe, dsh.bitmap);
    pon16(recibe + 2, dsh.cerrado_con[k]);
    int idx = 0;
    for (int a = 0; a < 16; a++)
        if (dsh.bitmap & (1u << a)) {
            if (dsh.cerrado_con[k] & (1u << a)) memcpy(p + 4 + idx * dsh.largo, dsh.datos[k][a], (size_t)dsh.largo);
            idx++;
        }
    pon16(info + 0x81A, (uint16_t)dsh.leidos);  // currentSeqNum
    dsh.leidos++;
    // si hay que esperar a menudo (más de 4 veces cada 2 s), un paso más de adelanto
    if (++dsh.buenos >= 120) {
        if (dsh.atascos > 4 && dsh.adelanto < 12) {
            dsh.adelanto++;
            if (traza) fprintf(stderr, "[wifi] carrera: adelanto %d (%d esperas)\n", dsh.adelanto, dsh.atascos);
        }
        dsh.buenos = dsh.atascos = 0;
    }
    return 0;
}

static void red_atiende(void) {
    if (!red_lista) return;
    if (retraso_ms > 0) envia_retrasados();
    static uint64_t ultimo_anuncio;
    int st = estado ? lee16(estado) : 0;
    if ((st == PARENT || st == MP_PARENT) && param_padre && ms_ahora - ultimo_anuncio >= 300) {
        ultimo_anuncio = ms_ahora;
        anuncia();
    }
    // el invitado espera la respuesta del anfitrión; a los 3 s, fallo
    if (conecta.esperando && ms_ahora - conecta.desde > 3000) {
        conecta.esperando = 0;
        pon_estado(IDLE);
        responde_simple(0x0C, 1);
    }
    SOCKET ss[2] = {comun, propio};
    for (int k = 0; k < 2; k++) {
        if (ss[k] == INVALID_SOCKET) continue;
        for (int vueltas = 0; vueltas < 32; vueltas++) {
            static uint8_t b[2048];  // los reenvíos del DataSharing pueden pasar de 512 bytes (con 8 jugadores, ~820)
            struct sockaddr_in de;
            int l = sizeof de;
            int n = recvfrom(ss[k], (char *)b, sizeof b, 0, (struct sockaddr *)&de, &l);
            if (n < 14) break;
            if (memcmp(b, "DSKR", 4) || !memcmp(b + 8, mi_mac, 6)) continue;  // no es nuestro, o soy yo
            uint16_t su_puerto;
            memcpy(&su_puerto, b + 6, 2);
            de.sin_port = htons(su_puerto);  // se le contesta a su socket propio
            const uint8_t *mac = b + 8;
            switch (b[4]) {
            case ANUNCIO: {
                if (n < 14 + 0x40 + 1) break;
                int g = -1;
                for (int i = 0; i < 4; i++) if (grupos[i].visto && !memcmp(grupos[i].mac, mac, 6)) g = i;
                for (int i = 0; i < 4 && g < 0; i++) if (!grupos[i].visto || ms_ahora - grupos[i].visto > 5000) g = i;
                if (g < 0) break;
                if (traza && (!grupos[g].visto || memcmp(grupos[g].mac, mac, 6)))
                    fprintf(stderr, "[wifi] grupo encontrado en %s\n", inet_ntoa(de.sin_addr));
                Grupo *G = &grupos[g];
                G->dir = de;
                memcpy(G->mac, mac, 6);
                memcpy(G->param, b + 14, 0x40);
                G->info_largo = b[14 + 0x40];
                if (G->info_largo > 112 || 14 + 0x41 + G->info_largo > n) G->info_largo = 0;
                memcpy(G->info, b + 14 + 0x41, (size_t)G->info_largo);
                G->visto = ms_ahora ? ms_ahora : 1;
                break;
            }
            case UNIRSE: {  // soy anfitrión: le doy un aid y se lo digo al juego
                if (!(st == PARENT || st == MP_PARENT) || n < 14 + 24) break;
                int aid = 0;
                for (int i = 1; i < 16; i++) if (invitados[i].usado && !memcmp(invitados[i].mac, mac, 6)) aid = i;
                int max = param_padre ? lee16(param_padre + 0x10) : 7;
                for (int i = 1; i <= max && i < 16 && !aid; i++) if (!invitados[i].usado) aid = i;
                if (!aid) break;
                int nuevo = !invitados[aid].usado;
                invitados[aid].usado = 1;
                invitados[aid].dir = de;
                memcpy(invitados[aid].mac, mac, 6);
                pon_hijos();
                pon16(estado + ST_AID, 0);
                if (param_padre) pon16(estado + ST_MAX_ENVIO, (uint16_t)(lee16(param_padre + 0x34) + 4));
                uint8_t r[32];
                int m = cabecera(r, ACEPTADO);
                r[m++] = (uint8_t)aid;
                envia(&de, r, m);
                if (!nuevo) break;
                if (traza) fprintf(stderr, "[wifi] se une un invitado (aid %d) desde %s\n", aid, inet_ntoa(de.sin_addr));
                // al juego: START_PARENT con estado 7 (conectado), MAC en +0A, aid en +10, sus 24 bytes en +14
                uint32_t c = bufer_respuesta();
                pon16(c, 0x08);
                pon16(c + 8, 7);
                memcpy(g_main + ((c + 0xA) & 0x3FFFFF), mac, 6);
                pon16(c + 0x10, (uint16_t)aid);
                memcpy(g_main + ((c + 0x14) & 0x3FFFFF), b + 14, 24);
                responde(c);
                break;
            }
            case ACEPTADO: {  // soy invitado: ya estoy dentro
                if (!conecta.esperando || n < 15) break;
                conecta.esperando = 0;
                mi_aid = b[14];
                mi_anfitrion = de;
                pon_estado(CHILD);
                pon16(estado + ST_AID, (uint16_t)mi_aid);
                pon16(estado + ST_HIJOS, 1);
                pon16(estado + ST_MAX_ENVIO, (uint16_t)(max_hijo + 2));
                pon16(estado + ST_LISTO_HIJO, 1);
                if (traza) fprintf(stderr, "[wifi] dentro del grupo de %s con aid %d\n", inet_ntoa(de.sin_addr), mi_aid);
                // al juego: START_CONNECT con estado 7 (conectado), aid en +0A y MAC del anfitrión en +0C. El juego
                // (02051AD4) guarda el aid de +0A como su número de jugador (objeto+0x432); con la MAC ahí se
                // presentaba como el 0 y el anfitrión le contestaba a sí mismo.
                uint32_t c = bufer_respuesta();
                pon16(c, 0x0C);
                pon16(c + 8, 7);
                pon16(c + 0xA, (uint16_t)mi_aid);
                memcpy(g_main + ((c + 0xC) & 0x3FFFFF), mac, 6);
                pon16(c + 0x10, (uint16_t)mi_aid);
                responde(c);
                break;
            }
            case DATOS: {  // +14 puerto, +16 aid de quien lo manda, +18 largo, +1A datos
                if (n < 14 + 6 || !mp_recibe) break;
                Paquete *q = &cola[cola_fin];
                if ((cola_fin + 1) % 32 == cola_ini) break;  // llena: se pierde (como en el aire)
                memcpy(&q->puerto, b + 14, 2);
                memcpy(&q->aid, b + 16, 2);
                memcpy(&q->largo, b + 18, 2);
                if (q->largo > n - 20 || q->largo > sizeof q->datos) break;
                memcpy(q->mac, mac, 6);
                memcpy(q->datos, b + 20, q->largo);
                cola_fin = (cola_fin + 1) % 32;
                break;
            }
            case DS_HIJO:
                for (int i = 1; i < 16; i++)
                    if (invitados[i].usado && !memcmp(invitados[i].mac, mac, 6)) ds_de_hijo(b, n, i);
                break;
            case DS_PADRE:
                ds_de_padre(b, n);
                break;
            case ADIOS:
                for (int i = 1; i < 16; i++)
                    if (invitados[i].usado && !memcmp(invitados[i].mac, mac, 6)) {
                        invitados[i].usado = 0;
                        pon_hijos();
                        uint32_t c = bufer_respuesta();  // al juego: estado 9 (desconectado)
                        pon16(c, 0x08);
                        pon16(c + 8, 9);
                        memcpy(g_main + ((c + 0xA) & 0x3FFFFF), mac, 6);
                        pon16(c + 0x10, (uint16_t)i);
                        responde(c);
                    }
                for (int i = 0; i < 4; i++) if (!memcmp(grupos[i].mac, mac, 6)) grupos[i].visto = 0;
                break;
            }
        }
    }
    entrega_datos();
}

// WMBssDesc (0xC0) de un grupo, como lo daría el chip: el juego mira ggid (+44), atributo (+4B) y tgid (+48)
static void pon_bss(uint32_t d, const Grupo *G, uint16_t ch) {
    uint8_t *p = g_main + (d & 0x3FFFFF);
    memset(p, 0, 0xC0);
    int gi = 0x10 + G->info_largo;  // WMGameInfo: 16 bytes fijos + userGameInfo
    uint16_t v;
    v = (uint16_t)((0x40 + gi + 1) / 2); memcpy(p + 0x00, &v, 2);  // largo en medias palabras
    v = 40; memcpy(p + 0x02, &v, 2);                                 // señal
    memcpy(p + 0x04, G->mac, 6);
    v = 32; memcpy(p + 0x0A, &v, 2);
    memcpy(p + 0x0C, G->param + 8, 4);    // ssid: ggid y tgid
    memcpy(p + 0x10, G->param + 0xC, 2);
    v = 0x0021; memcpy(p + 0x2C, &v, 2);  // capacidades
    v = 0x0003; memcpy(p + 0x2E, &v, 2);
    memcpy(p + 0x30, &v, 2);
    memcpy(p + 0x32, G->param + 0x18, 2); // periodo de anuncio
    v = 1; memcpy(p + 0x34, &v, 2);
    memcpy(p + 0x36, &ch, 2);
    v = (uint16_t)gi; memcpy(p + 0x3C, &v, 2);
    v = 1; memcpy(p + 0x40, &v, 2);       // magicNumber
    p[0x42] = 1;                          // versión
    memcpy(p + 0x44, G->param + 8, 4);    // ggid
    memcpy(p + 0x48, G->param + 0xC, 2);  // tgid
    p[0x4A] = (uint8_t)G->info_largo;
    uint16_t entrada, multi, ks, cs;
    memcpy(&entrada, G->param + 0xE, 2); memcpy(&multi, G->param + 0x12, 2);
    memcpy(&ks, G->param + 0x14, 2); memcpy(&cs, G->param + 0x16, 2);
    p[0x4B] = (uint8_t)((entrada ? 1 : 0) | (multi ? 2 : 0) | (ks ? 4 : 0) | (cs ? 8 : 0));
    memcpy(p + 0x4C, G->param + 0x34, 2); // tamaños MP
    memcpy(p + 0x4E, G->param + 0x36, 2);
    memcpy(p + 0x50, G->info, (size_t)G->info_largo);
}

// Respuesta corta (WMCallback): apiid, error, wlCmdID, wlResult
static void responde_simple(int api, int error) {
    uint32_t b = bufer_respuesta();
    pon16(b, (uint16_t)api);
    pon16(b + 2, (uint16_t)error);
    responde(b);
}

static void pon_estado(int s) {
    if (estado) pon16(estado, (uint16_t)s);
}

// Una orden del ARM9 (la dirección de su búfer). Devuelve 1 si la ha hecho el port.
int wifi_orden(uint32_t dir) {
    if (traza < 0) traza = getenv("MKDS_WIFI_LOG") != NULL;
    if ((dir & 0xFF000000) != 0x02000000) return 0;
    int api = lee16(dir) & 0x7FFF;
    uint32_t p[8];
    for (int i = 0; i < 8; i++) p[i] = lee32(dir + 4 + 4 * i);
    if (traza)
    {
        fprintf(stderr, "[wifi] orden %02X %s: %08X %08X %08X %08X %08X %08X %08X", api, nombre(api), p[0], p[1],
                p[2], p[3], p[4], p[5], p[6]);
        if (api == 0x0F && (p[0] & 0xFF000000) == 0x02000000)  // los primeros bytes de lo que se envía
            for (uint32_t i = 0; i < p[1] && i < 12; i++) fprintf(stderr, "%s%02X", i ? "" : " | ", g_main[(p[0] + i) & 0x3FFFFF]);
        fprintf(stderr, "\n");
    }
    pon16(dir, (uint16_t)(lee16(dir) | 0x8000));  // búfer leído: el ARM9 ya puede volver a usarlo
    switch (api) {
    case 0x00:  // INITIALIZE: búfer del ARM7, estado y búfer de respuestas
        buf7 = p[0]; estado = p[1]; respuestas = p[2];
        red_abre();
        for (int i = 0; i < 3; i++) pon16(buf7 + 0x100u * (uint32_t)i, 0x8000);
        pon_estado(IDLE);
        responde_simple(api, 0);
        break;
    case 0x01:  // RESET
        pon_estado(IDLE);
        responde_simple(api, 0);
        break;
    case 0x02:  // END
        pon_estado(READY);
        responde_simple(api, 0);
        break;
    case 0x03: case 0x05:  // ENABLE, POWER_ON
        pon_estado(IDLE);
        responde_simple(api, 0);
        break;
    case 0x04: case 0x06:  // DISABLE, POWER_OFF
        pon_estado(api == 4 ? READY : STOP);
        responde_simple(api, 0);
        break;
    case 0x0A: {  // START_SCAN: +2 canal, +4 dónde dejar el WMBssDesc, +8 cuánto escuchar (ms)
        // Respuesta (WMStartScanCallback): +8 estado (4 nadie, 5 encontrado), +0A MAC, +10 canal,
        // +12 calidad. El juego copia 0xC0 bytes del WMBssDesc del búfer que dio.
        canal = lee16(dir + 2);
        unsigned ms = lee16(dir + 8);
        if (ms < 10) ms = 10;
        if (ms > 500) ms = 500;
        pon_estado(SCAN);
        uint32_t b = bufer_respuesta();
        pon16(b, (uint16_t)api);
        pon16(b + 8, PARENT_NOT_FOUND);
        pon16(b + 0x10, canal);
        // un grupo anunciado por la red en los últimos 3 s, en este canal (o en cualquiera si el juego busca en
        // todos): se le da por turnos para que salgan todos en la lista
        static int turno;
        for (int k = 0; k < 4; k++) {
            Grupo *G = &grupos[(turno + k) % 4];
            uint16_t su_canal;
            memcpy(&su_canal, G->param + 0x32, 2);
            if (!G->visto || ms_ahora - G->visto > 3000 || (canal && su_canal && su_canal != canal)) continue;
            turno = (turno + k + 1) % 4;
            uint32_t bss = p[0];
            if ((bss & 0xFF000000) == 0x02000000) pon_bss(bss, G, canal ? canal : su_canal);
            pon16(b + 8, PARENT_FOUND);
            memcpy(g_main + ((b + 0xA) & 0x3FFFFF), G->mac, 6);
            pon16(b + 0x12, 3);  // calidad del enlace
            if (traza) fprintf(stderr, "[wifi] búsqueda en el canal %u: grupo encontrado\n", canal);
            break;
        }
        responde_luego(b, ms);
        break;
    }
    case 0x0C: {  // START_CONNECT: +4 WMBssDesc del grupo elegido, +8 24 bytes del invitado
        uint32_t bss = p[0];
        Grupo *G = NULL;
        for (int i = 0; i < 4; i++)
            if (grupos[i].visto && !memcmp(grupos[i].mac, g_main + ((bss + 4) & 0x3FFFFF), 6)) G = &grupos[i];
        if (!G) { responde_simple(api, 1); break; }
        conecta.esperando = 1;
        conecta.desde = ms_ahora;
        memcpy(&max_hijo, G->param + 0x36, 2);
        conecta.dir = G->dir;
        memcpy(conecta.ssid, g_main + ((dir + 8) & 0x3FFFFF), 24);
        pon_estado(CONNECT);
        uint8_t r[64];
        int n = cabecera(r, UNIRSE);
        memcpy(r + n, conecta.ssid, 24);
        n += 24;
        envia(&G->dir, r, n);
        if (traza) fprintf(stderr, "[wifi] pidiendo entrar en el grupo de %s\n", inet_ntoa(G->dir.sin_addr));
        break;  // la respuesta llega con ACEPTADO (red_atiende) o fallo a los 3 s
    }
    case 0x0D: {  // DISCONNECT
        uint8_t r[32];
        int n = cabecera(r, ADIOS);
        if (lee16(estado) == CHILD || lee16(estado) == MP_CHILD) envia(&mi_anfitrion, r, n);
        pon_estado(IDLE);
        responde_simple(api, 0);
        break;
    }
    case 0x0B:  // END_SCAN
        pon_estado(IDLE);
        responde_simple(api, 0);
        break;
    case 0x07:  // SET_P_PARAM: +4 dirección del WMParentParam (0x40 bytes)
        param_padre = p[0];
        if (traza) {
            fprintf(stderr, "[wifi] parámetros del anfitrión:");
            for (int i = 0; i < 0x40; i += 4) fprintf(stderr, " %08X", lee32(param_padre + (uint32_t)i));
            fprintf(stderr, "\n");
        }
        responde_simple(api, 0);
        break;
    case 0x0E: {  // START_MP: +4 búfer de recepción, +8 su tamaño /2, +C búfer de envío, +10 su tamaño /2...
        // Respuesta (WMStartMPCallback): el estado va en +4 (10 empieza, 11 fin de ronda, 12 datos) y en
        // +8 el búfer recibido. El juego (02051958) solo espera el 10 para seguir.
        pon_estado(lee16(estado) == CHILD ? MP_CHILD : MP_PARENT);
        pon32(estado + 0xC, 1);  // mp_flag: el ARM9 lo mira para no empezar dos veces
        mp_recibe = p[0];
        mp_recibe_tam = p[1] * 2;
        cola_ini = cola_fin = 0;
        uint32_t b = bufer_respuesta();
        pon16(b, (uint16_t)api);
        pon16(b + 4, 10);
        responde(b);
        break;
    }
    case 0x10:  // END_MP
        pon32(estado + 0xC, 0);
        pon_estado(lee16(estado) == MP_CHILD ? CHILD : PARENT);
        responde_simple(api, 0);
        break;
    case 0x08: {  // START_PARENT: ya somos anfitrión; los invitados llegarán por la red
        // Respuesta (WMStartParentCallback): +8 estado, +0A MAC, +10 aid, +12 motivo
        pon_estado(PARENT);
        uint32_t b = bufer_respuesta();
        pon16(b, (uint16_t)api);
        pon16(b + 8, PARENT_START);
        responde(b);
        break;
    }
    case 0x09: {  // END_PARENT: se avisa a los invitados
        uint8_t r[32];
        int n = cabecera(r, ADIOS);
        for (int i = 1; i < 16; i++) if (invitados[i].usado) { envia(&invitados[i].dir, r, n); invitados[i].usado = 0; }
        pon_estado(IDLE);
        responde_simple(api, 0);
        break;
    }
    case 0x1E: {  // MEASURE_CHANNEL (+2 modo, +4 umbral, +6 canal, +8 ms): el juego elige el canal más libre
        uint32_t b = bufer_respuesta();
        pon16(b, (uint16_t)api);
        pon16(b + 8, lee16(dir + 6));  // canal medido
        pon16(b + 0xA, 0);             // ocupación: todos libres
        unsigned ms = lee16(dir + 8);
        responde_luego(b, ms < 5 ? 5 : ms > 200 ? 200 : ms);
        break;
    }
    case 0x0F: {  // SET_MP_DATA: datos, largo, a quién (bit por aid), puerto, prioridad, función y argumento
        // Va por UDP: el anfitrión a cada invitado marcado, el invitado al anfitrión. Al acabar, aviso 0x81
        // (WMPortSendCallback): el ARM9 llama a la función de +1C con el argumento en +20.
        uint32_t datos = p[0];
        uint16_t largo = (uint16_t)p[1], para = (uint16_t)p[2], puerto = (uint16_t)p[3];
        uint8_t r[600];
        int n = cabecera(r, DATOS);
        uint16_t yo = (uint16_t)lee16(estado + ST_AID);
        memcpy(r + n, &puerto, 2); memcpy(r + n + 2, &yo, 2); memcpy(r + n + 4, &largo, 2);
        n += 6;
        if (largo > 512) largo = 512;
        if ((datos & 0xFF000000) == 0x02000000) memcpy(r + n, g_main + (datos & 0x3FFFFF), largo);
        n += largo;
        uint16_t enviado = 0;
        int st = lee16(estado);
        if (st == MP_PARENT || st == PARENT) {
            for (int i = 1; i < 16; i++)
                if (invitados[i].usado && (para & (1u << i))) { envia(&invitados[i].dir, r, n); enviado |= (uint16_t)(1u << i); }
        } else if (para & 1) {
            envia(&mi_anfitrion, r, n);
            enviado = 1;
        }
        uint32_t c = bufer_respuesta();
        pon16(c, 0x81);
        pon16(c + 8, 20);  // PORT_SEND
        pon16(c + 0xA, puerto);
        pon16(c + 0xC, para);
        pon16(c + 0x10, enviado);
        pon32(c + 0x14, datos);
        pon16(c + 0x18, largo);
        pon32(c + 0x1C, p[5]);
        pon32(c + 0x20, p[6]);
        responde(c);
        break;
    }
    case 0x18: case 0x19: case 0x1D: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
        // SET_GAMEINFO, SET_BEACON_IND, SET_LIFETIME, SET_ENTRY, AUTO_DEAUTH, SET_MP_PARAMETER,
        // SET_BEACON_PERIOD, AUTO_DISCONNECT: de momento basta con decir que sí
        responde_simple(api, 0);
        break;
    default:
        if (traza) fprintf(stderr, "[wifi] orden %02X %s: aún no la sabe hacer el port\n", api, nombre(api));
        responde_simple(api, 1);
        break;
    }
    return 1;
}
