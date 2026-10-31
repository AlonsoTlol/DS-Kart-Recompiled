// El servidor de la Conexión Wi-Fi de Nintendo, imitado en el propio juego (lo que hay en 192.168.77.10, ver wfc.c).
// El WFC de verdad cerró en 2014; esto es lo mínimo que necesita la DS para "entrar":
//   - HTTP (80): conntest (¿hay internet?) y nas /ac (inicio de sesión de Nintendo: acctcreate, login, svcloc).
//   - GameSpy Presence (gpcm 29900, gpsp 29901): el inicio de sesión de GameSpy y el perfil (el código de amigo).
//   - GameSpy Available (UDP 27900): "el juego tiene servidores".
// Lo de buscar partida (master 28910, QR2 27900, NAT negotiation 27901) va en el mismo sitio pero habla con el
// servidor de salas en Cloudflare para encontrar a los demás jugadores.
// Los formatos son los de GameSpy (los documentaron proyectos como AltWFC / dwc_network_server_emulator).
#include "compat.h"  // hilos y secciones críticas (en Windows, windows.h)
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wfc.h"

// ---- utilidades ---------------------------------------------------------------------------------------------------
static uint32_t azar_estado;
static uint32_t azar(void) {
    if (!azar_estado) azar_estado = (uint32_t)time(NULL) * 2654435761u | 1;
    azar_estado ^= azar_estado << 13; azar_estado ^= azar_estado >> 17; azar_estado ^= azar_estado << 5;
    return azar_estado;
}
static void azar_texto(char *s, int n, const char *letras) {
    int l = (int)strlen(letras);
    for (int i = 0; i < n; i++) s[i] = letras[azar() % (uint32_t)l];
    s[n] = 0;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
// Base64 de Nintendo: '=' es '*' (y a veces '+' es '.' y '/' es '-')
static int b64_dec(const char *s, int n, char *out, int max) {
    uint32_t v = 0;
    int bits = 0, k = 0;
    for (int i = 0; i < n; i++) {
        int c = s[i], x;
        if (c >= 'A' && c <= 'Z') x = c - 'A';
        else if (c >= 'a' && c <= 'z') x = c - 'a' + 26;
        else if (c >= '0' && c <= '9') x = c - '0' + 52;
        else if (c == '+' || c == '.') x = 62;
        else if (c == '/' || c == '-') x = 63;
        else continue;
        v = v << 6 | (uint32_t)x;
        bits += 6;
        if (bits >= 8) { bits -= 8; if (k < max - 1) out[k++] = (char)(v >> bits); }
    }
    out[k] = 0;
    return k;
}
static void b64_cod(const char *d, int n, char *out) {
    int k = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)d[i] << 16 | (i + 1 < n ? (uint32_t)(uint8_t)d[i + 1] << 8 : 0) | (i + 2 < n ? (uint8_t)d[i + 2] : 0);
        out[k++] = b64[v >> 18];
        out[k++] = b64[(v >> 12) & 63];
        out[k++] = i + 1 < n ? b64[(v >> 6) & 63] : '*';
        out[k++] = i + 2 < n ? b64[v & 63] : '*';
    }
    out[k] = 0;
}

// MD5 (para la "prueba" del inicio de sesión de GameSpy)
static uint32_t md5_k[64];
static const uint8_t md5_s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
                                  6, 10, 15, 21, 6, 10, 15, 21};
static void md5_bloque(uint32_t h[4], const uint8_t *p) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d, x = a + f + md5_k[i] + w[g];
        d = c; c = b;
        b = b + (x << md5_s[i] | x >> (32 - md5_s[i]));
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}
static void md5_hex(const void *dv, size_t n, char *out) {
    if (!md5_k[0]) for (int i = 0; i < 64; i++) md5_k[i] = (uint32_t)(fabs(sin(i + 1.0)) * 4294967296.0);
    const uint8_t *d = dv;
    uint32_t h[4] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) md5_bloque(h, d + i);
    uint8_t b[128] = {0};
    size_t r = n - i, tot = r + 9 <= 64 ? 64 : 128;
    memcpy(b, d + i, r);
    b[r] = 0x80;
    for (int k = 0; k < 8; k++) b[tot - 8 + (size_t)k] = (uint8_t)((uint64_t)n * 8 >> (8 * k));
    md5_bloque(h, b);
    if (tot == 128) md5_bloque(h, b + 64);
    for (int k = 0; k < 16; k++) sprintf(out + 2 * k, "%02x", (h[k / 4] >> (8 * (k % 4))) & 255);
}

// Valor de "\clave\valor\" en un mensaje de GameSpy ("" si no está)
static void gs_valor(const char *m, const char *clave, char *out, int max) {
    char busca[64];
    snprintf(busca, sizeof busca, "\\%s\\", clave);
    const char *p = strstr(m, busca);
    out[0] = 0;
    if (!p) return;
    p += strlen(busca);
    int k = 0;
    while (*p && *p != '\\' && k < max - 1) out[k++] = *p++;
    out[k] = 0;
}

// ---- quién es este jugador ----------------------------------------------------------------------------------------------
// Lo da el nas al iniciar sesión y lo usa GameSpy después: se guarda aquí (un solo jugador por juego).
static struct {
    char userid[32], gsbrcd[32], nombre[64], reto_ac[16], token[64];
    char apellido[64];  // "lastname" del perfil: el DWC guarda ahí algo suyo y espera leerlo después
    uint32_t perfil;
} yo;

static uint32_t perfil_de(const char *userid, const char *gsbrcd) {
    uint32_t h = 2166136261u;  // FNV-1a: siempre el mismo perfil (y código de amigo) para la misma DS y tarjeta
    for (const char *p = userid; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (const char *p = gsbrcd; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return (h & 0x7FFFFFFF) | 0x100000;
}

// Los perfiles, como en GameSpy: uno por número de usuario y gsbrcd (un código que el DWC guarda en la tarjeta).
// El DWC pone en el "lastname" del perfil "000000000" + su gsbrcd y al volver comprueba que siga ahí; si no, cree
// que la cuenta no es suya, se inventa otro gsbrcd y acaba en el error 20101. La primera vez (acctcreate) aún no
// manda gsbrcd: el perfil de esa sesión se apunta también con el gsbrcd que trae el lastname.
// wfc_perfil.txt (junto al juego), una línea por perfil: "userid gsbrcd perfil lastname"
void wfc_firmware_pon_id(uint64_t id);  // ds.c

static void perfil_lee(void) {
    yo.apellido[0] = 0;
    yo.perfil = perfil_de(yo.userid, yo.gsbrcd);
    FILE *f = fopen("wfc_perfil.txt", "r");
    if (!f) return;
    char u[32], g[32], a[64];
    unsigned p;
    while (fscanf(f, "%31s %31s %u %63s", u, g, &p, a) == 4)
        if (!strcmp(u, yo.userid) && !strcmp(g, yo.gsbrcd[0] ? yo.gsbrcd : "-")) {
            yo.perfil = p;
            snprintf(yo.apellido, sizeof yo.apellido, "%s", a);
        }
    fclose(f);
}
static void perfil_guarda(const char *gsbrcd) {
    char l[64][160];
    int n = 0;
    FILE *f = fopen("wfc_perfil.txt", "r");
    if (f) {
        char u[32], g[32], a[64];
        unsigned p;
        while (n < 63 && fscanf(f, "%31s %31s %u %63s", u, g, &p, a) == 4)
            if (strcmp(u, yo.userid) || strcmp(g, gsbrcd)) snprintf(l[n++], sizeof l[0], "%s %s %u %s\n", u, g, p, a);
        fclose(f);
    }
    snprintf(l[n++], sizeof l[0], "%s %s %u %s\n", yo.userid, gsbrcd, yo.perfil, yo.apellido);
    f = fopen("wfc_perfil.txt", "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fputs(l[i], f);
    fclose(f);
}

// ---- HTTP: conntest y nas -------------------------------------------------------------------------------------------------
static void http_responde(Con *c, int codigo, const char *extra, const char *cuerpo) {
    char r[4096];
    int n = snprintf(r, sizeof r,
                     "HTTP/1.1 %d %s\r\nContent-Type: text/%s\r\nContent-Length: %d\r\n%sConnection: close\r\n\r\n%s",
                     codigo, codigo == 200 ? "OK" : "Not Found", extra && strstr(extra, "Nintendo") ? "html" : "plain",
                     (int)strlen(cuerpo), extra ? extra : "", cuerpo);
    wfc_con_envia(c, r, n);
    wfc_con_cierra(c);
}

// Un campo del formulario del nas (decodificado)
static int nas_campo(const char *form, const char *clave, char *out, int max) {
    size_t l = strlen(clave);
    for (const char *p = form; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, clave, l) || p[l] != '=') continue;
        const char *v = p + l + 1, *f = strchr(v, '&');
        int n = f ? (int)(f - v) : (int)strlen(v);
        while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == '\n')) n--;
        return b64_dec(v, n, out, max);
    }
    out[0] = 0;
    return -1;
}

static void nas_pon(char *r, int max, const char *clave, const char *valor) {
    char cod[256];
    b64_cod(valor, (int)strlen(valor), cod);
    size_t l = strlen(r);
    snprintf(r + l, (size_t)max - l, "%s%s=%s", l ? "&" : "", clave, cod);
}

static void nas(Con *c, const char *ruta, const char *form) {
    char accion[64], fecha[32], r[2048] = "";
    nas_campo(form, "action", accion, sizeof accion);
    for (char *p = accion; *p; p++) *p = (char)tolower((uint8_t)*p);
    time_t t = time(NULL);
    strftime(fecha, sizeof fecha, "%Y%m%d%H%M%S", gmtime(&t));
    if (wfc_traza > 0) {  // todo lo que manda la DS, decodificado
        WT("[wfc] nas %s:", ruta);
        for (const char *p = form; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
            const char *igual = strchr(p, '=');
            if (!igual) break;
            char clave[32], v[256];
            int lc = (int)(igual - p) < 31 ? (int)(igual - p) : 31;
            memcpy(clave, p, (size_t)lc); clave[lc] = 0;
            nas_campo(form, clave, v, sizeof v);
            for (char *q = v; *q; q++) if ((uint8_t)*q < 32) *q = '.';
            WT(" %s=%s", clave, v);
        }
        WT("\n");
    }
    if (strstr(ruta, "/pr")) {  // filtro de palabras: ninguna es mala
        char palabras[1024], cero[256];
        int n = nas_campo(form, "words", palabras, sizeof palabras), k = 1;
        for (int i = 0; i < n; i++) if (palabras[i] == '\t') k++;
        if (k > 255) k = 255;
        memset(cero, '0', (size_t)k); cero[k] = 0;
        nas_pon(r, sizeof r, "prwords", cero);
        nas_pon(r, sizeof r, "returncd", "000");
        nas_pon(r, sizeof r, "datetime", fecha);
    } else if (!strcmp(accion, "acctcreate") || !strcmp(accion, "login")) {
        // El DWC de MKDS (sdkver 001000) es de los primeros: con acctcreate (DS sin número de usuario) quiere lo
        // mismo que con login (token y reto para GameSpy), y el número de usuario se lo da después GameSpy
        // (gpcm, "\userid\"). Número de usuario: 13 cifras, menos de 2^43 (lo que cabe en el firmware).
        nas_campo(form, "userid", yo.userid, sizeof yo.userid);
        if (!strtoull(yo.userid, NULL, 10))
            snprintf(yo.userid, sizeof yo.userid, "%013llu",
                     1000000000000ull + ((unsigned long long)azar() << 20 ^ azar()) % 7000000000000ull);
        nas_campo(form, "gsbrcd", yo.gsbrcd, sizeof yo.gsbrcd);
        nas_campo(form, "ingamesn", yo.nombre, sizeof yo.nombre);
        if (!yo.nombre[0]) nas_campo(form, "devname", yo.nombre, sizeof yo.nombre);
        azar_texto(yo.reto_ac, 8, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
        char t[40];
        azar_texto(t, 32, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
        snprintf(yo.token, sizeof yo.token, "NDS%s", t);
        perfil_lee();
        wfc_firmware_pon_id(strtoull(yo.userid, NULL, 10));  // ds.c: que la consola lo recuerde
        nas_pon(r, sizeof r, "retry", "0");
        nas_pon(r, sizeof r, "returncd", strcmp(accion, "login") ? "002" : "001");
        nas_pon(r, sizeof r, "locator", "gamespy.com");
        nas_pon(r, sizeof r, "challenge", yo.reto_ac);
        nas_pon(r, sizeof r, "token", yo.token);
        nas_pon(r, sizeof r, "datetime", fecha);
    } else if (!strcmp(accion, "svcloc")) {
        char svc[16];
        nas_campo(form, "svc", svc, sizeof svc);
        nas_pon(r, sizeof r, "retry", "0");
        nas_pon(r, sizeof r, "returncd", "007");
        nas_pon(r, sizeof r, "statusdata", "Y");
        nas_pon(r, sizeof r, "svchost", "n/a");
        nas_pon(r, sizeof r, !strcmp(svc, "9000") || !strcmp(svc, "9001") ? "token" : "servicetoken", "NDSservicetoken");
        nas_pon(r, sizeof r, "datetime", fecha);
    } else {
        WT("[wfc] nas: no sé qué es '%s'\n", accion);
        nas_pon(r, sizeof r, "retry", "0");
        nas_pon(r, sizeof r, "returncd", "109");
        nas_pon(r, sizeof r, "datetime", fecha);
    }
    WT("[wfc] nas responde: %s\n", r);
    strcat(r, "\r\n");
    http_responde(c, 200, "NODE: wifiappe1\r\n", r);
}

static void http(Con *c, uint8_t *ent, int *nent) {
    char *e = (char *)ent;
    if (*nent >= (int)16383) *nent = 16383;
    e[*nent] = 0;  // hay sitio: ent es más grande que lo que se deja llenar
    char *fin = strstr(e, "\r\n\r\n");
    if (!fin) return;
    int cab = (int)(fin - e) + 4, largo = 0;
    const char *cl = strstr(e, "Content-Length:");
    if (!cl) cl = strstr(e, "content-length:");
    if (cl && cl < fin) largo = atoi(cl + 15);
    if (*nent < cab + largo) return;
    char metodo[8] = "", ruta[256] = "", host[128] = "";
    sscanf(e, "%7s %255s", metodo, ruta);
    const char *h = strstr(e, "Host:");
    if (h && h < fin) sscanf(h + 5, " %127[^\r\n]", host);
    WT("[wfc] HTTP %s %s%s (%d bytes)\n", metodo, host, ruta, largo);
    char *cuerpo = e + cab;
    cuerpo[largo] = 0;
    if (strstr(host, "conntest") || strstr(ruta, "conntest"))
        http_responde(c, 200, "X-Organization: Nintendo\r\nServer: BigIP\r\n",
                      "<html><head><title>HTML Page</title></head><body bgcolor=\"#FFFFFF\">This is test.html page</body></html>");
    else if (strstr(ruta, "/ac") || strstr(ruta, "/pr")) nas(c, ruta, cuerpo);
    else {
        WT("[wfc] HTTP: no sé qué es %s%s\n%s\n", host, ruta, cuerpo);
        http_responde(c, 404, NULL, "");
    }
    *nent = 0;
}

// ---- GameSpy Presence: gpcm (29900) y gpsp (29901) -----------------------------------------------------------------------
typedef struct { char reto[16]; } Gpcm;

static void gp_envia(Con *c, const char *m) {
    WT("[wfc] gp -> %s\n", m);
    wfc_con_envia(c, m, (int)strlen(m));
}

static void gpcm(Con *c, const char *m) {
    Gpcm *s = *wfc_con_datos(c);
    char id[16], r[1024];
    gs_valor(m, "id", id, sizeof id);
    if (!strncmp(m, "\\login\\", 7)) {
        char reto_ds[128], token[128], hash[33], prueba[33], sesion[16], lt[32];
        gs_valor(m, "challenge", reto_ds, sizeof reto_ds);
        gs_valor(m, "authtoken", token, sizeof token);
        md5_hex(yo.reto_ac, strlen(yo.reto_ac), hash);
        // prueba = md5(md5(reto del nas) + 48 espacios + authtoken + reto del servidor + reto de la DS + md5(reto del nas))
        char b[512];
        int n = snprintf(b, sizeof b, "%s%48s%s%s%s%s", hash, "", token, s->reto, reto_ds, hash);
        md5_hex(b, (size_t)n, prueba);
        snprintf(sesion, sizeof sesion, "%u", azar() % 100000000);
        azar_texto(lt, 22, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
        snprintf(r, sizeof r, "\\lc\\2\\sesskey\\%s\\proof\\%s\\userid\\%s\\profileid\\%u\\uniquenick\\dskr%08x\\lt\\%s__\\id\\%s\\final\\",
                 sesion, prueba, yo.userid, yo.perfil, yo.perfil, lt, id[0] ? id : "1");
        gp_envia(c, r);
        wfc_firmware_pon_id(strtoull(yo.userid, NULL, 10));  // otra vez: el juego reescribe el firmware al conectar
    } else if (!strncmp(m, "\\getprofile\\", 12)) {
        char sig[33];
        azar_texto(sig, 32, "0123456789abcdef");
        snprintf(r, sizeof r,
                 "\\pi\\\\profileid\\%u\\nick\\%s\\userid\\%s\\email\\%s@nds\\sig\\%s\\uniquenick\\dskr%08x\\pid\\11\\"
                 "lastname\\%s\\lon\\0.000000\\lat\\0.000000\\loc\\\\id\\%s\\final\\",
                 yo.perfil, yo.nombre, yo.userid, yo.gsbrcd, sig, yo.perfil, yo.apellido, id);
        gp_envia(c, r);
    } else if (!strncmp(m, "\\updatepro\\", 11)) {
        char a[64];
        gs_valor(m, "lastname", a, sizeof a);
        if (strstr(m, "\\lastname\\")) {
            snprintf(yo.apellido, sizeof yo.apellido, "%s", a);
            perfil_guarda(yo.gsbrcd[0] ? yo.gsbrcd : "-");
            if (strlen(a) > 9) perfil_guarda(a + 9);  // el gsbrcd que usará la próxima vez (va en el lastname)
        }
    } else if (!strncmp(m, "\\ka\\", 4)) {
        gp_envia(c, "\\ka\\\\final\\");
    } else if (!strncmp(m, "\\logout\\", 8)) {
        wfc_con_cierra(c);
    }
    // status, updatepro, updateui, addbuddy, authadd, delbuddy, bm: nada que contestar
}

static void gpsp(Con *c, const char *m) {
    if (!strncmp(m, "\\otherslist\\", 12)) gp_envia(c, "\\otherslist\\\\oldone\\\\final\\");
    else if (!strncmp(m, "\\search\\", 8)) gp_envia(c, "\\bsrdone\\\\final\\");
    else if (!strncmp(m, "\\valid\\", 7)) gp_envia(c, "\\vr\\0\\final\\");
    else if (!strncmp(m, "\\nicks\\", 7)) gp_envia(c, "\\nr\\0\\ndone\\\\final\\");
    else if (!strncmp(m, "\\others\\", 8)) gp_envia(c, "\\others\\\\odone\\\\final\\");
}

// Los mensajes de GameSpy terminan en "\final\"
static void gp_datos(Con *c, uint16_t puerto, uint8_t *ent, int *nent) {
    for (;;) {
        ent[*nent < 16383 ? *nent : 16383] = 0;
        char *f = strstr((char *)ent, "\\final\\");
        if (!f) return;
        *f = 0;
        WT("[wfc] gp <- %s\\final\\\n", (char *)ent);
        if (puerto == 29900) gpcm(c, (char *)ent);
        else gpsp(c, (char *)ent);
        int usado = (int)(f - (char *)ent) + 7;
        memmove(ent, ent + usado, (size_t)(*nent - usado));
        *nent -= usado;
    }
}

// ---- el master server de GameSpy: compartido por todos, en el servidor de Cloudflare (servidor/salas.js) ---------------
// Para buscar partida cada DS anuncia sus claves por QR2 (UDP 27900: dwc_pid, numplayers, etc.) y pide la lista de las
// demás al "server browser" (TCP 28910) con un filtro tipo SQL; para juntarse se mandan mensajes por el server browser,
// que el master entrega a la otra DS por QR2. Aquí se contesta a la DS como el master de GameSpy, pero la lista y los
// mensajes salen y llegan del servidor de Cloudflare. Las peticiones van en dos hilos (tardan): uno para lo que pide
// el juego y otro que se queda esperando a que lleguen mensajes (el servidor aguanta la petición hasta que hay alguno).
#define VERSION_WFC "1"  // subirla si cambia algo que haga que dos juegos distintos no puedan jugar juntos
#define SALAS_URL "https://dskart-salas.alontubecontacto.workers.dev"
static const char clave_juego[] = "yeJ3x8";  // la clave secreta de mariokartds en GameSpy (está en el ARM9)
int http_pide(const char *url, const char *cuerpo, char *r, int max, int espera);  // http.c

static CRITICAL_SECTION cs_ms;
static int ms_hilos;
static volatile int pide_latido, pide_lista, pide_adios, registrado, lista_lista;
static char latido_claves[2048];
static uint16_t latido_puerto;
static struct { char ruta[160]; char *cuerpo; } salidas[32];  // peticiones sueltas (mensajes, NAT negotiation)
static int nsalidas;
static char *lista_resp, *entrantes;
static uint32_t mi_ip, mi_ipl;
static uint16_t mi_puerto, mi_puertol;

static const char *ip_txt(uint32_t ip, char *b) {
    sprintf(b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}
static uint32_t txt_ip(const char *s) {
    unsigned a, b, c, d;
    return sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 ? a << 24 | b << 16 | c << 8 | d : 0;
}

static int ms_pide(const char *ruta, const char *cuerpo, char *r, int max, int espera) {
    char url[1024];
    const char *base = getenv("MKDS_SALAS_URL");
    snprintf(url, sizeof url, "%s%s", base && *base ? base : SALAS_URL, ruta);
    int c = http_pide(url, cuerpo, r, max, espera);
    if (strncmp(ruta, "/wfc/espera", 11) || c != 200 || r[0] != 'n' || r[1] != 'a') WT("[wfc] master %s -> %d %.200s\n", ruta, c, r);
    return c;
}

static void encola(const char *ruta, char *cuerpo) {  // 'cuerpo' (o NULL) pasa a ser del hilo
    EnterCriticalSection(&cs_ms);
    if (nsalidas < 32) {
        snprintf(salidas[nsalidas].ruta, sizeof salidas[0].ruta, "%s", ruta);
        salidas[nsalidas++].cuerpo = cuerpo;
    } else free(cuerpo);
    LeaveCriticalSection(&cs_ms);
}

static DWORD WINAPI hilo_peticiones(LPVOID x) {
    (void)x;
    char ruta[512], a[20], b[20], v[20];
    wfc_tunel_direcciones(&mi_ip, &mi_puerto, &mi_ipl, &mi_puertol);
    for (;;) {
        static char r[4096], claves[2048];
        if (pide_latido) {
            EnterCriticalSection(&cs_ms);
            pide_latido = 0;
            memcpy(claves, latido_claves, sizeof claves);
            uint16_t puerto = latido_puerto;
            LeaveCriticalSection(&cs_ms);
            snprintf(ruta, sizeof ruta, "/wfc/latido?vip=%s&puerto=%u&ip=%s&ipp=%u&ipl=%s&iplp=%u&v=" VERSION_WFC,
                     ip_txt(IP_DS, v), puerto, ip_txt(mi_ip, a), mi_puerto, ip_txt(mi_ipl, b), mi_puertol);
            if (ms_pide(ruta, claves, r, sizeof r, 5000) == 200) registrado = 1;
        }
        if (pide_lista) {
            pide_lista = 0;
            char *l = malloc(65536);
            snprintf(ruta, sizeof ruta, "/wfc/lista?vip=%s&v=" VERSION_WFC, ip_txt(IP_DS, v));
            if (ms_pide(ruta, NULL, l, 65536, 5000) != 200) l[0] = 0;
            EnterCriticalSection(&cs_ms);
            free(lista_resp);
            lista_resp = l;
            lista_lista = 1;
            LeaveCriticalSection(&cs_ms);
        }
        char *cuerpo = NULL;
        ruta[0] = 0;
        EnterCriticalSection(&cs_ms);
        if (nsalidas) {
            snprintf(ruta, sizeof ruta, "%s", salidas[0].ruta);
            cuerpo = salidas[0].cuerpo;
            memmove(salidas, salidas + 1, sizeof salidas[0] * (size_t)--nsalidas);
        }
        LeaveCriticalSection(&cs_ms);
        if (ruta[0]) {
            ms_pide(ruta, cuerpo, r, sizeof r, 5000);
            free(cuerpo);
        }
        if (pide_adios) {
            pide_adios = 0;
            registrado = 0;
            snprintf(ruta, sizeof ruta, "/wfc/adios?vip=%s", ip_txt(IP_DS, v));
            ms_pide(ruta, NULL, r, sizeof r, 5000);
        }
        Sleep(20);
    }
    return 0;
}

static DWORD WINAPI hilo_espera(LPVOID x) {
    (void)x;
    char ruta[128], v[20];
    for (;;) {
        if (!registrado) { Sleep(100); continue; }
        char *r = malloc(65536);
        snprintf(ruta, sizeof ruta, "/wfc/espera?vip=%s", ip_txt(IP_DS, v));
        int c = ms_pide(ruta, NULL, r, 65536, 30000);
        if (c == 200 && (r[0] == 'm' || (r[0] == 'n' && r[1] == ' '))) {
            EnterCriticalSection(&cs_ms);
            size_t l = entrantes ? strlen(entrantes) : 0;
            entrantes = realloc(entrantes, l + strlen(r) + 1);
            strcpy(entrantes + l, r);
            LeaveCriticalSection(&cs_ms);
        } else if (c != 200) {
            if (c == 404) pide_latido = 1;  // el servidor no se acuerda de mí (se ha reiniciado): otra vez
            Sleep(1000);
        }
        free(r);
    }
    return 0;
}

static void ms_arranca(void) {
    if (ms_hilos) return;
    ms_hilos = 1;
    InitializeCriticalSection(&cs_ms);
    CreateThread(NULL, 0, hilo_peticiones, NULL, 0, NULL);
    CreateThread(NULL, 0, hilo_espera, NULL, 0, NULL);
}

// ---- QR2 (UDP 27900): lo que anuncia esta DS ----
static struct {
    uint16_t puerto;          // el de la DS (es también el de la partida)
    uint8_t instancia[4];
    int registrado;
    char claves[2048];
    uint64_t latido;
} qr;
static struct { uint8_t d[1100]; int n, veces; uint64_t cuando; } a_la_ds[16];  // mensajes hasta que la DS los confirma
static uint64_t ahora;

static void qr_envia(uint8_t tipo, const uint8_t *d, int n) {
    uint8_t b[1200];
    b[0] = 0xFE; b[1] = 0xFD; b[2] = tipo;
    memcpy(b + 3, qr.instancia, 4);
    memcpy(b + 7, d, (size_t)n);
    wfc_udp_a_la_ds(IP_WFC, 27900, qr.puerto, b, 7 + n);
}

static void qr2(uint16_t po, const uint8_t *d, int n) {
    if (n < 5) return;
    switch (d[0]) {
    case 0x09: {  // ¿está el juego disponible? sí
        static const uint8_t si[7] = {0xFE, 0xFD, 0x09, 0, 0, 0, 0};
        WT("[wfc] available: sí\n");
        wfc_udp_a_la_ds(IP_WFC, 27900, po, si, 7);
        return;
    }
    case 0x03: {  // latido: las claves de la DS ("clave\0valor\0"... hasta una clave vacía)
        ms_arranca();
        qr.puerto = po;
        memcpy(qr.instancia, d + 1, 4);
        char claves[2048] = "";
        int k = 0, estado = 1;
        for (int o = 5; o < n && d[o] && k < 1900;) {
            const char *c = (const char *)d + o;
            int lc = (int)strnlen(c, (size_t)(n - o));
            if (o + lc + 1 >= n) break;
            const char *v = c + lc + 1;
            int lv = (int)strnlen(v, (size_t)(n - o - lc - 1));
            if (!strcmp(c, "statechanged")) estado = atoi(v);
            k += snprintf(claves + k, sizeof claves - (size_t)k, "%s\t%.100s\n", c, v);
            o += lc + 1 + lv + 1;
        }
        WT("[wfc] QR2 latido de %u (estado %d): %s", po, estado, claves);
        if (estado == 2) { pide_adios = 1; qr.registrado = 0; qr.claves[0] = 0; return; }  // ya no busca
        if (strcmp(claves, qr.claves) || ahora - qr.latido > 15000) {
            EnterCriticalSection(&cs_ms);
            strcpy(qr.claves, claves);
            strcpy(latido_claves, claves);
            latido_puerto = po;
            pide_latido = 1;
            LeaveCriticalSection(&cs_ms);
            qr.latido = ahora;
        }
        if (!qr.registrado) {  // el reto; lleva la IP y el puerto "públicos" de la DS (los de la red virtual)
            char reto[40];
            azar_texto(reto, 6, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
            snprintf(reto + 6, sizeof reto - 6, "00%08X%04X", IP_DS, po);
            qr_envia(0x01, (const uint8_t *)reto, (int)strlen(reto) + 1);
        }
        return;
    }
    case 0x01:  // la respuesta al reto: registrada
        qr.registrado = 1;
        qr_envia(0x0A, NULL, 0);
        return;
    case 0x07:  // la DS ha recibido un mensaje
        for (int i = 0; i < 16; i++)
            if (a_la_ds[i].n && n >= 9 && !memcmp(a_la_ds[i].d, d + 5, 4)) {
                a_la_ds[i].n = 0;
                WT("[wfc] QR2: la DS confirma el mensaje (%llu ms)\n", (unsigned long long)ahora);
            }
        return;
    case 0x08:  // sigo aquí
        return;
    }
    WT("[wfc] QR2: no sé qué es %02X (%d bytes)\n", d[0], n);
}

// un mensaje de otra DS (llega por el servidor): a esta DS por QR2, repetido hasta que lo confirme
static void mensaje_a_la_ds(const uint8_t *d, int n) {
    if (!qr.puerto || n > 1024) return;
    int i = 0;
    while (i < 15 && a_la_ds[i].n) i++;
    uint32_t clave = azar();
    memcpy(a_la_ds[i].d, &clave, 4);
    memcpy(a_la_ds[i].d + 4, d, (size_t)n);
    a_la_ds[i].n = 4 + n;
    a_la_ds[i].veces = 0;
    a_la_ds[i].cuando = 0;
}

// ---- el filtro de la lista: "dwc_mver = 3 and dwc_pid != 123 and (numplayers < 4 or ...)" ----
typedef struct { const char *p, *claves; int prof; } Filtro;

static void clave_de(const char *claves, const char *k, int kl, char *out, int max) {
    out[0] = 0;
    for (const char *p = claves; (p = strchr(p, '\t')) != NULL;) {
        p++;
        const char *f = strchr(p, '\t');
        if (!f) return;
        const char *v = f + 1, *fv = strchr(v, '\t');
        int lv = fv ? (int)(fv - v) : (int)strlen(v);
        if ((int)(f - p) == kl && !strncmp(p, k, (size_t)kl)) {
            if (lv > max - 1) lv = max - 1;
            memcpy(out, v, (size_t)lv);
            out[lv] = 0;
            return;
        }
        p = fv ? fv : v + lv;
    }
}
static void f_salta(Filtro *f) { while (*f->p == ' ') f->p++; }
static int f_palabra(Filtro *f, const char *w) {
    f_salta(f);
    size_t l = strlen(w);
    if (!strncmp(f->p, w, l) && !isalnum((uint8_t)f->p[l]) && f->p[l] != '_') { f->p += l; return 1; }
    return 0;
}
static int f_operando(Filtro *f, char *out, int max) {  // 1 si se ha leído algo
    f_salta(f);
    int k = 0;
    if (*f->p == '\'') {
        f->p++;
        while (*f->p && *f->p != '\'') { if (k < max - 1) out[k++] = *f->p; f->p++; }
        if (*f->p) f->p++;
        out[k] = 0;
        return 1;
    }
    const char *i = f->p;
    if (*f->p == '-') f->p++;
    while (isalnum((uint8_t)*f->p) || *f->p == '_' || *f->p == '.') f->p++;
    if (f->p == i) return 0;
    if (isdigit((uint8_t)*i) || *i == '-') {
        k = (int)(f->p - i) < max - 1 ? (int)(f->p - i) : max - 1;
        memcpy(out, i, (size_t)k);
        out[k] = 0;
    } else clave_de(f->claves, i, (int)(f->p - i), out, max);
    return 1;
}
static int f_numero(const char *s) {
    if (*s == '-') s++;
    if (!*s) return 0;
    while (*s) if (!isdigit((uint8_t)*s++)) return 0;
    return 1;
}
static int f_expr(Filtro *f);
static int f_factor(Filtro *f) {
    if (++f->prof > 50) return 0;
    if (f_palabra(f, "not")) return !f_factor(f);
    f_salta(f);
    if (*f->p == '(') {
        f->p++;
        int r = f_expr(f);
        f_salta(f);
        if (*f->p == ')') f->p++;
        return r;
    }
    char a[128], b[128], op[4] = "";
    if (!f_operando(f, a, sizeof a)) { if (*f->p) f->p++; return 0; }
    f_salta(f);
    int k = 0;
    while (k < 2 && strchr("=!<>", *f->p) && *f->p) op[k++] = *f->p++;
    op[k] = 0;
    if (!k) { if (f_palabra(f, "like")) strcpy(op, "~"); else return a[0] && strcmp(a, "0"); }
    if (!f_operando(f, b, sizeof b)) return 0;
    double c;
    if (f_numero(a) && f_numero(b)) c = atof(a) - atof(b);
    else c = strcmp(a, b);
    if (!strcmp(op, "=") || !strcmp(op, "==")) return c == 0;
    if (!strcmp(op, "!=") || !strcmp(op, "<>")) return c != 0;
    if (!strcmp(op, "<")) return c < 0;
    if (!strcmp(op, ">")) return c > 0;
    if (!strcmp(op, "<=")) return c <= 0;
    if (!strcmp(op, ">=")) return c >= 0;
    if (!strcmp(op, "~")) {  // like: solo con % al principio y/o al final
        char pat[128];
        snprintf(pat, sizeof pat, "%s", b);
        size_t l = strlen(pat);
        int ini = pat[0] == '%', fin = l > 0 && pat[l - 1] == '%';
        if (fin) pat[--l] = 0;
        const char *q = pat + ini;
        if (ini && fin) return strstr(a, q) != NULL;
        if (ini) return strlen(a) >= strlen(q) && !strcmp(a + strlen(a) - strlen(q), q);
        if (fin) return !strncmp(a, q, strlen(q));
        return !strcmp(a, q);
    }
    return 0;
}
static int f_termino(Filtro *f) {
    int r = f_factor(f);
    while (f_palabra(f, "and")) { int s = f_factor(f); r = r && s; }
    return r;
}
static int f_expr(Filtro *f) {
    int r = f_termino(f);
    while (f_palabra(f, "or")) { int s = f_termino(f); r = r || s; }
    return r;
}
static int pasa_filtro(const char *filtro, const char *claves) {
    Filtro f = {filtro, claves, 0};
    f_salta(&f);
    return !*f.p || f_expr(&f);
}

// ---- el server browser (TCP 28910) ----
// Las respuestas van cifradas con el cifrado de GameSpy (GOA, que es Sapphire II), con una clave que sale del reto
// que manda la DS, unos bytes que pone el servidor y la clave secreta del juego.
typedef struct { uint8_t cartas[256], rotor, trinquete, avalancha, ult_claro, ult_cifrado; } Cripto;
static uint8_t cr_azar(Cripto *c, int limite, const uint8_t *k, int kn, uint8_t *suma, int *pos) {
    if (!limite) return 0;
    unsigned mascara = 1, u, intentos = 0;
    while (mascara < (unsigned)limite) mascara = (mascara << 1) + 1;
    do {
        *suma = (uint8_t)(c->cartas[*suma] + k[(*pos)++]);
        if (*pos >= kn) { *pos = 0; *suma = (uint8_t)(*suma + kn); }
        u = mascara & *suma;
        if (++intentos > 11) u %= (unsigned)limite;
    } while (u > (unsigned)limite);
    return (uint8_t)u;
}
static void cr_inicia(Cripto *c, const uint8_t *k, int kn) {
    for (int i = 0; i < 256; i++) c->cartas[i] = (uint8_t)i;
    uint8_t suma = 0;
    int pos = 0;
    for (int i = 255; i >= 0; i--) {
        uint8_t t = cr_azar(c, i, k, kn, &suma, &pos), x = c->cartas[i];
        c->cartas[i] = c->cartas[t];
        c->cartas[t] = x;
    }
    c->rotor = c->cartas[1];
    c->trinquete = c->cartas[3];
    c->avalancha = c->cartas[5];
    c->ult_claro = c->cartas[7];
    c->ult_cifrado = c->cartas[suma];
}
static uint8_t cr_cifra(Cripto *c, uint8_t b) {
    uint8_t *k = c->cartas, x;
    c->trinquete = (uint8_t)(c->trinquete + k[c->rotor++]);
    x = k[c->ult_cifrado];
    k[c->ult_cifrado] = k[c->trinquete];
    k[c->trinquete] = k[c->ult_claro];
    k[c->ult_claro] = k[c->rotor];
    k[c->rotor] = x;
    c->avalancha = (uint8_t)(c->avalancha + k[x]);
    c->ult_cifrado = (uint8_t)(b ^ k[(k[c->avalancha] + k[c->rotor]) & 0xFF] ^
                               k[k[(k[c->ult_claro] + k[c->ult_cifrado] + k[c->trinquete]) & 0xFF]]);
    c->ult_claro = b;
    return c->ult_cifrado;
}

typedef struct {
    Con *con;
    int cifrando, esperando;
    uint8_t reto[8];
    Cripto cr;
    char campos[512], filtro[1024];
} Sb;
static Sb *sbs[4];

static void sb_envia(Sb *s, const uint8_t *d, int n) {
    uint8_t *b = malloc((size_t)n + 32);
    int k = 0;
    if (!s->cifrando) {  // cabecera: (largo ^ 0xEC), bytes al azar, (largo de la clave ^ 0xEA), clave
        uint8_t clave[14], reto[8];
        b[k++] = 2 ^ 0xEC;
        b[k++] = (uint8_t)azar();
        b[k++] = (uint8_t)azar();
        b[k++] = sizeof clave ^ 0xEA;
        for (int i = 0; i < (int)sizeof clave; i++) b[k++] = clave[i] = (uint8_t)azar();
        memcpy(reto, s->reto, 8);
        for (int i = 0; i < (int)sizeof clave; i++)
            reto[(i * clave_juego[i % 6]) % 8] ^= (uint8_t)(reto[i % 8] ^ clave[i]);
        cr_inicia(&s->cr, reto, 8);
        s->cifrando = 1;
    }
    for (int i = 0; i < n; i++) b[k++] = cr_cifra(&s->cr, d[i]);
    wfc_con_envia(s->con, b, k);
    free(b);
}

static void sb_lista(Sb *s, const char *lista) {
    static uint8_t o[32768];
    char campos[32][40];
    int nc = 0, n = 0, servidores = 0;
    for (const char *p = s->campos; *p && nc < 32;) {  // "\clave1\clave2..."
        if (*p == '\\') { p++; continue; }
        int k = 0;
        while (*p && *p != '\\') { if (k < 39) campos[nc][k++] = *p; p++; }
        campos[nc++][k] = 0;
    }
    p32(o, IP_DS);  // "tu IP pública" (la de la red virtual: sin NAT)
    p16(o + 4, 6500);
    n = 6;
    o[n++] = (uint8_t)nc;
    for (int i = 0; i < nc; i++) {
        o[n++] = 0;  // tipo: texto
        n += sprintf((char *)o + n, "%s", campos[i]) + 1;
    }
    o[n++] = 0;  // sin "valores populares"
    for (const char *l = lista; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        char vip[20], ip[20], ipl[20];
        int puerto, ipp, iplp;
        if (l[0] != 's' || sscanf(l, "s %19s %d %19s %d %19s %d", vip, &puerto, ip, &ipp, ipl, &iplp) != 6) continue;
        const char *fin = strchr(l, '\n');
        size_t lc = fin ? (size_t)(fin - l) : strlen(l);
        char *claves = malloc(lc + 1);
        memcpy(claves, l, lc);
        claves[lc] = 0;
        char *t = strchr(claves, '\t');
        uint32_t v = txt_ip(vip);
        wfc_tunel_pon(v, txt_ip(ip), (uint16_t)ipp, txt_ip(ipl), (uint16_t)iplp);
        if (t && pasa_filtro(s->filtro, t) && n < (int)sizeof o - 2048) {
            o[n++] = 2 | 16 | 32 | 64;  // IP privada, puerto, puerto privado, claves
            p32(o + n, v); p16(o + n + 4, (uint16_t)puerto);
            p32(o + n + 6, v); p16(o + n + 10, (uint16_t)puerto);
            n += 12;
            for (int i = 0; i < nc; i++) {
                char val[128];
                clave_de(t, campos[i], (int)strlen(campos[i]), val, sizeof val);
                o[n++] = 0xFF;
                n += sprintf((char *)o + n, "%s", val) + 1;
            }
            servidores++;
        }
        free(claves);
    }
    o[n++] = 0;
    p32(o + n, 0xFFFFFFFF);
    n += 4;
    WT("[wfc] server browser: lista con %d\n", servidores);
    sb_envia(s, o, n);
}

static void sb_datos(Con *c, uint8_t *ent, int *nent) {
    Sb *s = *wfc_con_datos(c);
    while (*nent >= 3) {
        int largo = g16(ent);
        if (largo < 3) { *nent = 0; return; }
        if (*nent < largo) return;
        const uint8_t *p = ent;
        if (p[2] == 0x00 && largo > 9) {  // lista: versiones(2), versión del juego(4), juego, juego, reto(8), filtro, claves, opciones
            int o = 9;
            o += (int)strnlen((const char *)p + o, (size_t)(largo - o)) + 1;
            o += (int)strnlen((const char *)p + o, (size_t)(largo - o)) + 1;
            if (o + 8 <= largo) {
                memcpy(s->reto, p + o, 8);
                o += 8;
                snprintf(s->filtro, sizeof s->filtro, "%.*s", largo - o, (const char *)p + o);
                o += (int)strnlen((const char *)p + o, (size_t)(largo - o)) + 1;
                snprintf(s->campos, sizeof s->campos, "%.*s", largo > o ? largo - o : 0, (const char *)p + o);
                o += (int)strnlen((const char *)p + o, (size_t)(largo - o)) + 1;
                uint32_t opciones = o + 4 <= largo ? g32(p + o) : 0;
                WT("[wfc] server browser: lista con filtro \"%s\", claves \"%s\", opciones %X\n", s->filtro, s->campos,
                   opciones);
                if (opciones & 2) {
                    // NO_SERVER_LIST: la DS solo quiere la conexión para mandar mensajes. Si le llegaba una lista, la
                    // leía como mensajes sueltos del servidor y dejaba de atender la red (se perdía la partida)
                    if (!s->cifrando) sb_envia(s, NULL, 0);  // solo la cabecera del cifrado
                } else {
                    s->esperando = 1;
                    pide_lista = 1;
                }
            }
        } else if (p[2] == 0x02 && largo >= 9) {  // un mensaje para otra DS (ip, puerto, datos)
            uint32_t a = g32(p + 3);
            int nd = largo - 9;
            char *hex = malloc((size_t)nd * 2 + 1);
            for (int i = 0; i < nd; i++) sprintf(hex + 2 * i, "%02x", p[9 + i]);
            hex[nd * 2] = 0;
            WT("[wfc] server browser: mensaje de %d bytes para %u.%u.%u.%u:%u: %s\n", nd, a >> 24, (a >> 16) & 255,
               (a >> 8) & 255, a & 255, g16(p + 7), hex);
            char ruta[160], x[20], y[20];
            snprintf(ruta, sizeof ruta, "/wfc/manda?de=%s&a=%s", ip_txt(IP_DS, x), ip_txt(a, y));
            encola(ruta, hex);
        } else if (p[2] != 0x03) {
            WT("[wfc] server browser: no sé qué es %02X (%d bytes)\n", p[2], largo);
        }
        memmove(ent, ent + largo, (size_t)(*nent - largo));
        *nent -= largo;
    }
}

// ---- NAT negotiation (UDP 27901) ----
// Para conectar dos DS, las dos mandan INIT con el mismo número ("cookie") y cuál de las dos es (0 o 1); cuando
// están las dos, el servidor le dice a cada una dónde está la otra (CONNECT). Aquí los dos lados se juntan en el
// servidor de Cloudflare y, como en la red virtual no hay NAT, la dirección es la IP virtual y el puerto de la partida.
static const uint8_t nn_magia[6] = {0xFD, 0xFC, 0x1E, 0x66, 0x6A, 0xB2};
static struct { uint8_t cab[12]; uint16_t puerto; int veces; uint64_t cuando; uint32_t vip; uint16_t su_puerto; } nns[4];

static void natneg(uint16_t po, const uint8_t *d, int n) {
    if (n < 12 || memcmp(d, nn_magia, 6)) return;
    uint8_t r[128];
    char ruta[160], x[20];
    switch (d[7]) {
    case 0x00: {  // INIT: tipo de puerto, índice, usa el puerto del juego, IP y puerto locales
        if (n < 21) return;
        WT("[wfc] natneg INIT cookie %08X índice %u tipo %u puerto del juego %u desde %u\n", g32(d + 8), d[13], d[12], d[14], po);
        memcpy(r, d, 14);
        r[7] = 0x01;
        memcpy(r + 14, "\xFF\xFF\x6D\x16\xB5\x7D\xEA", 7);
        wfc_udp_a_la_ds(IP_WFC, 27901, po, r, 21);
        if (d[12] != (d[14] ? 0 : 1)) return;  // solo cuenta el que sale del socket de la partida
        for (int i = 0; i < 4; i++) if (nns[i].puerto && !memcmp(nns[i].cab + 8, d + 8, 4)) return;  // ya pedido
        int i = 0;
        for (int k = 1; k < 4; k++) if (nns[k].cuando < nns[i].cuando) i = k;
        memset(&nns[i], 0, sizeof nns[i]);
        memcpy(nns[i].cab, d, 12);
        nns[i].puerto = po;
        nns[i].cuando = ahora;
        ms_arranca();
        snprintf(ruta, sizeof ruta, "/wfc/natneg?vip=%s&cookie=%08X&indice=%u&puerto=%u", ip_txt(IP_DS, x), g32(d + 8), d[13], po);
        encola(ruta, NULL);
        return;
    }
    case 0x06:  // CONNECT_ACK
        for (int i = 0; i < 4; i++) if (!memcmp(nns[i].cab + 8, d + 8, 4)) nns[i].veces = 99;
        return;
    case 0x0A:  // ADDRESS_CHECK: la dirección "pública" es la de la red virtual
        if (n < 15) return;
        memcpy(r, d, 15);
        p32(r + 15, IP_DS);
        p16(r + 19, po);
        r[7] = 0x0B;
        wfc_udp_a_la_ds(IP_WFC, 27901, po, r, 21);
        return;
    case 0x0D:  // REPORT: cómo ha ido
        if (n < 21) return;
        WT("[wfc] natneg REPORT cookie %08X resultado %u\n", g32(d + 8), d[14]);
        memcpy(r, d, 21);
        r[7] = 0x0E;
        r[14] = 0;
        wfc_udp_a_la_ds(IP_WFC, 27901, po, r, 21);
        return;
    }
    WT("[wfc] natneg: no sé qué es %02X (%d bytes)\n", d[7], n);
}

static void natneg_atiende(void) {  // CONNECT a la DS hasta que conteste
    for (int i = 0; i < 4; i++) {
        if (!nns[i].vip || nns[i].veces >= 5 || ahora - nns[i].cuando < 500) continue;
        nns[i].veces++;
        nns[i].cuando = ahora;
        uint8_t r[20];
        memcpy(r, nns[i].cab, 12);
        r[7] = 0x05;
        p32(r + 12, nns[i].vip);
        p16(r + 16, nns[i].su_puerto);
        r[18] = 0x42;  // "tengo tus datos"
        r[19] = 0;
        wfc_udp_a_la_ds(IP_WFC, 27901, nns[i].puerto, r, 20);
    }
}

// ---- lo que llega del router ------------------------------------------------------------------------------------------------
void wfc_servicio_abre(Con *c, uint16_t puerto) {
    WT("[wfc] servicio: conexión al puerto %u\n", puerto);
    if (puerto == 28910) {
        ms_arranca();
        Sb *s = calloc(1, sizeof *s);
        s->con = c;
        *wfc_con_datos(c) = s;
        for (int i = 0; i < 4; i++) if (!sbs[i]) { sbs[i] = s; break; }
    }
    if (puerto == 29900) {
        Gpcm *s = calloc(1, sizeof *s);
        *wfc_con_datos(c) = s;
        azar_texto(s->reto, 10, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        char r[128];
        snprintf(r, sizeof r, "\\lc\\1\\challenge\\%s\\id\\1\\final\\", s->reto);
        gp_envia(c, r);
    }
}

void wfc_servicio_datos(Con *c, uint16_t puerto, uint8_t *ent, int *nent) {
    if (puerto == 80) http(c, ent, nent);
    else if (puerto == 29900 || puerto == 29901) gp_datos(c, puerto, ent, nent);
    else if (puerto == 28910) sb_datos(c, ent, nent);
    else {
        WT("[wfc] servicio %u: %d bytes:", puerto, *nent);
        for (int i = 0; i < *nent && i < 256; i++) WT(" %02X", ent[i]);
        WT("\n");
        *nent = 0;
    }
}

void wfc_servicio_cierra(Con *c) {
    for (int i = 0; i < 4; i++)
        if (sbs[i] && sbs[i]->con == c) sbs[i] = NULL;  // (el Sb lo libera wfc.c: son sus datos)
}

void wfc_servicio_udp(uint16_t po, uint16_t pd, const uint8_t *d, int n) {
    if (pd == 27900) { qr2(po, d, n); return; }
    if (pd == 27901) { natneg(po, d, n); return; }
    WT("[wfc] UDP %u -> %u: %d bytes:", po, pd, n);
    for (int i = 0; i < n && i < 128; i++) WT(" %02X", d[i]);
    WT("\n");
}

void wfc_servicio_atiende(uint64_t ms) {
    ahora = ms;
    if (wfc_traza > 0) {  // depuración: el estado de la búsqueda de partida del DWC (+0x198 de su control, en 021B566C)
        extern uint8_t g_main[0x400000];
        static int antes = -1, antes_n = -1;
        uint32_t p, e;
        memcpy(&p, g_main + (0x021B566C & 0x3FFFFF), 4);
        if ((p >> 24) == 0x02) {
            memcpy(&e, g_main + ((p + 0x198) & 0x3FFFFF), 4);
            int n = g_main[(p + 0xD) & 0x3FFFFF];  // los conectados
            if ((int)e != antes || n != antes_n)
                WT("[wfc] DWC: estado %d -> %d, conectados %d (%llu ms)\n", antes, (int)e, n, (unsigned long long)ms);
            antes = (int)e;
            antes_n = n;
        }
    }
    if (!ms_hilos) return;
    char *l = NULL;
    EnterCriticalSection(&cs_ms);
    if (lista_lista) {  // la lista que pidió el server browser
        lista_lista = 0;
        l = lista_resp;
        lista_resp = NULL;
    }
    char *m = entrantes;
    entrantes = NULL;
    LeaveCriticalSection(&cs_ms);
    if (l) {
        for (int i = 0; i < 4; i++) if (sbs[i] && sbs[i]->esperando) { sbs[i]->esperando = 0; sb_lista(sbs[i], l); }
        free(l);
    }
    for (const char *p = m; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {  // mensajes de otras DS
        char vip[20], ip[20], ipl[20], hex[2100];
        int puerto, ipp, iplp;
        unsigned cookie;
        if (sscanf(p, "n %X %19s %d %19s %d %19s %d", &cookie, vip, &puerto, ip, &ipp, ipl, &iplp) == 7) {  // natneg
            wfc_tunel_pon(txt_ip(vip), txt_ip(ip), (uint16_t)ipp, txt_ip(ipl), (uint16_t)iplp);
            for (int i = 0; i < 4; i++)
                if (nns[i].puerto && g32(nns[i].cab + 8) == cookie) {
                    nns[i].vip = txt_ip(vip);
                    nns[i].su_puerto = (uint16_t)puerto;
                    nns[i].cuando = 0;
                    WT("[wfc] natneg %08X: la otra DS es %s:%d\n", cookie, vip, puerto);
                }
            continue;
        }
        if (sscanf(p, "m %19s %d %19s %d %19s %d %2099s", vip, &puerto, ip, &ipp, ipl, &iplp, hex) != 7) continue;
        wfc_tunel_pon(txt_ip(vip), txt_ip(ip), (uint16_t)ipp, txt_ip(ipl), (uint16_t)iplp);
        uint8_t d[1050];
        int n = 0;
        for (int i = 0; hex[i] && hex[i + 1] && n < (int)sizeof d; i += 2) {
            unsigned x;
            sscanf(hex + i, "%2x", &x);
            d[n++] = (uint8_t)x;
        }
        WT("[wfc] mensaje de %s (%d bytes): %s\n", vip, n, hex);
        mensaje_a_la_ds(d, n);
    }
    free(m);
    natneg_atiende();
    for (int i = 0; i < 16; i++) {  // a la DS hasta que diga que lo tiene (3 veces como mucho)
        if (!a_la_ds[i].n || ms - a_la_ds[i].cuando < 1000) continue;
        if (a_la_ds[i].veces++ >= 3) { a_la_ds[i].n = 0; continue; }
        a_la_ds[i].cuando = ms;
        qr_envia(0x06, a_la_ds[i].d, a_la_ds[i].n);
        WT("[wfc] QR2: mensaje a la DS (vez %d, %d bytes, %llu ms)\n", a_la_ds[i].veces, a_la_ds[i].n - 4,
           (unsigned long long)ms);
    }
}
