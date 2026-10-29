// Lanzador de Mario Kart DS recompilado.
// Deja la memoria como la dejaría la DS al encender (código de los dos
// procesadores, cabecera de la ROM) y los arranca. El hardware está en ds.c.
#include <windows.h>  // antes que cpu.h: su macro R choca con windows.h
#include "ds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern volatile uint32_t g_actividad;

// ---- trucos --------------------------------------------------------------
uint32_t g_trucos;

// ¿Está desbloqueado el elemento r0? (personajes, karts, circuitos...): sí
int truco_desbloqueado(Cpu *c) {
    if (!(g_trucos & 1)) return 0;
    c->r[0] = 1;
    return 1;
}

// Cilindrada más alta disponible: 4 = 50, 100, 150 y espejo
int truco_cilindradas(Cpu *c) {
    if (!(g_trucos & 1)) return 0;
    c->r[0] = 4;
    return 1;
}

// Misiones: el menú (overlay 1, 0219F3A4) guarda en objeto+0x4A el último nivel abierto (0 = solo el 1; se abre
// uno al superar las 9 misiones del anterior, y el 7 pide además rango ★ en todas). El objeto está en 021CA410.
int truco_niveles_mision(Cpu *c) {
    if (!(g_trucos & 1)) return 0;
    uint32_t obj = rd32_9(0x021CA410);
    if ((obj & 0xFF000000) == 0x02000000) wr16_9(obj + 0x4A, 6);
    return 1;
}

// y el jefe de cada nivel (021A0E34): objeto (021CA414) + 0xA0 = 1 si las misiones 1-8 están superadas
int truco_jefe_mision(Cpu *c) {
    if (!(g_trucos & 1)) return 0;
    uint32_t obj = rd32_9(0x021CA414);
    if ((obj & 0xFF000000) == 0x02000000) wr32_9(obj + 0xA0, 1);
    return 1;
}

static void volcado(Cpu *c) {
    fprintf(stderr, "  %s en la función %08X (%s)\n", c->id ? "ARM7" : "ARM9", c->func & ~1u,
            (c->func & 1) ? "Thumb" : "ARM");
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "    r%-2d %08X%s", i, c->r[i], i % 4 == 3 ? "\n" : "");
    extern void ds_info(int id);
    ds_info(c->id);
    if (c->id) {
        fprintf(stderr, "    CPSR %08X  pila:", cpu_get_cpsr(c));
        for (int i = 0; i < 24; i++) fprintf(stderr, " %08X", rd32_7(c->r[13] + 4 * i));
        fprintf(stderr, "\n");
    }
    if (!c->id) {  // hilo actual del SDK (OSi_CurrentThread) y su prioridad
        uint32_t t = rd32_9(rd32_9(0x0216FF54));
        fprintf(stderr, "    hilo actual del juego: %08X\n", t);
        fprintf(stderr, "    datos de usuario en 027FFC80:");  // nombre (UTF-16 en +6, longitud en +1A)
        for (int i = 0; i < 0x20; i += 2) fprintf(stderr, " %04X", rd16_9(0x027FFC80 + i));
        fprintf(stderr, "\n");
        fprintf(stderr, "    CPSR %08X  pila:", cpu_get_cpsr(c));
        for (int i = 0; i < 24; i++) fprintf(stderr, " %08X", rd32_9(c->r[13] + 4 * i));
        fprintf(stderr, "\n");
        // MKDS_BUSCA="texto": dónde está ese texto en UTF-16 en la memoria principal (p. ej. el nombre)
        const char *busca = getenv("MKDS_BUSCA");
        if (busca && *busca) {
            size_t n = strlen(busca);
            fprintf(stderr, "    \"%s\" en UTF-16:", busca);
            for (uint32_t a = 0x02000000; a + 2 * n <= 0x02400000; a += 2) {
                size_t k = 0;
                while (k < n && rd16_9(a + 2 * (uint32_t)k) == (uint8_t)busca[k]) k++;
                if (k == n) fprintf(stderr, " %08X", a);
            }
            fprintf(stderr, "\n");
        }
        // MKDS_VER="02174E00 *02174E00": palabras a mirar (con * se sigue el puntero)
        const char *ver = getenv("MKDS_VER");
        while (ver && *ver) {
            int ptr = *ver == '*';
            uint32_t a = (uint32_t)strtoul(ver + ptr, (char **)&ver, 16);
            if (ptr) a = rd32_9(a);
            fprintf(stderr, "    [%08X] = %08X\n", a, rd32_9(a));
            while (*ver == ' ') ver++;
        }
    }
}

// ---- vigilante: si el juego se atasca sin tocar el hardware, dice dónde ----
static DWORD WINAPI vigilante(LPVOID p) {
    (void)p;
    uint32_t antes = ~0u;
    const char *lim = getenv("MKDS_LIMITE");  // segundos; al pasar, vuelca el estado y sale
    int limite = lim ? atoi(lim) : 0, pasados = 0;
    for (;;) {
        Sleep(3000);
        pasados += 3;
        if (limite && pasados >= limite) {
            fprintf(stderr, "\n[mkds] %d s: así están los dos procesadores\n", pasados);
            volcado(&g_cpu9);
            volcado(&g_cpu7);
            const char *vr = getenv("MKDS_VUELCA_RAM");  // también al acabar el tiempo: memoria para estudiarla
            if (vr && *vr) {
                extern uint8_t g_itcm[0x8000], g_dtcm[0x4000];
                FILE *m = fopen(vr, "wb");
                if (m) { fwrite(g_main, 1, 0x400000, m); fwrite(g_itcm, 1, 0x8000, m); fwrite(g_dtcm, 1, 0x4000, m); fclose(m); }
            }
            ExitProcess(10);
        }
        if (g_actividad == antes) {
            fprintf(stderr, "\n[mkds] atascado 3 s sin tocar el hardware\n");
            volcado(&g_cpu9);
            volcado(&g_cpu7);
            ExitProcess(7);
        }
        antes = g_actividad;
    }
}

static LONG WINAPI al_romperse(EXCEPTION_POINTERS *e) {
    DWORD cod = e->ExceptionRecord->ExceptionCode;
    if (cod != EXCEPTION_ACCESS_VIOLATION && cod != EXCEPTION_STACK_OVERFLOW &&
        cod != EXCEPTION_ILLEGAL_INSTRUCTION && cod != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n[mkds] fallo %08lX en el PC (mkds.exe+%llX)\n", e->ExceptionRecord->ExceptionCode,
            (unsigned long long)((char *)e->ExceptionRecord->ExceptionAddress -
                                 (char *)GetModuleHandle(NULL)));
    volcado(&g_cpu9);
    volcado(&g_cpu7);
    fflush(stderr);
    ExitProcess(9);
}

static void *carga(const char *ruta, size_t *n) {
    FILE *f = fopen(ruta, "rb");
    if (!f) { perror(ruta); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc(*n);
    fread(p, 1, *n, f);
    fclose(f);
    return p;
}

// Nombre del jugador (NOMBRE en ajustes.ini, lo pone el launcher). La partida cargada está en 0x023CE2C8
// (empieza por "NKSY") y el nombre va detrás, en UTF-16, hasta 10 letras. El juego lo copia del firmware al
// crear la partida; la del usuario se creó con el nombre vacío y el juego no deja cambiarlo. Se escribe en
// cada fotograma si no coincide; al guardar, el juego lo deja ya en la partida.
void nombre_por_frame(void) {
    static int listo;
    static uint16_t nombre[10];
    if (!listo) {
        listo = 1;
        const char *n = getenv("MKDS_NOMBRE");
        wchar_t w[11] = {0};
        if (n && *n) MultiByteToWideChar(CP_UTF8, 0, n, -1, w, 11);
        w[10] = 0;
        for (int i = 0; i < 10; i++) nombre[i] = (uint16_t)w[i];
        if (!nombre[0]) listo = 2;  // sin nombre: no se toca nada
    }
    if (listo == 2) return;
    uint8_t *p = g_main + (0x023CE2C8 & 0x3FFFFF);
    if (memcmp(p, "NKSY", 4)) return;  // la partida aún no está cargada
    if (memcmp(p + 4, nombre, sizeof nombre)) memcpy(p + 4, nombre, sizeof nombre);
}

// ajustes.ini (lo escribe el launcher): líneas CLAVE=valor que se convierten en las variables de siempre
// (ESCALA -> MKDS_ESCALA, PANTALLA_COMPLETA -> MKDS_PANTALLA_COMPLETA, NOMBRE -> MKDS_NOMBRE).
// Lo que ya venga puesto en el entorno manda.
static void lee_ajustes(void) {
    FILE *f = fopen("ajustes.ini", "r");
    if (!f) return;
    char linea[256];
    while (fgets(linea, sizeof linea, f)) {
        linea[strcspn(linea, "\r\n")] = 0;
        char *igual = strchr(linea, '=');
        if (!igual || linea[0] == '#' || linea[0] == ';' || linea[0] == '[') continue;
        *igual = 0;
        char var[64];
        snprintf(var, sizeof var, "MKDS_%s", linea);
        if (!getenv(var)) _putenv_s(var, igual + 1);
    }
    fclose(f);
}

// ---- el código de los dos procesadores, sacado directamente de la ROM del usuario ----------------------
// (así basta con elegir la ROM en el launcher; antes hacía falta extraido/ de tools/nds_rom.py)
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// "Backwards LZ" del SDK (MI_UncompressBackward), igual que blz_decompress en tools/nds_rom.py:
// los últimos 8 bytes dicen cuánto crece y qué parte está comprimida; se lee del final al principio.
static uint8_t *blz(const uint8_t *buf, size_t len, size_t *sal) {
    uint32_t crece = le32(buf + len - 4);
    uint8_t *out = malloc(len + crece);
    memcpy(out, buf, len);
    *sal = len + crece;
    if (!crece) return out;
    uint32_t cab = le32(buf + len - 8);
    size_t cab_len = cab >> 24, cod_len = cab & 0xFFFFFF;
    size_t crudo = len - cod_len;  // lo de antes va sin comprimir
    size_t src = len - cab_len, dst = len + crece;
    while (dst > crudo && src > crudo) {
        uint8_t flags = buf[--src];
        for (int k = 0; k < 8 && dst > crudo && src > crudo; k++, flags <<= 1) {
            if (flags & 0x80) {
                uint8_t b1 = buf[src - 1], b2 = buf[src - 2];
                src -= 2;
                int largo = (b1 >> 4) + 3, desp = (((b1 & 15) << 8) | b2) + 3;
                for (int i = 0; i < largo && dst > 0; i++, dst--) out[dst - 1] = out[dst - 1 + desp];
            } else {
                out[--dst] = buf[--src];
            }
        }
    }
    return out;
}

static void carga_codigo(const uint8_t *rom, size_t n) {
    uint32_t a9o = le32(rom + 0x20), a9ram = le32(rom + 0x28), a9t = le32(rom + 0x2C);
    uint32_t a7o = le32(rom + 0x30), a7ram = le32(rom + 0x38), a7t = le32(rom + 0x3C);
    if ((size_t)a9o + a9t > n || (size_t)a7o + a7t > n) { fprintf(stderr, "ROM incompleta\n"); exit(1); }
    const uint8_t *crudo = rom + a9o;
    uint8_t *arm9 = malloc(a9t);
    memcpy(arm9, crudo, a9t);
    size_t t9 = a9t;
    static const uint8_t magia[8] = {0x21, 0x06, 0xC0, 0xDE, 0xDE, 0xC0, 0x06, 0x21};
    for (uint32_t i = 0x1C; i + 8 <= a9t; i += 4) {
        if (memcmp(crudo + i, magia, 8)) continue;
        uint32_t mp = i - 0x1C, fin = le32(crudo + mp + 0x14);  // compressed_static_end
        if (fin) {
            size_t corte = fin - a9ram, td;
            uint8_t *dec = blz(crudo, corte, &td);
            free(arm9);
            t9 = td + (a9t - corte);
            arm9 = malloc(t9);
            memcpy(arm9, dec, td);
            memcpy(arm9 + td, crudo + corte, a9t - corte);
            free(dec);
            memset(arm9 + mp + 0x14, 0, 4);  // ya descomprimido: que el arranque no lo intente otra vez
        }
        break;
    }
    memcpy(g_main + (a9ram & 0x3FFFFF), arm9, t9);
    memcpy(g_main + (a7ram & 0x3FFFFF), rom + a7o, a7t);
    if (getenv("MKDS_COMPARA")) {  // comprobación: ¿sale igual que extraido/ de nds_rom.py?
        size_t m;
        uint8_t *e9 = carga("extraido/arm9.bin", &m);
        fprintf(stderr, "[mkds] ARM9 %s (%zu / %zu bytes)\n", m == t9 && !memcmp(e9, arm9, m) ? "igual" : "DISTINTO", t9, m);
        uint8_t *e7 = carga("extraido/arm7.bin", &m);
        fprintf(stderr, "[mkds] ARM7 %s\n", m == a7t && !memcmp(e7, rom + a7o, m) ? "igual" : "DISTINTO");
        free(e9); free(e7);
    }
    free(arm9);
}

// La carpeta del juego es la del launcher (rom\, ajustes.ini, partida.sav, repeticiones\), una por encima de
// build\. Si se abre build\mkds.exe con doble clic, Windows arranca en build\ y no encontraba la ROM.
static void ve_a_la_carpeta_del_juego(void) {
    wchar_t ruta[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, ruta, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t *barra = wcsrchr(ruta, L'\\');
    if (!barra) return;
    *barra = 0;  // carpeta del .exe
    barra = wcsrchr(ruta, L'\\');
    if (barra && !_wcsicmp(barra + 1, L"build")) *barra = 0;
    SetCurrentDirectoryW(ruta);
}

int main(void) {
    ve_a_la_carpeta_del_juego();
    lee_ajustes();
    size_t n;
    FILE *hay = fopen("rom/mkds.nds", "rb");
    if (!hay) {
        MessageBoxW(NULL, L"No encuentro el juego (rom\\mkds.nds).\nElige tu ROM desde el launcher.",
                    L"DS Kart Recompiled", MB_ICONWARNING);
        return 1;
    }
    fclose(hay);
    uint8_t *rom = carga("rom/mkds.nds", &n);
    carga_codigo(rom, n);                     // ARM9 en 0x02000000, ARM7 en 0x02380000
    memcpy(g_main + 0x3FFE00, rom, 0x170);   // la cabecera queda en 0x027FFE00
    ds_pon_rom(rom, n);

    // Sistema operativo del juego (NitroSDK): direcciones sacadas del desensamblado
    g_so[0] = (SoConfig){
        .cp_save = 0x02146C24, .cp_restore = 0x02146BB8, .reschedule = 0x0200DE50,
        .cola_irq = 0x027E0000, .flag_resched = 0x0216FF5C,
        .off_estado = 0x64, .off_enlace = 0x78, .off_sig = 0x80,
    };
    g_so[1] = (SoConfig){
        .reschedule = 0x037F8F48, .cola_irq = 0x0380766C, .flag_resched = 0x038076FC,
        .off_estado = 0x48, .off_enlace = 0x5C, .off_sig = 0x64,
    };

    // Todo desbloqueado viene siempre de serie (lo pidió el usuario: no es una opción).
    // Solo para depurar: la variable de entorno TODO_DESBLOQUEADO=0 lo quita.
    if (!getenv("TODO_DESBLOQUEADO") || atoi(getenv("TODO_DESBLOQUEADO"))) {
        g_trucos |= 1;
        fprintf(stderr, "[mkds] todo desbloqueado\n");
    }
    CreateThread(NULL, 0, vigilante, NULL, 0, NULL);
    AddVectoredExceptionHandler(1, al_romperse);
    ds_arranca(0x02000800, 0x02380000);
    return 0;
}
