// Rich Presence de Discord: a qué se está jugando, debajo del nombre del jugador en Discord.
//
// El juego habla con la aplicación de Discord de este mismo PC por su conexión local (una tubería con nombre en
// Windows, un socket Unix en Linux y macOS), sin ninguna biblioteca: cada mensaje es op (u32), largo (u32) y JSON.
//   op 0  saludo: {"v":1,"client_id":"..."}       op 1  órdenes (SET_ACTIVITY) y respuestas
//   op 2  adiós                                   op 3 / 4  ping / pong
// Nada va a ningún servidor nuestro, y si Discord no está abierto no pasa nada (se prueba otra vez cada 15 s).
// DISCORD=0 en ajustes.ini (launcher > Ajustes, o menú > Juego) lo quita: se borra la actividad y se cierra la conexión.
// El modo de juego sale siempre; DISCORD_CIRCUITO, DISCORD_VUELTA y DISCORD_POSICION a 0 quitan cada cosa.
//
// Todo va en su propio hilo, que una vez por segundo mira la memoria del juego (solo lee):
//   *(021759C0)  la carrera: +0 circuito (tabla de 02154228), +8 modo (0 Grand Prix, 1 contrarreloj, 2 batalla, 3 VS,
//                4 misión, 5 Conexión Wi-Fi), +10 cilindrada (0-3; 3 = espejo), +3D0 karts, +3D1 jugadores
//   *(0217561C)  los karts, de 0x8C: +44 bit 0 = el de este jugador, +4E vuelta, +50 puesto (4 bits). 0 fuera de carrera.
// El puesto cambia mucho y Discord limita los cambios: como mucho uno cada 5 s.
#include "compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#endif

#define APLICACION "1574853807458754580"  // el Application ID (portal de desarrolladores de Discord); no es secreto

extern uint8_t g_main[0x400000];
int online_estado(char *codigo, int *jugadores);  // wifi.c: 0 sin sala
int wifi_en_que(void);                            // wifi.c: 1 en un grupo local, 2 en la Conexión Wi-Fi

static volatile int encendido = -1, ver_circuito, ver_vuelta, ver_posicion;
void discord_pon(int si) { encendido = si; }
void discord_ver(int circuito, int vuelta, int posicion) {
    ver_circuito = circuito;
    ver_vuelta = vuelta;
    ver_posicion = posicion;
}

// ---- textos ---------------------------------------------------------------------------------------------------------
enum { ES, EN, FR, DE, IT };
static int idioma(void) {
    const char *v = getenv("MKDS_LAUNCHER_IDIOMA");
    static const char *id[] = {"es", "en", "fr", "de", "it"};
    for (int i = 0; v && i < 5; i++)
        if (!strcmp(v, id[i])) return i;
    v = getenv("MKDS_IDIOMA");  // el del juego: 1 inglés, 2 francés, 3 alemán, 4 italiano, 5 español
    static const int juego[] = {EN, EN, FR, DE, IT, ES};
    int n = v ? atoi(v) : 1;
    return n >= 1 && n <= 5 ? juego[n] : EN;
}
typedef struct { const char *t[5]; } Texto;
static const Texto MENU = {{"En el menú", "In the menus", "Dans les menus", "Im Menü", "Nel menu"}};
static const Texto MODOS[5] = {
    {{"Grand Prix", "Grand Prix", "Grand Prix", "Grand Prix", "Grand Prix"}},
    {{"Contrarreloj", "Time Trials", "Contre-la-montre", "Zeitfahren", "Prova a tempo"}},
    {{"Batalla", "Battle", "Bataille", "Schlacht", "Battaglia"}},
    {{"Versus", "VS", "VS", "Versus", "Sfida"}},
    {{"Misiones", "Missions", "Missions", "Missionen", "Missioni"}},
};
static const Texto ONLINE = {{"Online", "Online", "En ligne", "Online", "Online"}};
static const Texto LOCAL = {{"Multijugador", "Multiplayer", "Multijoueur", "Mehrspieler", "Multigiocatore"}};
static const Texto ESPEJO = {{"espejo", "mirror", "miroir", "Spiegel", "specchio"}};
static const Texto VUELTA = {{"Vuelta", "Lap", "Tour", "Runde", "Giro"}};
static const Texto META = {{"En la meta", "Finished", "Arrivé", "Im Ziel", "Al traguardo"}};
static const Texto SALA = {{"En una sala", "In a room", "Dans un salon", "In einem Raum", "In una stanza"}};

// los circuitos, por su número (los que no están no se pueden elegir en el juego)
static const struct { uint8_t id; Texto n; } CIRCUITOS[] = {
    {20, {{"Circuito en 8", "Figure-8 Circuit", "Circuit en 8", "Achterpiste", "Ottotornante"}}},
    {22, {{"Cataratas de Yoshi", "Yoshi Falls", "Cascades Yoshi", "Yoshi-Kaskaden", "Cascate di Yoshi"}}},
    {31, {{"Playa Cheep Cheep", "Cheep Cheep Beach", "Plage Cheep Cheep", "Cheep Cheep-Strand", "Spiaggia Smack"}}},
    {18, {{"Mansión de Luigi", "Luigi's Mansion", "Manoir de Luigi", "Luigi's Mansion", "Palazzo di Luigi"}}},
    {27, {{"Desierto Sol-Sol", "Desert Hills", "Désert du Soleil", "Glühheiße Wüste", "Deserto Picchiasol"}}},
    {28, {{"Ciudad Delfino", "Delfino Square", "Quartier Delfino", "Piazzale Delfino", "Borgo Delfino"}}},
    {33, {{"Pinball Waluigi", "Waluigi Pinball", "Flipper Waluigi", "Waluigi-Flipper", "Flipper di Waluigi"}}},
    {24, {{"Colinas Champiñón", "Shroom Ridge", "Corniche Champignon", "Pilz-Pass", "Colli Fungo"}}},
    {30, {{"DK Alpino", "DK Pass", "Alpes DK", "DK Alpin", "Vette di DK"}}},
    {17, {{"Reloj Tictac", "Tick-Tock Clock", "Horloge Tic-Tac", "Ticktack-Trauma", "Pista Tictac"}}},
    {25, {{"Circuito Mario", "Mario Circuit", "Circuit Mario", "Marios Piste", "Circuito di Mario"}}},
    {19, {{"Fortaleza Aérea", "Airship Fortress", "Bateau volant", "Fliegende Festung", "Fortezza Volante"}}},
    {34, {{"Estadio Wario", "Wario Stadium", "Stade Wario", "Wario-Arena", "Stadio di Wario"}}},
    {26, {{"Jardín de Peach", "Peach Gardens", "Jardin Peach", "Peachs Schlossgarten", "Giardino di Peach"}}},
    {32, {{"Castillo de Bowser", "Bowser Castle", "Château de Bowser", "Bowsers Festung", "Castello di Bowser"}}},
    {29, {{"Senda Arco Iris", "Rainbow Road", "Route Arc-en-ciel", "Regenbogen-Boulevard", "Pista Arcobaleno"}}},
    {10, {{"SNES Circuito Mario", "SNES Mario Circuit 1", "SNES Circuit Mario 1", "SNES Marios Piste 1", "SNES Circuito di Mario 1"}}},
    {11, {{"N64 Granja Mu-Mu", "N64 Moo Moo Farm", "N64 Ferme Meuh Meuh", "N64 Kuhmuh-Farm", "N64 Fattoria Muu Muu"}}},
    {13, {{"GBA Circuito Peach", "GBA Peach Circuit", "GBA Circuit Peach", "GBA Peachs Piste", "GBA Pista di Peach"}}},
    {14, {{"GCN Circuito de Luigi", "GCN Luigi Circuit", "GCN Circuit Luigi", "GCN Luigis Piste", "GCN Circuito di Luigi"}}},
    {35, {{"SNES Prado Rosquilla", "SNES Donut Plains 1", "SNES Plaine Donut 1", "SNES Donut-Ebene 1", "SNES Pianura Ciambella"}}},
    {16, {{"N64 Circuito Nevado", "N64 Frappe Snowland", "N64 Route Glagla", "N64 Polar-Parcours", "N64 Circuito Innevato"}}},
    {12, {{"GBA Castillo de Bowser", "GBA Bowser Castle 2", "GBA Château de Bowser 2", "GBA Bowsers Festung 2", "GBA Castello di Bowser 2"}}},
    {9, {{"GCN Parque Bebé", "GCN Baby Park", "GCN Parc Baby", "GCN Baby-Park", "GCN Baby Park"}}},
    {15, {{"SNES Playa Koopa", "SNES Koopa Beach 2", "SNES Plage Koopa 2", "SNES Koopa-Strand 2", "SNES Spiaggia Koopa 2"}}},
    {36, {{"N64 Monte Chocolate", "N64 Choco Mountain", "N64 Montagne Choco", "N64 Schoko-Sumpf", "N64 Cioccocanyon"}}},
    {37, {{"GBA Circuito de Luigi", "GBA Luigi Circuit", "GBA Circuit Luigi", "GBA Luigis Piste", "GBA Circuito di Luigi"}}},
    {38, {{"GCN Puente Champiñón", "GCN Mushroom Bridge", "GCN Pont Champignon", "GCN Pilz-Brücke", "GCN Ponte dei Funghi"}}},
    {39, {{"SNES Isla Chocolate", "SNES Choco Island 2", "SNES Île Choco 2", "SNES Schoko-Insel 2", "SNES Cioccoisola 2"}}},
    {23, {{"N64 Muelle Embrujado", "N64 Banshee Boardwalk", "N64 Ponton lugubre", "N64 Spukpfad", "N64 Pontile Stregato"}}},
    {40, {{"GBA Jardín Celeste", "GBA Sky Garden", "GBA Jardin volant", "GBA Wolkenpiste", "GBA Giardino Nuvola"}}},
    {1, {{"GCN Circuito Yoshi", "GCN Yoshi Circuit", "GCN Circuit Yoshi", "GCN Yoshis Piste", "GCN Circuito di Yoshi"}}},
    // batalla
    {42, {{"Ciudad Bloque", "Block Fort", "Bloc Fort", "Block-Fort", "Forte Blocco"}}},
    {43, {{"Plaza Tuberías", "Pipe Plaza", "Square Tuyau", "Röhren-Platz", "Tubitunnel"}}},
    {44, {{"Nintendo DS", "Nintendo DS", "Nintendo DS", "Nintendo DS", "Nintendo DS"}}},
    {45, {{"Casa Crepuscular", "Twilight House", "Maison de l'Aube", "Finstervilla", "Casa Crepuscolare"}}},
    {46, {{"Costa Palma", "Palm Shore", "Feuille de palmier", "Palmenblatt", "Riva delle Palme"}}},
    {47, {{"Dulce Tarta", "Tart Top", "Tarte sucrée", "Süße Torte", "Dolce Torta"}}},
};

static const char *circuito(uint32_t id, int l) {
    for (size_t i = 0; i < sizeof CIRCUITOS / sizeof *CIRCUITOS; i++)
        if (CIRCUITOS[i].id == id) return CIRCUITOS[i].n.t[l];
    return NULL;
}

static void puesto(char *s, size_t n, int p, int l) {
    if (l == EN) snprintf(s, n, "%d%s", p, p == 1 ? "st" : p == 2 ? "nd" : p == 3 ? "rd" : "th");
    else if (l == FR) snprintf(s, n, "%d%s", p, p == 1 ? "er" : "e");
    else if (l == DE) snprintf(s, n, "%d.", p);
    else if (l == IT) snprintf(s, n, "%d\xC2\xBA", p);
    else snprintf(s, n, "%d.\xC2\xBA", p);
}

// ---- qué está pasando en el juego -----------------------------------------------------------------------------------
static uint32_t lee32(uint32_t a) { uint32_t v; memcpy(&v, g_main + (a & 0x3FFFFC), 4); return v; }
static int en_ram(uint32_t p) { return p >= 0x02000000u && p < 0x023FF000u && !(p & 3); }

static void junta(char *d, size_t n, const char *s) {  // añade s a d, con un punto en medio si ya había algo
    size_t k = strlen(d);
    snprintf(d + k, n - k, "%s%s", k ? " \xC2\xB7 " : "", s);
}

static void estado(char *arriba, size_t na, char *abajo, size_t nb) {
    int l = idioma();
    arriba[0] = abajo[0] = 0;
    char cod[16] = "";
    int en_sala = 0, e = online_estado(cod, &en_sala), red = wifi_en_que();
    int sala = e == 7 || e == 8;  // la tuya (7) o la de otro (8); menu.cpp, ONL_EN_SALA y ONL_UNIDO
    const char *multi = red == 2 || sala ? ONLINE.t[l] : red == 1 ? LOCAL.t[l] : NULL;
    uint32_t c = lee32(0x021759C0), k = lee32(0x0217561C);
    int yo = -1, pos = 0, vuelta = 0, acabado = 0;
    uint32_t id = 0, modo = 9, cc = 0, n = 0;
    if (en_ram(c) && en_ram(k)) {
        const uint8_t *q = g_main + (c & 0x3FFFFF);
        memcpy(&id, q, 4); memcpy(&modo, q + 8, 4); memcpy(&cc, q + 0x10, 4);
        n = q[0x3D0];
        for (int i = 0; i < 8 && yo < 0; i++) {
            const uint8_t *e = g_main + ((k + (uint32_t)i * 0x8C) & 0x3FFFFF);
            uint32_t f;
            uint16_t p;
            memcpy(&f, e + 0x44, 4);
            memcpy(&p, e + 0x50, 2);
            if (!(f & 1) || (p & 15) < 1 || (p & 15) > 8) continue;
            uint32_t est;
            memcpy(&est, e + 0x14, 4);
            yo = i; pos = p & 15; vuelta = (int8_t)e[0x4E]; acabado = est != 0;
        }
    }
    if (yo < 0 || modo > 7 || id > 51 || !n || n > 8) {  // fuera de carrera
        snprintf(arriba, na, "%s", multi ? multi : MENU.t[l]);
        if (e == 7) snprintf(abajo, nb, "%s (%d)", SALA.t[l], en_sala + 1);
        else if (sala) snprintf(abajo, nb, "%s", SALA.t[l]);
        else if (multi) snprintf(abajo, nb, "%s", MENU.t[l]);
        return;
    }
    if (modo > 4) modo = 3;  // 5: la carrera de la Conexión Wi-Fi
    char ccs[48] = "";
    if (modo == 0 || modo == 3) {
        if (cc == 3) snprintf(ccs, sizeof ccs, " \xC2\xB7 150cc %s", ESPEJO.t[l]);
        else if (cc < 3) snprintf(ccs, sizeof ccs, " \xC2\xB7 %dcc", cc == 0 ? 50 : cc == 1 ? 100 : 150);
    }
    snprintf(arriba, na, "%s%s%s%s", multi ? multi : "", multi ? " \xC2\xB7 " : "", MODOS[modo].t[l], ccs);
    const char *nom = circuito(id, l);
    if (ver_circuito && nom) junta(abajo, nb, nom);
    if (modo == 0 || modo == 1 || modo == 3) {
        int total = id == 9 ? 5 : 3;  // Parque Bebé
        char s[40], ps[16];
        if (ver_vuelta) {
            if (acabado || vuelta > total) snprintf(s, sizeof s, "%s", META.t[l]);
            else snprintf(s, sizeof s, "%s %d/%d", VUELTA.t[l], vuelta < 1 ? 1 : vuelta, total);
            junta(abajo, nb, s);
        }
        if (ver_posicion && n > 1) {
            puesto(ps, sizeof ps, pos, l);
            snprintf(s, sizeof s, "%s/%u", ps, n);
            junta(abajo, nb, s);
        }
    }
}

// ---- la conexión con Discord ----------------------------------------------------------------------------------------
static int primera(void) {  // pruebas: otra numeración, para no hablar con el Discord de verdad
    const char *v = getenv("MKDS_DISCORD_PRIMERA");
    return v ? atoi(v) : 0;
}
#ifdef _WIN32
static HANDLE tubo = INVALID_HANDLE_VALUE;
static int conectado(void) { return tubo != INVALID_HANDLE_VALUE; }
static void cierra(void) { if (conectado()) CloseHandle(tubo); tubo = INVALID_HANDLE_VALUE; }
static int abre(void) {
    for (int i = primera(); i < primera() + 10; i++) {
        char r[64];
        snprintf(r, sizeof r, "\\\\.\\pipe\\discord-ipc-%d", i);
        tubo = CreateFileA(r, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (conectado()) return 1;
    }
    return 0;
}
static int escribe(const void *p, uint32_t n) {
    DWORD e;
    return WriteFile(tubo, p, n, &e, NULL) && e == n;
}
static int hay(void) {  // bytes por leer, -1 si se ha cortado
    DWORD n = 0;
    return PeekNamedPipe(tubo, NULL, 0, NULL, &n, NULL) ? (int)n : -1;
}
static int lee(void *p, uint32_t n) {
    DWORD e;
    for (uint32_t k = 0; k < n; k += e)
        if (!ReadFile(tubo, (char *)p + k, n - k, &e, NULL) || !e) return 0;
    return 1;
}
#else
#include <sys/ioctl.h>
static int tubo = -1;
static int conectado(void) { return tubo >= 0; }
static void cierra(void) { if (conectado()) close(tubo); tubo = -1; }
static int abre(void) {
    const char *base[] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), getenv("TEMP"), "/tmp"};
    static const char *sub[] = {"", "app/com.discordapp.Discord/", "snap.discord/", ".flatpak/com.discordapp.Discord/xdg-run/"};
    for (int b = 0; b < 5; b++)
        for (int s = 0; base[b] && s < 4; s++)
            for (int i = primera(); i < primera() + 10; i++) {
                struct sockaddr_un a;
                memset(&a, 0, sizeof a);
                a.sun_family = AF_UNIX;
                int n = snprintf(a.sun_path, sizeof a.sun_path, "%s/%sdiscord-ipc-%d", base[b], sub[s], i);
                if (n < 0 || n >= (int)sizeof a.sun_path) continue;
                tubo = socket(AF_UNIX, SOCK_STREAM, 0);
                if (tubo < 0) return 0;
                if (!connect(tubo, (struct sockaddr *)&a, sizeof a)) {
#ifdef SO_NOSIGPIPE
                    int si = 1;
                    setsockopt(tubo, SOL_SOCKET, SO_NOSIGPIPE, &si, sizeof si);
#endif
                    return 1;
                }
                cierra();
            }
    return 0;
}
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
static int escribe(const void *p, uint32_t n) { return send(tubo, p, n, MSG_NOSIGNAL) == (ssize_t)n; }
static int hay(void) {
    int n = 0;
    char b;
    if (recv(tubo, &b, 1, MSG_PEEK | MSG_DONTWAIT) == 0) return -1;
    return ioctl(tubo, FIONREAD, &n) ? -1 : n;
}
static int lee(void *p, uint32_t n) {
    for (uint32_t k = 0; k < n;) {
        ssize_t e = recv(tubo, (char *)p + k, n - k, 0);
        if (e <= 0) return 0;
        k += (uint32_t)e;
    }
    return 1;
}
#endif

static int traza;
static int manda(uint32_t op, const char *json) {
    static uint8_t b[4096];
    uint32_t n = (uint32_t)strlen(json);
    if (n + 8 > sizeof b) return 0;
    memcpy(b, &op, 4); memcpy(b + 4, &n, 4); memcpy(b + 8, json, n);
    if (traza) fprintf(stderr, "[discord] -> %u %s\n", op, json);
    return escribe(b, n + 8);
}
// lee lo que haya mandado Discord; 0 si se ha cortado
static int atiende(void) {
    for (int n; (n = hay()) != 0;) {
        if (n < 0) return 0;
        uint32_t cab[2];
        static char t[8192];
        if (n < 8) return 1;
        if (!lee(cab, 8) || cab[1] >= sizeof t || !lee(t, cab[1])) return 0;
        t[cab[1]] = 0;
        if (traza) fprintf(stderr, "[discord] <- %u %.300s\n", cab[0], t);
        if (cab[0] == 2) return 0;              // adiós (por ejemplo, Application ID malo)
        if (cab[0] == 3 && !manda(4, t)) return 0;
    }
    return 1;
}

static void escapa(char *d, size_t n, const char *s) {  // a cadena de JSON
    size_t k = 0;
    for (; *s && k + 3 < n; s++) {
        if (*s == '"' || *s == '\\') d[k++] = '\\';
        if ((unsigned char)*s >= 32) d[k++] = *s;
    }
    d[k] = 0;
}

static int pid(void) {
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

static DWORD WINAPI hilo(LPVOID x) {
    (void)x;
    const char *app = getenv("MKDS_DISCORD_APP");  // pruebas
    if (!app || !*app) app = APLICACION;
    traza = getenv("MKDS_DISCORD_LOG") != NULL;
    long long inicio = (long long)time(NULL);
    char antes[600] = "", arriba[128], abajo[200];
    unsigned num = 0;
    int espera = 0, desde = 5, saludado = 0;
    for (;; Sleep(1000)) {
        if (!encendido) {
            if (conectado()) {  // quitado en el menú: se borra la actividad
                char j[200];
                snprintf(j, sizeof j, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d},\"nonce\":\"%u\"}", pid(), ++num);
                manda(1, j);
                Sleep(200);
                cierra();
            }
            espera = 0;
            continue;
        }
        if (!conectado()) {
            if (espera-- > 0) continue;
            espera = 15;
            if (!abre()) continue;
            char j[128];
            snprintf(j, sizeof j, "{\"v\":1,\"client_id\":\"%s\"}", app);
            if (!manda(0, j)) { cierra(); continue; }
            antes[0] = 0;
            desde = 5;
            saludado = 0;
            continue;
        }
        if (!atiende()) { cierra(); continue; }
        if (!saludado) { saludado = 1; continue; }  // un segundo para que Discord conteste al saludo
        desde++;
        estado(arriba, sizeof arriba, abajo, sizeof abajo);
        char ahora[600];
        snprintf(ahora, sizeof ahora, "%s|%s", arriba, abajo);
        if (!strcmp(ahora, antes) || desde < 5) continue;
        char e1[260], e2[420], st[460] = "", j[1600];
        escapa(e1, sizeof e1, arriba);
        escapa(e2, sizeof e2, abajo);
        if (e2[0]) snprintf(st, sizeof st, "\"state\":\"%s\",", e2);
        snprintf(j, sizeof j,
                 "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{\"details\":\"%s\",%s"
                 "\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"logo_512\",\"large_text\":\"DS Kart Recompiled\"},"
                 "\"instance\":false}},\"nonce\":\"%u\"}",
                 pid(), e1, st, inicio, ++num);
        if (!manda(1, j)) { cierra(); continue; }
        snprintf(antes, sizeof antes, "%s", ahora);
        desde = 0;
    }
    return 0;
}

static int opcion(const char *clave) {  // sin ponerla en ajustes.ini, sí
    const char *v = getenv(clave);
    return !v || !*v || atoi(v);
}

void discord_inicia(void) {
    if (encendido < 0) encendido = opcion("MKDS_DISCORD");
    discord_ver(opcion("MKDS_DISCORD_CIRCUITO"), opcion("MKDS_DISCORD_VUELTA"), opcion("MKDS_DISCORD_POSICION"));
    const char *app = getenv("MKDS_DISCORD_APP");
    if (!APLICACION[0] && !(app && *app)) return;
    if (getenv("MKDS_OCULTO") && !getenv("MKDS_DISCORD_LOG")) return;  // las pruebas ocultas no salen en Discord
    CreateThread(NULL, 0, hilo, NULL, 0, NULL);
}
