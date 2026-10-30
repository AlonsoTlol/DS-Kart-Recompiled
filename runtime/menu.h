// Menú de ajustes dentro del juego (menu.cpp, con Dear ImGui) y contador de FPS en pantalla.
//
// Lo usan las dos ventanas (ventana.c en Windows, ventana_sdl.c en Linux): les da igual cómo está hecho.
//   - Desde el hilo de la ventana (el del juego): las teclas, el ratón y el mando, con menu_tecla, menu_raton...
//     Se guardan en una cola; el menú solo hace caso mientras está abierto (salvo menu_alterna).
//   - Desde el hilo que pinta con OpenGL: menu_inicia una vez y menu_pinta en cada imagen, justo antes de
//     cambiar de búfer. Si no hay OpenGL (MKDS_GDI) no hay menú.
// Se abre y se cierra con F1 o con los dos sticks del mando a la vez (L3 + R3). Mientras está abierto el juego
// no recibe los botones. Al cerrarlo guarda los ajustes en ajustes.ini (los mismos que escribe el launcher).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// teclas que entiende el menú (las de mover y escribir; lo demás llega como texto)
enum {
    MT_TAB = 1, MT_IZQ, MT_DER, MT_ARRIBA, MT_ABAJO, MT_INICIO, MT_FIN, MT_BORRAR, MT_SUPR, MT_ENTER, MT_ESC,
    MT_CTRL, MT_SHIFT,
    // el mando (botones de la DS ya traducidos por la ventana)
    MT_PAD_ARRIBA, MT_PAD_ABAJO, MT_PAD_IZQ, MT_PAD_DER, MT_PAD_ACEPTA, MT_PAD_ATRAS, MT_PAD_L, MT_PAD_R,
};

void menu_alterna(void);           // abre / cierra
int menu_abierto(void);
void menu_tecla(int tecla, int pulsada);
void menu_caracter(unsigned c);    // una letra escrita (Unicode)
void menu_raton(float x, float y); // en puntos de la ventana
void menu_boton(int boton, int pulsado);  // 0 izquierdo, 1 derecho
void menu_rueda(float dy);
// el mando mientras el menú está abierto: botones de la DS (bits a 0 = pulsado, como KEYINPUT)
void menu_mando(unsigned short in);

int menu_inicia(void);  // en el hilo de OpenGL, con el contexto ya hecho; 0 si no se puede
void menu_pinta(int ancho, int alto, int ancho_px, int alto_px);  // ventana en puntos y en píxeles

void menu_pon_fps(int juego, int tres_d, int pantalla);  // cada segundo (ventana_titulo_fps)

// lo que da cada ventana al menú
int ventana_pantalla_completa(void);
void ventana_pide_pantalla_completa(int si);  // se hace en el hilo de la ventana (ventana_mensajes)
int ventana_mandos(char nombres[][64], int max);  // los mandos conectados

#ifdef __cplusplus
}
#endif
