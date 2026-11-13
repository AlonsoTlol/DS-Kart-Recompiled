// Launcher con ventana para Linux y macOS (en Windows está Launcher.cs, con WPF). Hace lo mismo con menos: elegir la
// ROM, los ajustes, JUGAR (prepara el juego la primera vez con linux/preparar.sh) y actualizar.
// Va con SDL2, OpenGL y Dear ImGui (lo mismo que el juego), y lo compila linux/lanzador.sh la primera vez, porque en
// estos sistemas el juego ya se compila en el PC de cada uno. Se abre con linux/jugar.sh, que también lo vuelve a
// compilar después de una actualización (sale con SALIDA_ACTUALIZADO).
// Sin excepciones ni biblioteca de C++, como menu.cpp: se enlaza como C.
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "imgui.h"
#include "imgui_impl_opengl2.h"

enum { SALIDA_ACTUALIZADO = 42, SALIDA_SIN_VENTANA = 43 };

// launcher/portada.c: el vídeo de fondo (portada.mpg), como el portada.mp4 del launcher de Windows
extern "C" {
int portada_abre(const char *ruta, int *w, int *h);
const uint8_t *portada_avanza(double dt);
void portada_cierra(void);
}

// ---- ajustes.ini: se cambian las claves de aquí y se deja todo lo demás como estaba ----------------------------------
#define MAX_LINEAS 128
static char lineas[MAX_LINEAS][256];
static int nlineas;

static void lee_ajustes(void) {
    FILE *f = fopen("ajustes.ini", "r");
    if (!f) return;
    while (nlineas < MAX_LINEAS && fgets(lineas[nlineas], 256, f)) {
        lineas[nlineas][strcspn(lineas[nlineas], "\r\n")] = 0;
        nlineas++;
    }
    fclose(f);
}

static void guarda_ajustes(void) {
    FILE *f = fopen("ajustes.ini", "w");
    if (!f) return;
    for (int i = 0; i < nlineas; i++) fprintf(f, "%s\n", lineas[i]);
    fclose(f);
}

static const char *ajuste(const char *clave, const char *si_no) {
    size_t l = strlen(clave);
    for (int i = 0; i < nlineas; i++)
        if (!strncmp(lineas[i], clave, l) && lineas[i][l] == '=') return lineas[i] + l + 1;
    return si_no;
}

static void pon_ajuste(const char *clave, const char *valor) {
    size_t l = strlen(clave);
    for (int i = 0; i < nlineas; i++)
        if (!strncmp(lineas[i], clave, l) && lineas[i][l] == '=') {
            snprintf(lineas[i], 256, "%s=%s", clave, valor);
            guarda_ajustes();
            return;
        }
    if (nlineas < MAX_LINEAS) snprintf(lineas[nlineas++], 256, "%s=%s", clave, valor);
    guarda_ajustes();
}

static void pon_ajuste_n(const char *clave, int n) {
    char t[16];
    snprintf(t, sizeof t, "%d", n);
    pon_ajuste(clave, t);
}

// ---- textos ---------------------------------------------------------------------------------------------------------
static const char *const idiomas[] = {"es", "en", "fr", "de", "it"};
static const char *const nombres_idioma[] = {"Español", "English", "Français", "Deutsch", "Italiano"};
static const int idioma_juego[] = {5, 1, 2, 3, 4};  // como el firmware de la DS
static int idioma;

static const struct { const char *t[5]; } textos[] = {
    {{"JUGAR", "PLAY", "JOUER", "SPIELEN", "GIOCA"}},
    {{"Inicio", "Home", "Accueil", "Start", "Home"}},
    {{"Sala por internet, hasta 8 jugadores", "Internet room, up to 8 players", "Salon par Internet, jusqu'à 8 joueurs",
      "Internet-Raum, bis zu 8 Spieler", "Stanza via Internet, fino a 8 giocatori"}},
    {{"Crear sala", "Create room", "Créer un salon", "Raum erstellen", "Crea stanza"}},
    {{"Unirse", "Join", "Rejoindre", "Beitreten", "Entra"}},
    {{"Ajustes", "Settings", "Paramètres", "Einstellungen", "Impostazioni"}},
    {{"Tu ROM", "Your ROM", "Votre ROM", "Deine ROM", "La tua ROM"}},
    {{"Elegir ROM", "Choose ROM", "Choisir la ROM", "ROM wählen", "Scegli la ROM"}},
    {{"También puedes arrastrar el archivo .nds a esta ventana.", "You can also drag the .nds file onto this window.",
      "Vous pouvez aussi glisser le fichier .nds sur cette fenêtre.", "Du kannst die .nds-Datei auch auf dieses Fenster ziehen.",
      "Puoi anche trascinare il file .nds in questa finestra."}},
    {{"ROM lista", "ROM ready", "ROM prête", "ROM bereit", "ROM pronta"}},
    {{"Falta la ROM: elígela en Ajustes", "ROM missing: choose it in Settings", "ROM manquante : choisissez-la dans Paramètres",
      "ROM fehlt: wähle sie in den Einstellungen", "ROM mancante: sceglila in Impostazioni"}},
    {{"Tu nombre", "Your name", "Votre nom", "Dein Name", "Il tuo nome"}},
    {{"Pon tu nombre en Ajustes para poder jugar", "Enter your name in Settings to play", "Entrez votre nom dans Paramètres pour jouer",
      "Gib in den Einstellungen deinen Namen ein", "Inserisci il tuo nome in Impostazioni per giocare"}},
    {{"Idioma", "Language", "Langue", "Sprache", "Lingua"}},
    {{"Resolución del 3D", "3D resolution", "Résolution 3D", "3D-Auflösung", "Risoluzione 3D"}},
    {{"Imágenes por segundo", "Frames per second", "Images par seconde", "Bilder pro Sekunde", "Fotogrammi al secondo"}},
    {{"Pantalla completa", "Full screen", "Plein écran", "Vollbild", "Schermo intero"}},
    {{"3D con la tarjeta gráfica (experimental)", "3D on the graphics card (experimental)",
      "3D sur la carte graphique (expérimental)", "3D auf der Grafikkarte (experimentell)", "3D sulla scheda grafica (sperimentale)"}},
    {{"Micrófono (para soplar)", "Microphone (for blowing)", "Microphone (pour souffler)", "Mikrofon (zum Pusten)", "Microfono (per soffiare)"}},
    {{"Actualizar a la %s", "Update to %s", "Mettre à jour vers %s", "Auf %s aktualisieren", "Aggiorna a %s"}},
    {{"Hay una versión nueva: %s", "There is a new version: %s", "Nouvelle version : %s", "Neue Version: %s", "Nuova versione: %s"}},
    {{"Actualizando...", "Updating...", "Mise à jour...", "Wird aktualisiert...", "Aggiornamento..."}},
    {{"No se pudo actualizar", "Could not update", "Mise à jour impossible", "Aktualisierung fehlgeschlagen", "Aggiornamento non riuscito"}},
    {{"Preparando el juego (la primera vez tarda unos minutos)...", "Preparing the game (the first time takes a few minutes)...",
      "Préparation du jeu (quelques minutes la première fois)...", "Spiel wird vorbereitet (beim ersten Mal einige Minuten)...",
      "Preparazione del gioco (la prima volta richiede qualche minuto)..."}},
    {{"No se pudo preparar el juego:", "Could not prepare the game:", "Impossible de préparer le jeu :",
      "Spiel konnte nicht vorbereitet werden:", "Impossibile preparare il gioco:"}},
    {{"El juego se ha cerrado con un error. Mira fallos.txt.", "The game closed with an error. See fallos.txt.",
      "Le jeu s'est fermé avec une erreur. Voir fallos.txt.", "Das Spiel wurde mit einem Fehler beendet. Siehe fallos.txt.",
      "Il gioco si è chiuso con un errore. Vedi fallos.txt."}},
    {{"Ese archivo no es una ROM de DS.", "That file is not a DS ROM.", "Ce fichier n'est pas une ROM DS.",
      "Diese Datei ist keine DS-ROM.", "Questo file non è una ROM DS."}},
    {{"Esa ROM no es Mario Kart DS europeo (AMCP).", "That ROM is not European Mario Kart DS (AMCP).",
      "Cette ROM n'est pas Mario Kart DS européen (AMCP).", "Diese ROM ist nicht das europäische Mario Kart DS (AMCP).",
      "Questa ROM non è Mario Kart DS europeo (AMCP)."}},
    {{"Es la versión europea, pero no la misma revisión o está modificada.", "It is the European version, but a different revision or modified.",
      "C'est la version européenne, mais une autre révision ou modifiée.", "Das ist die europäische Version, aber eine andere Revision oder verändert.",
      "È la versione europea, ma un'altra revisione o modificata."}},
    {{"No se pudo copiar la ROM.", "Could not copy the ROM.", "Impossible de copier la ROM.", "ROM konnte nicht kopiert werden.",
      "Impossibile copiare la ROM."}},
    {{"Hecho por alontube junto con Claude. Proyecto de aficionado sin relación con Nintendo.",
      "Made by alontube together with Claude. Fan project not affiliated with Nintendo.",
      "Fait par alontube avec Claude. Projet de fan sans lien avec Nintendo.",
      "Von alontube zusammen mit Claude gemacht. Fanprojekt ohne Verbindung zu Nintendo.",
      "Fatto da alontube insieme a Claude. Progetto amatoriale non affiliato a Nintendo."}},
};

static const char *T(const char *es) {
    for (unsigned i = 0; i < sizeof textos / sizeof *textos; i++)
        if (!strcmp(textos[i].t[0], es)) return textos[i].t[idioma];
    return es;
}

// ---- la ROM ---------------------------------------------------------------------------------------------------------
static const uint32_t CRC_ARM9 = 0x7DF73400, CRC_ARM7 = 0x3CB95F4C;  // los mismos que Launcher.cs

static uint32_t crc32_de(const uint8_t *d, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static uint32_t l32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *comprueba_rom(const uint8_t *d, size_t n) {
    if (n < 0x200) return T("Ese archivo no es una ROM de DS.");
    if (memcmp(d + 12, "AMCP", 4)) return T("Esa ROM no es Mario Kart DS europeo (AMCP).");
    uint32_t a9o = l32(d + 0x20), a9t = l32(d + 0x2C), a7o = l32(d + 0x30), a7t = l32(d + 0x3C);
    if ((uint64_t)a9o + a9t > n || (uint64_t)a7o + a7t > n) return T("Ese archivo no es una ROM de DS.");
    if (crc32_de(d + a9o, a9t) != CRC_ARM9 || crc32_de(d + a7o, a7t) != CRC_ARM7)
        return T("Es la versión europea, pero no la misma revisión o está modificada.");
    return NULL;
}

static int hay_rom(void) {
    struct stat st;
    return !stat("rom/mkds.nds", &st) && st.st_size > 0x200;
}

static char aviso[300];  // el último error, para enseñarlo

// Copia la ROM a rom/mkds.nds si vale (la que hubiera se queda como .antes)
static void elige_rom(const char *ruta) {
    FILE *f = fopen(ruta, "rb");
    if (!f) { snprintf(aviso, sizeof aviso, "%s", T("Ese archivo no es una ROM de DS.")); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    size_t leido = d ? fread(d, 1, (size_t)n, f) : 0;
    fclose(f);
    const char *mal = d && leido == (size_t)n ? comprueba_rom(d, (size_t)n) : T("Ese archivo no es una ROM de DS.");
    if (mal) { snprintf(aviso, sizeof aviso, "%s", mal); free(d); return; }
    mkdir("rom", 0755);
    if (hay_rom()) rename("rom/mkds.nds", "rom/mkds.nds.antes");
    f = fopen("rom/mkds.nds", "wb");
    if (!f || fwrite(d, 1, (size_t)n, f) != (size_t)n) snprintf(aviso, sizeof aviso, "%s", T("No se pudo copiar la ROM."));
    else aviso[0] = 0;
    if (f) fclose(f);
    free(d);
}

// ---- órdenes en segundo plano (preparar, actualizar, elegir archivo): la ventana sigue viva --------------------------
typedef struct {
    char orden[1024];
    char ultima[256];     // la última línea que ha escrito
    char todo[4096];      // lo último escrito, para enseñar el error
    volatile int activo, fin, salida;
} Trabajo;

static int hilo_trabajo(void *arg) {
    Trabajo *t = (Trabajo *)arg;
    FILE *p = popen(t->orden, "r");
    if (!p) { t->salida = 1; t->fin = 1; return 0; }
    char l[256];
    size_t n = 0;
    while (fgets(l, sizeof l, p)) {
        l[strcspn(l, "\r\n")] = 0;
        if (l[0]) snprintf(t->ultima, sizeof t->ultima, "%s", l);
        size_t k = strlen(l);
        if (n + k + 2 > sizeof t->todo) { n = 0; }
        memcpy(t->todo + n, l, k);
        n += k;
        t->todo[n++] = '\n';
        t->todo[n] = 0;
    }
    t->salida = pclose(p);
    t->fin = 1;
    return 0;
}

static void lanza(Trabajo *t, const char *orden) {
    memset(t, 0, sizeof *t);
    snprintf(t->orden, sizeof t->orden, "%s", orden);
    t->activo = 1;
    SDL_DetachThread(SDL_CreateThread(hilo_trabajo, "orden", t));
}

static Trabajo mira, instala, prepara, dialogo;
static char version_nueva[32], url_nueva[512];

// El diálogo de archivos del sistema: zenity o kdialog en Linux, osascript en macOS
static void abre_dialogo(void) {
#ifdef __APPLE__
    lanza(&dialogo, "osascript -e 'POSIX path of (choose file with prompt \"Mario Kart DS (.nds)\")' 2>/dev/null");
#else
    lanza(&dialogo, "if command -v zenity >/dev/null 2>&1; then zenity --file-selection --title='Mario Kart DS (.nds)' "
                    "--file-filter='*.nds' 2>/dev/null; elif command -v kdialog >/dev/null 2>&1; then "
                    "kdialog --getopenfilename . '*.nds' 2>/dev/null; else exit 7; fi");
#endif
}

// ---- la ventana -----------------------------------------------------------------------------------------------------
static SDL_Window *ventana;
static GLuint tex_logo;
static int logo_w, logo_h;
static GLuint tex_portada;  // 0 si no hay portada.mpg: se queda el fondo liso
static int portada_w, portada_h;

static void carga_portada(void) {
    if (!portada_abre("portada.mpg", &portada_w, &portada_h)) return;
    glGenTextures(1, &tex_portada);
    glBindTexture(GL_TEXTURE_2D, tex_portada);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, portada_w, portada_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static void avanza_portada(double dt) {
    if (!tex_portada) return;
    const uint8_t *px = portada_avanza(dt);
    if (!px) return;
    glBindTexture(GL_TEXTURE_2D, tex_portada);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, portada_w, portada_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

// La banda de arriba de Inicio: el vídeo a todo el ancho, recortado arriba y abajo (como UniformToFill en Windows) y
// fundido con el fondo por abajo. Devuelve su alto (0 si no hay vídeo)
static float pinta_portada(float an) {
    if (!tex_portada || an <= 0) return 0;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = an * 0.36f;
    float visible = (h / an) / ((float)portada_h / (float)portada_w);  // qué parte del alto del vídeo cabe
    if (visible > 1) visible = 1;
    float v0 = (1 - visible) / 2;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddImage((ImTextureID)(intptr_t)tex_portada, p, ImVec2(p.x + an, p.y + h), ImVec2(0, v0), ImVec2(1, v0 + visible));
    ImU32 fondo = ImGui::GetColorU32(ImGuiCol_WindowBg), nada = fondo & 0x00FFFFFFu;
    dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + h * 0.4f), ImVec2(p.x + an, p.y + h), nada, nada, fondo, fondo);
    return h;
}

static void carga_logo(void) {
    SDL_Surface *s = SDL_LoadBMP("linux/logo.bmp");
    if (!s) return;
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    if (!c) return;
    glGenTextures(1, &tex_logo);
    glBindTexture(GL_TEXTURE_2D, tex_logo);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, c->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    logo_w = c->w;
    logo_h = c->h;
    SDL_FreeSurface(c);
}

static void pon_estilo(void) {  // los colores del launcher de Windows
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 0;
    st.FrameRounding = 7;
    st.WindowBorderSize = 0;
    st.ItemSpacing = ImVec2(10, 10);
    st.FramePadding = ImVec2(12, 7);
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.067f, 0.078f, 0.161f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.125f, 0.145f, 0.29f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.19f, 0.38f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.2f, 0.22f, 0.44f, 1);
    c[ImGuiCol_Button] = ImVec4(0.125f, 0.145f, 0.29f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 0.30f, 0.24f, 0.8f);
    c[ImGuiCol_ButtonActive] = ImVec4(1.0f, 0.30f, 0.24f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.79f, 0.25f, 1);
    c[ImGuiCol_Text] = ImVec4(0.953f, 0.957f, 1, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.604f, 0.639f, 0.78f, 1);
}

static void carga_letra(void) {
    ImGuiIO &io = ImGui::GetIO();
    static const ImWchar latin[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0};
    const char *rutas[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
    };
    for (unsigned i = 0; i < sizeof rutas / sizeof *rutas; i++) {
        FILE *f = fopen(rutas[i], "rb");
        if (!f) continue;
        fclose(f);
        if (io.Fonts->AddFontFromFileTTF(rutas[i], 19, NULL, latin)) return;
    }
    io.Fonts->AddFontDefault();
}

static ImGuiKey tecla_imgui(SDL_Keycode k) {
    switch (k) {
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_TAB: return ImGuiKey_Tab;
    default: return ImGuiKey_None;
    }
}

static int pagina;
static char nombre[32];
static int salir, actualizado;

static void boton_opcion(const char *clave, const char *por_defecto, const char *texto, int valor) {
    int actual = atoi(ajuste(clave, por_defecto));
    if (actual == valor) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.30f, 0.24f, 1));
    int cambia = ImGui::Button(texto, ImVec2(82, 0));
    if (actual == valor) ImGui::PopStyleColor();
    if (cambia) pon_ajuste_n(clave, valor);
}

// Salas por internet (hasta 8, con el modo local): lo hace el juego (online_prueba en wifi.c), que enseña el código
static char sala_pedida[8];

static void juega(void) {
    SDL_HideWindow(ventana);
    SDL_PumpEvents();
    if (!strcmp(sala_pedida, "1")) setenv("MKDS_ONLINE_CREAR", "1", 1);
    else if (sala_pedida[0]) setenv("MKDS_ONLINE_UNIRSE", sala_pedida, 1);
    int r = system("./build/mkds");
    unsetenv("MKDS_ONLINE_CREAR");
    unsetenv("MKDS_ONLINE_UNIRSE");
    sala_pedida[0] = 0;
    SDL_ShowWindow(ventana);
    SDL_RaiseWindow(ventana);
    if (r) snprintf(aviso, sizeof aviso, "%s", T("El juego se ha cerrado con un error. Mira fallos.txt."));
}

static void pagina_inicio(void) {
    float an = ImGui::GetContentRegionAvail().x;
    float y0 = ImGui::GetCursorPosY(), banda = pinta_portada(an);
    if (banda > 0) ImGui::SetCursorPosY(y0 + banda * 0.62f);  // el logo, encima de la parte de abajo del vídeo
    if (tex_logo) {
        float w = 340, h = w * (float)logo_h / (float)(logo_w ? logo_w : 1);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (an - w) / 2);
        ImGui::Image((ImTextureID)(intptr_t)tex_logo, ImVec2(w, h));
    } else {
        ImGui::TextUnformatted("DS Kart Recompiled");
    }
    ImGui::Spacing();
    int ocupado = prepara.activo || instala.activo;
    int listo = hay_rom() && nombre[0];
    if (!listo || ocupado) ImGui::BeginDisabled();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (an - 260) / 2);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.30f, 0.24f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.42f, 0.3f, 1));
    if (ImGui::Button(T("JUGAR"), ImVec2(260, 58))) {
        aviso[0] = 0;
        lanza(&prepara, "sh linux/preparar.sh 2>&1");
    }
    ImGui::PopStyleColor(2);
    if (!listo || ocupado) ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("Sala por internet, hasta 8 jugadores"));
    if (!listo || ocupado) ImGui::BeginDisabled();
    if (ImGui::Button(T("Crear sala"))) {
        aviso[0] = 0;
        strcpy(sala_pedida, "1");
        lanza(&prepara, "sh linux/preparar.sh 2>&1");
    }
    ImGui::SameLine();
    static char codigo[8];
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("##codigo", codigo, 7, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    if (strlen(codigo) != 6) ImGui::BeginDisabled();
    if (ImGui::Button(T("Unirse"))) {
        aviso[0] = 0;
        snprintf(sala_pedida, sizeof sala_pedida, "%s", codigo);
        lanza(&prepara, "sh linux/preparar.sh 2>&1");
    }
    if (strlen(codigo) != 6) ImGui::EndDisabled();
    if (!listo || ocupado) ImGui::EndDisabled();
    if (!hay_rom()) ImGui::TextDisabled("%s", T("Falta la ROM: elígela en Ajustes"));
    else if (!nombre[0]) ImGui::TextDisabled("%s", T("Pon tu nombre en Ajustes para poder jugar"));
    if (prepara.activo) {
        ImGui::TextWrapped("%s", T("Preparando el juego (la primera vez tarda unos minutos)..."));
        ImGui::TextDisabled("%s", prepara.ultima);
    }
    if (version_nueva[0]) {
        char t[128];
        ImGui::Spacing();
        snprintf(t, sizeof t, T("Hay una versión nueva: %s"), version_nueva);
        ImGui::TextUnformatted(t);
        snprintf(t, sizeof t, T("Actualizar a la %s"), version_nueva);
        if (ocupado) ImGui::BeginDisabled();
        if (ImGui::Button(t)) {
            char o[700];
            snprintf(o, sizeof o, "python3 linux/actualiza.py instala '%s' 2>&1", url_nueva);
            lanza(&instala, o);
        }
        if (ocupado) ImGui::EndDisabled();
    }
    if (instala.activo) ImGui::TextUnformatted(T("Actualizando..."));
    if (aviso[0]) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", aviso);
}

static void pagina_ajustes(void) {
    ImGui::TextDisabled("%s", T("Tu ROM"));
    ImGui::TextUnformatted(hay_rom() ? T("ROM lista") : T("Falta la ROM: elígela en Ajustes"));
    ImGui::SameLine();
    int eligiendo = dialogo.activo;  // el botón lo cambia: el Begin y el End tienen que ir a la par
    if (eligiendo) ImGui::BeginDisabled();
    if (ImGui::Button(T("Elegir ROM"))) { aviso[0] = 0; abre_dialogo(); }
    if (eligiendo) ImGui::EndDisabled();
    ImGui::TextDisabled("%s", T("También puedes arrastrar el archivo .nds a esta ventana."));
    if (aviso[0]) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", aviso);

    ImGui::TextDisabled("%s", T("Tu nombre"));
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputText("##nombre", nombre, 11)) pon_ajuste("NOMBRE", nombre);

    ImGui::TextDisabled("%s", T("Idioma"));
    for (int i = 0; i < 5; i++) {
        if (i) ImGui::SameLine();
        if (idioma == i) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.30f, 0.24f, 1));
        int c = ImGui::Button(nombres_idioma[i]);
        if (idioma == i) ImGui::PopStyleColor();
        if (c) {
            idioma = i;
            pon_ajuste("LAUNCHER_IDIOMA", idiomas[i]);
            pon_ajuste_n("IDIOMA", idioma_juego[i]);
        }
    }

    ImGui::TextDisabled("%s", T("Resolución del 3D"));
    const char *esc[] = {"1x", "2x", "3x", "4x"};
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        boton_opcion("ESCALA", "2", esc[i], i + 1);
    }
    ImGui::TextDisabled("%s", T("Imágenes por segundo"));
    const char *fps[] = {"60", "120", "180", "240"};
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        boton_opcion("FPS", "60", fps[i], (i + 1) * 60);
    }
    bool b = atoi(ajuste("PANTALLA_COMPLETA", "0")) != 0;
    if (ImGui::Checkbox(T("Pantalla completa"), &b)) pon_ajuste_n("PANTALLA_COMPLETA", b);
    b = atoi(ajuste("GPU", "0")) != 0;
    if (ImGui::Checkbox(T("3D con la tarjeta gráfica (experimental)"), &b)) pon_ajuste_n("GPU", b);
    b = atoi(ajuste("MICRO", "1")) != 0;
    if (ImGui::Checkbox(T("Micrófono (para soplar)"), &b)) pon_ajuste_n("MICRO", b);
}

static void atiende_trabajos(void) {
    if (mira.activo && mira.fin) {
        mira.activo = 0;
        if (sscanf(mira.ultima, "%31s %511s", version_nueva, url_nueva) != 2) version_nueva[0] = 0;
    }
    if (dialogo.activo && dialogo.fin) {
        dialogo.activo = 0;
        if (WEXITSTATUS(dialogo.salida) == 7) snprintf(aviso, sizeof aviso, "%s", T("También puedes arrastrar el archivo .nds a esta ventana."));
        else if (dialogo.ultima[0]) elige_rom(dialogo.ultima);
    }
    if (instala.activo && instala.fin) {
        instala.activo = 0;
        if (instala.salida) snprintf(aviso, sizeof aviso, "%s %s", T("No se pudo actualizar"), instala.ultima);
        else { actualizado = 1; salir = 1; }
    }
    if (prepara.activo && prepara.fin) {
        prepara.activo = 0;
        if (prepara.salida) snprintf(aviso, sizeof aviso, "%s %s", T("No se pudo preparar el juego:"), prepara.ultima);
        else juega();
    }
}

extern "C" int main(int argc, char **argv) {
    (void)argc; (void)argv;
    lee_ajustes();
    const char *id = ajuste("LAUNCHER_IDIOMA", NULL);
    int guardado = id != NULL;
    if (!id) id = getenv("LANG");
    int visto = 0;
    for (int i = 0; id && i < 5; i++)
        if (!strncmp(id, idiomas[i], 2)) { idioma = i; visto = 1; }
    if (!guardado) {
        // La primera vez (jugar.sh crea ajustes.ini solo con IDIOMA): el idioma del sistema, o si no es uno de los 5,
        // el del juego. Se guardan los dos para que el juego y sus textos (el menú F1, la sala) vayan igual.
        if (!visto)
            for (int i = 0; i < 5; i++)
                if (idioma_juego[i] == atoi(ajuste("IDIOMA", "5"))) idioma = i;
        pon_ajuste("LAUNCHER_IDIOMA", idiomas[idioma]);
        pon_ajuste_n("IDIOMA", idioma_juego[idioma]);
    }
    snprintf(nombre, sizeof nombre, "%s", ajuste("NOMBRE", ""));
    if (!strcmp(nombre, "Jugador")) nombre[0] = 0;  // el que deja jugar.sh la primera vez

    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "[launcher] SDL: %s\n", SDL_GetError()); return SALIDA_SIN_VENTANA; }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    ventana = SDL_CreateWindow("DS Kart Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 760, 640,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!ventana) { fprintf(stderr, "[launcher] sin ventana: %s\n", SDL_GetError()); return SALIDA_SIN_VENTANA; }
    SDL_GLContext ctx = SDL_GL_CreateContext(ventana);
    if (!ctx) { fprintf(stderr, "[launcher] sin OpenGL: %s\n", SDL_GetError()); return SALIDA_SIN_VENTANA; }
    SDL_GL_SetSwapInterval(1);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    pon_estilo();
    carga_letra();
    ImGui_ImplOpenGL2_Init();
    carga_logo();
    carga_portada();
    lanza(&mira, "python3 linux/actualiza.py mira 2>/dev/null");

    Uint64 antes = SDL_GetPerformanceCounter();
    while (!salir) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: salir = 1; break;
            case SDL_MOUSEMOTION: io.AddMousePosEvent((float)ev.motion.x, (float)ev.motion.y); break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) io.AddMouseButtonEvent(0, ev.type == SDL_MOUSEBUTTONDOWN);
                break;
            case SDL_MOUSEWHEEL: io.AddMouseWheelEvent(0, (float)ev.wheel.y); break;
            case SDL_TEXTINPUT: io.AddInputCharactersUTF8(ev.text.text); break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                ImGuiKey k = tecla_imgui(ev.key.keysym.sym);
                if (k != ImGuiKey_None) io.AddKeyEvent(k, ev.type == SDL_KEYDOWN);
                break;
            }
            case SDL_DROPFILE:
                aviso[0] = 0;
                elige_rom(ev.drop.file);
                SDL_free(ev.drop.file);
                break;
            }
        }
        atiende_trabajos();
        int w, h, fw, fh;
        SDL_GetWindowSize(ventana, &w, &h);
        SDL_GL_GetDrawableSize(ventana, &fw, &fh);
        Uint64 ahora = SDL_GetPerformanceCounter();
        io.DeltaTime = (float)((double)(ahora - antes) / (double)SDL_GetPerformanceFrequency());
        if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60;
        antes = ahora;
        if (pagina == 0) avanza_portada(io.DeltaTime);
        io.DisplaySize = ImVec2((float)w, (float)h);
        io.DisplayFramebufferScale = ImVec2(w ? (float)fw / (float)w : 1, h ? (float)fh / (float)h : 1);
        ImGui_ImplOpenGL2_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
        ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
        for (int i = 0; i < 2; i++) {
            if (i) ImGui::SameLine();
            if (pagina == i) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.30f, 0.24f, 1));
            int c = ImGui::Button(T(i ? "Ajustes" : "Inicio"), ImVec2(150, 0));
            if (pagina == i) ImGui::PopStyleColor();
            if (c) pagina = i;
        }
        ImGui::Separator();
        ImGui::Spacing();
        if (pagina) pagina_ajustes();
        else pagina_inicio();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
        ImGui::TextDisabled("%s", T("Hecho por alontube junto con Claude. Proyecto de aficionado sin relación con Nintendo."));
        ImGui::End();
        ImGui::Render();
        glViewport(0, 0, fw, fh);
        glClearColor(0.067f, 0.078f, 0.161f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(ventana);
        // mientras no se hace nada, sin gastar CPU
        if (!prepara.activo && !instala.activo) SDL_Delay(8);
    }
    portada_cierra();
    ImGui_ImplOpenGL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(ventana);
    SDL_Quit();
    return actualizado ? SALIDA_ACTUALIZADO : 0;
}
