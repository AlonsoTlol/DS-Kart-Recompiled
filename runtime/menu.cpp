// Menú de ajustes dentro del juego, con Dear ImGui (runtime/imgui, licencia MIT). Ver menu.h.
//
// Pestañas: Gráficos (pantalla completa, imágenes por segundo, resolución del 3D, mostrar FPS), Mandos (los
// conectados y qué hace cada botón), Audio (volumen, silencio, micrófono) y Juego (nombre, idioma, IP del amigo
// para el multijugador). Lo que se puede cambiar en marcha se aplica al momento; la resolución y el idioma, al
// volver a abrir el juego. Todo se guarda en ajustes.ini al cerrar el menú.
// Los textos están en los 5 idiomas del launcher (LAUNCHER_IDIOMA en ajustes.ini).
// Se compila sin la biblioteca de C++ (sin excepciones, RTTI ni estáticos con candado): basta el enlazador de C.
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl2.h"
#include "menu.h"

extern "C" {
void gx_pon_fps(int fps);                       // gpu3d.c: imágenes por segundo (intermedias)
void sonido_pon_volumen(int volumen, int silencio);  // sonido.c
void mic_pon(int activo, int ganancia);          // microfono.c
void nombre_cambia(void);                        // main_mkds.c: vuelve a leer MKDS_NOMBRE
}

// ---- cola de lo que llega del hilo de la ventana ------------------------------------------------------
#ifdef _WIN32
static CRITICAL_SECTION cerrojo;
static void cierra_c(void) { EnterCriticalSection(&cerrojo); }
static void abre_c(void) { LeaveCriticalSection(&cerrojo); }
static void cerrojo_inicia(void) { InitializeCriticalSection(&cerrojo); }
#else
static pthread_mutex_t cerrojo = PTHREAD_MUTEX_INITIALIZER;
static void cierra_c(void) { pthread_mutex_lock(&cerrojo); }
static void abre_c(void) { pthread_mutex_unlock(&cerrojo); }
static void cerrojo_inicia(void) {}
#endif

enum { EV_TECLA, EV_CARACTER, EV_RATON, EV_BOTON, EV_RUEDA };
struct Evento { int tipo, a, b; float x, y; };
static Evento cola[512];
static int cola_n;
static volatile int abierto, listo;
static volatile int fps_juego, fps_3d, fps_pantalla;

static void mete(Evento e) {
    if (!listo || !abierto) return;
    cierra_c();
    if (cola_n < 512) cola[cola_n++] = e;
    abre_c();
}

extern "C" int menu_abierto(void) { return listo && abierto; }
extern "C" void menu_tecla(int t, int p) { Evento e = {EV_TECLA, t, p, 0, 0}; mete(e); }
extern "C" void menu_caracter(unsigned c) { Evento e = {EV_CARACTER, (int)c, 0, 0, 0}; mete(e); }
extern "C" void menu_raton(float x, float y) { Evento e = {EV_RATON, 0, 0, x, y}; mete(e); }
extern "C" void menu_boton(int b, int p) { Evento e = {EV_BOTON, b, p, 0, 0}; mete(e); }
extern "C" void menu_rueda(float dy) { Evento e = {EV_RUEDA, 0, 0, 0, dy}; mete(e); }
extern "C" void menu_pon_fps(int j, int t, int p) { fps_juego = j; fps_3d = t; fps_pantalla = p; }

// el mando: los botones de la DS que cambian se convierten en teclas de navegación del menú
extern "C" void menu_mando(unsigned short in) {
    static unsigned short antes = 0x3FF;
    static const struct { int bit, tecla; } m[] = {
        {6, MT_PAD_ARRIBA}, {7, MT_PAD_ABAJO}, {5, MT_PAD_IZQ}, {4, MT_PAD_DER},
        {0, MT_PAD_ACEPTA}, {1, MT_PAD_ATRAS}, {9, MT_PAD_L}, {8, MT_PAD_R},
    };
    for (unsigned k = 0; k < sizeof m / sizeof *m; k++) {
        int ahora = !(in & (1u << m[k].bit)), era = !(antes & (1u << m[k].bit));
        if (ahora != era) menu_tecla(m[k].tecla, ahora);
    }
    antes = in;
}

// ---- ajustes -----------------------------------------------------------------------------------------
static struct {
    int fps;          // 60, 120, 180, 240
    int escala;       // 1-4 (al reiniciar)
    int mostrar_fps;
    int volumen;      // 0-100
    int silencio;
    int micro;
    int ganancia;     // 1-16
    char nombre[32];
    int idioma;       // 1 inglés, 2 francés, 3 alemán, 4 italiano, 5 español (al reiniciar)
    char ip[64];      // la del amigo (Radmin), para el multijugador
} aj;
static char idioma_menu[4] = "es";
static int pestaña_pedida = -1, pestaña_actual;
static int cambiado;

static int entero(const char *v, int def) { const char *e = getenv(v); return e && *e ? atoi(e) : def; }
static void texto(const char *v, char *dst, size_t n, const char *def) {
    const char *e = getenv(v);
    snprintf(dst, n, "%s", e ? e : def);
}

static void pon_env(const char *clave, const char *valor) {
    char var[64];
    snprintf(var, sizeof var, "MKDS_%s", clave);
#ifdef _WIN32
    _putenv_s(var, valor);
#else
    setenv(var, valor, 1);
#endif
}

static void lee_ajustes(void) {
    aj.fps = entero("MKDS_FPS", 60);
    aj.escala = entero("MKDS_ESCALA", 2);
    aj.mostrar_fps = entero("MKDS_MOSTRAR_FPS", 0);
    aj.volumen = entero("MKDS_VOLUMEN", 100);
    aj.silencio = entero("MKDS_SILENCIO", 0);
    aj.micro = entero("MKDS_MICRO", 1);
    aj.ganancia = entero("MKDS_MICRO_GANANCIA", 8);
    texto("MKDS_NOMBRE", aj.nombre, sizeof aj.nombre, "");
    aj.idioma = entero("MKDS_IDIOMA", 5);
    texto("MKDS_LOCAL_IP", aj.ip, sizeof aj.ip, "");
    texto("MKDS_LAUNCHER_IDIOMA", idioma_menu, sizeof idioma_menu, "es");
}

// ajustes.ini: se cambian las claves del menú y se deja todo lo demás (lo del launcher) como estaba
static void guarda_ajustes(void) {
    char v[16][80];
    const char *claves[] = {"FPS", "ESCALA", "MOSTRAR_FPS", "VOLUMEN", "SILENCIO", "MICRO", "MICRO_GANANCIA",
                            "NOMBRE", "IDIOMA", "LOCAL_IP", "PANTALLA_COMPLETA"};
    int n = (int)(sizeof claves / sizeof *claves);
    snprintf(v[0], 80, "%d", aj.fps); snprintf(v[1], 80, "%d", aj.escala); snprintf(v[2], 80, "%d", aj.mostrar_fps);
    snprintf(v[3], 80, "%d", aj.volumen); snprintf(v[4], 80, "%d", aj.silencio); snprintf(v[5], 80, "%d", aj.micro);
    snprintf(v[6], 80, "%d", aj.ganancia); snprintf(v[7], 80, "%s", aj.nombre); snprintf(v[8], 80, "%d", aj.idioma);
    snprintf(v[9], 80, "%s", aj.ip); snprintf(v[10], 80, "%d", ventana_pantalla_completa());
    static char lineas[128][256];
    int nl = 0, hecho[16] = {0};
    FILE *f = fopen("ajustes.ini", "r");
    if (f) {
        while (nl < 128 && fgets(lineas[nl], 256, f)) {
            lineas[nl][strcspn(lineas[nl], "\r\n")] = 0;
            for (int k = 0; k < n; k++) {
                size_t l = strlen(claves[k]);
                if (!strncmp(lineas[nl], claves[k], l) && lineas[nl][l] == '=') {
                    snprintf(lineas[nl], 256, "%s=%s", claves[k], v[k]);
                    hecho[k] = 1;
                }
            }
            nl++;
        }
        fclose(f);
    }
    for (int k = 0; k < n && nl < 128; k++)
        if (!hecho[k]) snprintf(lineas[nl++], 256, "%s=%s", claves[k], v[k]);
    f = fopen("ajustes.ini", "w");
    if (!f) return;
    for (int i = 0; i < nl; i++) fprintf(f, "%s\n", lineas[i]);
    fclose(f);
    for (int k = 0; k < n; k++) pon_env(claves[k], v[k]);  // y lo que lea el juego a partir de ahora
}

static void aplica(void) {
    gx_pon_fps(aj.fps);
    sonido_pon_volumen(aj.volumen, aj.silencio);
    mic_pon(aj.micro, aj.ganancia);
    pon_env("NOMBRE", aj.nombre);
    nombre_cambia();
    pon_env("LOCAL_IP", aj.ip);
    cambiado = 1;
}

extern "C" void menu_alterna(void) {
    if (!listo) return;
    abierto = !abierto;
    if (!abierto && cambiado) { guarda_ajustes(); cambiado = 0; }
}

// ---- textos en los 5 idiomas -----------------------------------------------------------------------------
struct Texto { const char *es, *en, *fr, *de, *it; };
static const Texto textos[] = {
    {"Ajustes", "Settings", "Paramètres", "Einstellungen", "Impostazioni"},
    {"Gráficos", "Graphics", "Graphismes", "Grafik", "Grafica"},
    {"Mandos", "Controllers", "Manettes", "Controller", "Controller"},
    {"Audio", "Audio", "Audio", "Audio", "Audio"},
    {"Juego", "Game", "Jeu", "Spiel", "Gioco"},
    {"Pantalla completa", "Fullscreen", "Plein écran", "Vollbild", "Schermo intero"},
    {"Las dos pantallas lado a lado. También con F11.", "Both screens side by side. Also with F11.",
     "Les deux écrans côte à côte. Aussi avec F11.", "Beide Bildschirme nebeneinander. Auch mit F11.",
     "I due schermi affiancati. Anche con F11."},
    {"Imágenes por segundo", "Frames per second", "Images par seconde", "Bilder pro Sekunde", "Fotogrammi al secondo"},
    {"El juego va a 60; por encima se pintan imágenes intermedias del 3D. Si el PC no llega, pinta las que puede.",
     "The game runs at 60; above that, in-between 3D frames are drawn. If the PC can't keep up, it draws what it can.",
     "Le jeu tourne à 60 ; au-delà, des images 3D intermédiaires sont dessinées. Si le PC ne suit pas, il en dessine moins.",
     "Das Spiel läuft mit 60; darüber werden 3D-Zwischenbilder gezeichnet. Schafft es der PC nicht, zeichnet er weniger.",
     "Il gioco va a 60; sopra si disegnano fotogrammi 3D intermedi. Se il PC non ce la fa, ne disegna meno."},
    {"Resolución del 3D", "3D resolution", "Résolution 3D", "3D-Auflösung", "Risoluzione 3D"},
    {"Original", "Original", "Originale", "Original", "Originale"},
    {"Se aplica al volver a abrir el juego.", "Applied the next time you open the game.",
     "Appliqué à la prochaine ouverture du jeu.", "Wird beim nächsten Spielstart übernommen.",
     "Si applica alla prossima apertura del gioco."},
    {"Mostrar FPS", "Show FPS", "Afficher les FPS", "FPS anzeigen", "Mostra FPS"},
    {"En una esquina, también en pantalla completa.", "In a corner, also in fullscreen.",
     "Dans un coin, aussi en plein écran.", "In einer Ecke, auch im Vollbild.", "In un angolo, anche a schermo intero."},
    {"Conectados", "Connected", "Connectées", "Verbunden", "Collegati"},
    {"Ningún mando conectado. Puedes jugar con el teclado.", "No controller connected. You can play with the keyboard.",
     "Aucune manette connectée. Tu peux jouer au clavier.", "Kein Controller verbunden. Du kannst mit der Tastatur spielen.",
     "Nessun controller collegato. Puoi giocare con la tastiera."},
    {"Botón de la DS", "DS button", "Bouton DS", "DS-Taste", "Tasto DS"},
    {"Mando", "Controller", "Manette", "Controller", "Controller"},
    {"Teclado", "Keyboard", "Clavier", "Tastatur", "Tastiera"},
    {"Cruceta", "D-pad", "Croix", "Steuerkreuz", "Croce"},
    {"cruceta o stick izquierdo", "D-pad or left stick", "croix ou stick gauche", "Steuerkreuz oder linker Stick",
     "croce o levetta sinistra"},
    {"flechas", "arrows", "flèches", "Pfeiltasten", "frecce"},
    {"Retroceso", "Backspace", "Retour arrière", "Rücktaste", "Backspace"},
    {"Lápiz", "Stylus", "Stylet", "Stift", "Pennino"},
    {"ratón en la pantalla de abajo", "mouse on the bottom screen", "souris sur l'écran du bas",
     "Maus auf dem unteren Bildschirm", "mouse sullo schermo inferiore"},
    {"Volumen", "Volume", "Volume", "Lautstärke", "Volume"},
    {"Silenciar", "Mute", "Couper le son", "Stumm", "Muto"},
    {"Micrófono (soplar en la Batalla de globos)", "Microphone (blow in Balloon Battle)",
     "Micro (souffler dans la Bataille de ballons)", "Mikrofon (pusten im Ballonkampf)",
     "Microfono (soffiare nella Battaglia palloncini)"},
    {"Sensibilidad", "Sensitivity", "Sensibilité", "Empfindlichkeit", "Sensibilità"},
    {"Si no se infla al soplar, súbela; si se infla solo, bájala.", "If it doesn't inflate when you blow, raise it; if it inflates by itself, lower it.",
     "S'il ne gonfle pas quand tu souffles, augmente-la ; s'il gonfle tout seul, baisse-la.",
     "Bläst er sich beim Pusten nicht auf, erhöhen; bläst er sich von selbst auf, senken.",
     "Se non si gonfia quando soffi, alzala; se si gonfia da solo, abbassala."},
    {"Tu nombre", "Your name", "Ton nom", "Dein Name", "Il tuo nome"},
    {"El que ven los demás en multijugador (10 letras como mucho).", "What others see in multiplayer (10 letters max).",
     "Celui que voient les autres en multijoueur (10 lettres max).", "Den sehen die anderen im Mehrspieler (max. 10 Zeichen).",
     "Quello che vedono gli altri in multigiocatore (max 10 lettere)."},
    {"Idioma del juego", "Game language", "Langue du jeu", "Spielsprache", "Lingua del gioco"},
    {"Multijugador: IP de tu amigo", "Multiplayer: your friend's IP", "Multijoueur : IP de ton ami",
     "Mehrspieler: IP deines Freundes", "Multigiocatore: IP del tuo amico"},
    {"Solo si no os veis en la lista de grupos (por ejemplo, su IP de Radmin). Vacío = buscar solo.",
     "Only if you can't see each other's group (e.g. their Radmin IP). Empty = find automatically.",
     "Seulement si vous ne vous voyez pas (par ex. son IP Radmin). Vide = recherche automatique.",
     "Nur wenn ihr euch nicht seht (z. B. seine Radmin-IP). Leer = automatisch suchen.",
     "Solo se non vi vedete (es. il suo IP Radmin). Vuoto = ricerca automatica."},
    {"Volver al juego", "Back to the game", "Retour au jeu", "Zurück zum Spiel", "Torna al gioco"},
    {"Menú abierto: el juego no recibe los botones. F1 o L3 + R3 para volver.",
     "Menu open: the game doesn't get your buttons. F1 or L3 + R3 to go back.",
     "Menu ouvert : le jeu ne reçoit pas tes boutons. F1 ou L3 + R3 pour revenir.",
     "Menü offen: das Spiel bekommt keine Tasten. F1 oder L3 + R3 zum Zurückkehren.",
     "Menu aperto: il gioco non riceve i tasti. F1 o L3 + R3 per tornare."},
    {"L / R: cambiar de pestaña", "L / R: change tab", "L / R : changer d'onglet", "L / R: Tab wechseln", "L / R: cambia scheda"},
    {"Cruz", "Cross", "Croix", "Kreuz", "Croce"},
    {"Círculo", "Circle", "Rond", "Kreis", "Cerchio"},
    {"Triángulo", "Triangle", "Triangle", "Dreieck", "Triangolo"},
    {"Cuadrado", "Square", "Carré", "Quadrat", "Quadrato"},
};

static const char *T(const char *es) {
    int i = !strcmp(idioma_menu, "en") ? 1 : !strcmp(idioma_menu, "fr") ? 2 : !strcmp(idioma_menu, "de") ? 3
          : !strcmp(idioma_menu, "it") ? 4 : 0;
    if (!i) return es;
    for (unsigned k = 0; k < sizeof textos / sizeof *textos; k++)
        if (!strcmp(textos[k].es, es)) {
            const char *t[] = {textos[k].es, textos[k].en, textos[k].fr, textos[k].de, textos[k].it};
            return t[i];
        }
    return es;
}

// ---- ImGui -----------------------------------------------------------------------------------------
static double reloj_s(void) {
#ifdef _WIN32
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

// colores del launcher: azul noche, rojo y amarillo
static void estilo(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 7; s.TabRounding = 7; s.GrabRounding = 7; s.ChildRounding = 8;
    s.WindowPadding = ImVec2(22, 18); s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 9);
    s.WindowBorderSize = 1;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.063f, 0.075f, 0.165f, 0.97f);   // #10132A
    c[ImGuiCol_Border] = ImVec4(0.165f, 0.188f, 0.384f, 1);          // #2A3062
    c[ImGuiCol_FrameBg] = ImVec4(0.125f, 0.145f, 0.29f, 1);          // #20254A
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.19f, 0.38f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.2f, 0.22f, 0.44f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.094f, 0.11f, 0.227f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(1.0f, 0.30f, 0.24f, 0.85f);
    c[ImGuiCol_TabSelected] = ImVec4(1.0f, 0.30f, 0.24f, 1);         // #FF4D3D
    c[ImGuiCol_TabSelectedOverline] = ImVec4(1.0f, 0.79f, 0.25f, 1);
    c[ImGuiCol_Button] = ImVec4(0.125f, 0.145f, 0.29f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 0.30f, 0.24f, 0.8f);
    c[ImGuiCol_ButtonActive] = ImVec4(1.0f, 0.30f, 0.24f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.79f, 0.25f, 1);           // #FFC940
    c[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.79f, 0.25f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.86f, 0.45f, 1);
    c[ImGuiCol_Header] = ImVec4(0.125f, 0.145f, 0.29f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 0.30f, 0.24f, 0.6f);
    c[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.79f, 0.25f, 1);
    c[ImGuiCol_Text] = ImVec4(0.953f, 0.957f, 1, 1);                 // #F3F4FF
    c[ImGuiCol_TextDisabled] = ImVec4(0.604f, 0.639f, 0.78f, 1);     // #9AA3C7
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.125f, 0.145f, 0.29f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
}

// La letra: la del sistema (Segoe UI en Windows; DejaVu, Noto o Ubuntu en Linux), con tildes y ñ. Se carga
// grande y se escala según el tamaño de la ventana. Si no hay ninguna, la de ImGui (sin tildes).
static const float LETRA = 34;
static void carga_letra(void) {
    ImGuiIO &io = ImGui::GetIO();
    static const ImWchar latin[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0};  // Latin-1, rayas, puntos suspensivos
    const char *rutas[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };
    for (unsigned i = 0; i < sizeof rutas / sizeof *rutas; i++) {
        FILE *f = fopen(rutas[i], "rb");
        if (!f) continue;
        fclose(f);
        if (io.Fonts->AddFontFromFileTTF(rutas[i], LETRA, NULL, latin)) return;
    }
    ImFontConfig c;
    c.SizePixels = LETRA;
    io.Fonts->AddFontDefault(&c);
}

extern "C" int menu_inicia(void) {
    if (listo) return 1;
    cerrojo_inicia();  // antes de 'listo': hasta entonces la ventana no mete nada en la cola
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;  // nada de imgui.ini: los ajustes van en ajustes.ini
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    estilo();
    carga_letra();
    if (!ImGui_ImplOpenGL2_Init()) return 0;
    lee_ajustes();
    aplica();
    cambiado = 0;
    listo = 1;
    if (getenv("MKDS_MENU_ABIERTO")) {  // pruebas: arranca con el menú abierto (y en esa pestaña, 0-3)
        abierto = 1;
        pestaña_pedida = atoi(getenv("MKDS_MENU_ABIERTO")) % 4;
    }
    return 1;
}

static ImGuiKey clave_imgui(int t) {
    switch (t) {
    case MT_TAB: return ImGuiKey_Tab;
    case MT_IZQ: return ImGuiKey_LeftArrow;
    case MT_DER: return ImGuiKey_RightArrow;
    case MT_ARRIBA: return ImGuiKey_UpArrow;
    case MT_ABAJO: return ImGuiKey_DownArrow;
    case MT_INICIO: return ImGuiKey_Home;
    case MT_FIN: return ImGuiKey_End;
    case MT_BORRAR: return ImGuiKey_Backspace;
    case MT_SUPR: return ImGuiKey_Delete;
    case MT_ENTER: return ImGuiKey_Enter;
    case MT_ESC: return ImGuiKey_Escape;
    case MT_CTRL: return ImGuiMod_Ctrl;
    case MT_SHIFT: return ImGuiMod_Shift;
    case MT_PAD_ARRIBA: return ImGuiKey_GamepadDpadUp;
    case MT_PAD_ABAJO: return ImGuiKey_GamepadDpadDown;
    case MT_PAD_IZQ: return ImGuiKey_GamepadDpadLeft;
    case MT_PAD_DER: return ImGuiKey_GamepadDpadRight;
    case MT_PAD_ACEPTA: return ImGuiKey_GamepadFaceDown;
    case MT_PAD_ATRAS: return ImGuiKey_GamepadFaceRight;
    }
    return ImGuiKey_None;
}

// pasa a ImGui lo que ha llegado desde el hilo de la ventana
static void vacia_cola(void) {
    ImGuiIO &io = ImGui::GetIO();
    cierra_c();
    for (int i = 0; i < cola_n; i++) {
        Evento &e = cola[i];
        switch (e.tipo) {
        case EV_TECLA:
            if (e.a == MT_PAD_L || e.a == MT_PAD_R) {  // L / R: pestaña anterior / siguiente
                if (e.b) pestaña_pedida = (pestaña_actual + (e.a == MT_PAD_R ? 1 : 3)) % 4;
            } else if (e.a == MT_PAD_ATRAS && e.b) {
                abierto = 0;  // B en el mando: volver al juego (como en los menús de la DS)
                if (cambiado) { guarda_ajustes(); cambiado = 0; }
            } else {
                io.AddKeyEvent(clave_imgui(e.a), e.b != 0);
            }
            break;
        case EV_CARACTER: io.AddInputCharacter(e.a); break;
        case EV_RATON: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BOTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_RUEDA: io.AddMouseWheelEvent(0, e.y); break;
        }
    }
    cola_n = 0;
    abre_c();
}

static void ayuda(const char *t) {  // la nota gris de debajo de cada ajuste, un poco más pequeña
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::SetWindowFontScale(0.85f);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1);
    ImGui::PopStyleColor();
}

static void titulo(const char *t) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.79f, 0.25f, 1));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
}

static int pestaña(const char *nombre, int i) {
    ImGuiTabItemFlags f = pestaña_pedida == i ? ImGuiTabItemFlags_SetSelected : 0;
    if (!ImGui::BeginTabItem(nombre, NULL, f)) return 0;
    pestaña_actual = i;
    return 1;
}

static void pestaña_graficos(void) {
    int pc = ventana_pantalla_completa();
    bool b = pc != 0;
    if (ImGui::Checkbox(T("Pantalla completa"), &b)) ventana_pide_pantalla_completa(b);
    ayuda(T("Las dos pantallas lado a lado. También con F11."));
    titulo(T("Imágenes por segundo"));
    static const int fpss[] = {60, 120, 180, 240};
    for (int i = 0; i < 4; i++) {
        char t[16];
        snprintf(t, sizeof t, "%d##fps", fpss[i]);
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(t, aj.fps == fpss[i])) { aj.fps = fpss[i]; aplica(); }
    }
    ayuda(T("El juego va a 60; por encima se pintan imágenes intermedias del 3D. Si el PC no llega, pinta las que puede."));
    titulo(T("Resolución del 3D"));
    for (int i = 1; i <= 4; i++) {
        char t[32];
        if (i == 1) snprintf(t, sizeof t, "%s##esc", T("Original"));
        else snprintf(t, sizeof t, "%d×##esc%d", i, i);
        if (i > 1) ImGui::SameLine();
        if (ImGui::RadioButton(t, aj.escala == i)) { aj.escala = i; cambiado = 1; }
    }
    ayuda(T("Se aplica al volver a abrir el juego."));
    ImGui::Spacing();
    bool m = aj.mostrar_fps != 0;
    if (ImGui::Checkbox(T("Mostrar FPS"), &m)) { aj.mostrar_fps = m; cambiado = 1; }
    ayuda(T("En una esquina, también en pantalla completa."));
}

static void pestaña_mandos(void) {
    titulo(T("Conectados"));
    char nombres[8][64];
    int n = ventana_mandos(nombres, 8);
    if (!n) ayuda(T("Ningún mando conectado. Puedes jugar con el teclado."));
    for (int i = 0; i < n; i++) ImGui::BulletText("%s", nombres[i]);
    ImGui::Spacing();
    if (ImGui::BeginTable("botones", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Botón de la DS"));
        ImGui::TableSetupColumn(T("Mando"));
        ImGui::TableSetupColumn(T("Teclado"));
        ImGui::TableHeadersRow();
        char a[64], b[64], x[64], y[64];  // Xbox / PlayStation (por posición: el de arriba es la X de la DS)
        snprintf(a, sizeof a, "A / %s", T("Cruz"));
        snprintf(b, sizeof b, "B / %s", T("Círculo"));
        snprintf(x, sizeof x, "Y / %s", T("Triángulo"));
        snprintf(y, sizeof y, "X / %s", T("Cuadrado"));
        const char *filas[][3] = {
            {"A", a, "X"}, {"B", b, "Z"}, {"X", x, "S"}, {"Y", y, "A"},
            {"L", "LB · LT / L1 · L2", "Q"}, {"R", "RB · RT / R1 · R2", "W"},
            {"Start", "Start / Options", "Enter"}, {"Select", "Back / Create", T("Retroceso")},
            {T("Cruceta"), T("cruceta o stick izquierdo"), T("flechas")},
            {T("Lápiz"), "—", T("ratón en la pantalla de abajo")},
        };
        for (unsigned i = 0; i < sizeof filas / sizeof *filas; i++) {
            ImGui::TableNextRow();
            for (int k = 0; k < 3; k++) { ImGui::TableSetColumnIndex(k); ImGui::TextWrapped("%s", filas[i][k]); }
        }
        ImGui::EndTable();
    }
}

static void pestaña_audio(void) {
    titulo(T("Volumen"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##vol", &aj.volumen, 0, 100, "%d %%")) aplica();
    bool s = aj.silencio != 0;
    if (ImGui::Checkbox(T("Silenciar"), &s)) { aj.silencio = s; aplica(); }
    ImGui::Spacing();
    titulo(T("Micrófono (soplar en la Batalla de globos)"));
    bool m = aj.micro != 0;
    if (ImGui::Checkbox("##micro", &m)) { aj.micro = m; aplica(); }
    ImGui::SameLine();
    ImGui::TextUnformatted(T("Sensibilidad"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!aj.micro);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##gan", &aj.ganancia, 1, 16, "x%d")) aplica();
    ImGui::EndDisabled();
    ayuda(T("Si no se infla al soplar, súbela; si se infla solo, bájala."));
}

static void pestaña_juego(void) {
    titulo(T("Tu nombre"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##nombre", aj.nombre, 11)) aplica();
    ayuda(T("El que ven los demás en multijugador (10 letras como mucho)."));
    titulo(T("Idioma del juego"));
    static const char *idiomas[] = {"English", "Français", "Deutsch", "Italiano", "Español"};
    int i = aj.idioma - 1;
    if (i < 0 || i > 4) i = 4;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##idioma", &i, idiomas, 5)) { aj.idioma = i + 1; cambiado = 1; }
    ayuda(T("Se aplica al volver a abrir el juego."));
    titulo(T("Multijugador: IP de tu amigo"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##ip", aj.ip, sizeof aj.ip, ImGuiInputTextFlags_CharsNoBlank)) aplica();
    ayuda(T("Solo si no os veis en la lista de grupos (por ejemplo, su IP de Radmin). Vacío = buscar solo."));
}

static void contador_fps(float W, float s) {
    ImGui::SetNextWindowPos(ImVec2(W - 12 * s, 12 * s), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * s, 5 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * s);
    ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(0.75f);
    ImGui::Text("%d FPS", fps_pantalla);  // lo que se ve por segundo (con las imágenes intermedias)
    ImGui::End();
    ImGui::PopStyleVar(2);
}

extern "C" void menu_pinta(int ancho, int alto, int ancho_px, int alto_px) {
    if (!listo || ancho <= 0 || alto <= 0) return;
    if (!abierto && !aj.mostrar_fps) return;
    ImGuiIO &io = ImGui::GetIO();
    static double antes;
    double ahora = reloj_s();
    io.DeltaTime = antes > 0 && ahora > antes ? (float)(ahora - antes) : 1.0f / 60;
    antes = ahora;
    io.DisplaySize = ImVec2((float)ancho, (float)alto);
    io.DisplayFramebufferScale = ImVec2((float)ancho_px / ancho, (float)alto_px / alto);
    float s = alto / 768.0f;  // todo crece con la ventana (768 = la ventana normal)
    if (s < 0.75f) s = 0.75f;
    io.FontGlobalScale = 21 * s / LETRA;
    vacia_cola();
    ImGui_ImplOpenGL2_NewFrame();
    ImGui::NewFrame();
    if (aj.mostrar_fps) contador_fps((float)ancho, s);
    if (abierto) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(5, 6, 15, 165));
        float w = 640 * s, h = 560 * s;
        if (w > ancho - 20) w = (float)ancho - 20;
        if (h > alto - 20) h = (float)alto - 20;
        ImGui::SetNextWindowPos(ImVec2(ancho * 0.5f, alto * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetWindowFontScale(1.25f);
        ImGui::TextUnformatted("DS KART");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.24f, 1));
        ImGui::TextUnformatted("RECOMPILED");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("·  %s", T("Ajustes"));
        ImGui::SetWindowFontScale(1);
        ImGui::Spacing();
        float pie = ImGui::GetFrameHeightWithSpacing() * 2.2f;
        if (ImGui::BeginTabBar("pestañas")) {
            if (pestaña(T("Gráficos"), 0)) { ImGui::BeginChild("g", ImVec2(0, -pie)); pestaña_graficos(); ImGui::EndChild(); ImGui::EndTabItem(); }
            if (pestaña(T("Mandos"), 1)) { ImGui::BeginChild("m", ImVec2(0, -pie)); pestaña_mandos(); ImGui::EndChild(); ImGui::EndTabItem(); }
            if (pestaña(T("Audio"), 2)) { ImGui::BeginChild("a", ImVec2(0, -pie)); pestaña_audio(); ImGui::EndChild(); ImGui::EndTabItem(); }
            if (pestaña(T("Juego"), 3)) { ImGui::BeginChild("j", ImVec2(0, -pie)); pestaña_juego(); ImGui::EndChild(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        pestaña_pedida = -1;
        ImGui::Separator();
        ayuda(T("Menú abierto: el juego no recibe los botones. F1 o L3 + R3 para volver."));
        ImGui::SetWindowFontScale(0.85f);
        ImGui::TextDisabled("%s", T("L / R: cambiar de pestaña"));
        ImGui::SetWindowFontScale(1);
        ImGui::SameLine(ImGui::GetWindowWidth() - 220 * s);
        if (ImGui::Button(T("Volver al juego"), ImVec2(200 * s, 0))) menu_alterna();
        ImGui::End();
    }
    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
}
