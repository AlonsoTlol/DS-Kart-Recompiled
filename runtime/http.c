// Peticiones HTTP(S) sencillas para el servidor de salas del online (wifi.c y wfc_servicios.c): POST, respuesta en texto.
// En Windows con WinHTTP (viene con Windows); en Linux con el programa curl (lo traen casi todas las
// distribuciones, SteamOS incluido). Se llama desde el hilo del online, nunca desde el del juego: puede tardar.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

// devuelve el código HTTP (200...) o -1; la respuesta queda en 'r' (acabada en 0). 'cuerpo' puede ser NULL;
// 'espera' es lo máximo que se espera la respuesta, en ms.
int http_pide(const char *url, const char *cuerpo, char *r, int max, int espera) {
    r[0] = 0;
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 1024);
    URL_COMPONENTS u = {sizeof u};
    wchar_t host[256], ruta[768];
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = ruta; u.dwUrlPathLength = 768;
    u.dwSchemeLength = 1; u.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(w, 0, 0, &u)) return -1;
    if (u.lpszExtraInfo && u.dwExtraInfoLength)  // la consulta (?a=b) va con la ruta
        wcsncat(ruta, u.lpszExtraInfo, u.dwExtraInfoLength);
    int codigo = -1;
    HINTERNET s = WinHttpOpen(L"DS-Kart-Recompiled", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, u.nPort, 0) : NULL;
    HINTERNET q = c ? WinHttpOpenRequest(c, L"POST", ruta, NULL, NULL, NULL,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (q) WinHttpSetTimeouts(q, 5000, 5000, 5000, espera);
    DWORD largo = cuerpo ? (DWORD)strlen(cuerpo) : 0;
    if (q && WinHttpSendRequest(q, NULL, 0, (LPVOID)cuerpo, largo, largo, 0) && WinHttpReceiveResponse(q, NULL)) {
        DWORD v = 0, t = sizeof v;
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &v, &t, NULL);
        codigo = (int)v;
        int n = 0;
        DWORD leido;
        while (n < max - 1 && WinHttpReadData(q, r + n, (DWORD)(max - 1 - n), &leido) && leido) n += (int)leido;
        r[n] = 0;
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return codigo;
}
#else
#include <unistd.h>
int http_pide(const char *url, const char *cuerpo, char *r, int max, int espera) {
    r[0] = 0;
    for (const char *p = url; *p; p++)  // la URL va entre comillas simples: nada raro dentro
        if (*p == '\'' || *p == '\\' || (unsigned char)*p < 32) return -1;
    char orden[1400], archivo[64] = "";
    if (cuerpo) {  // el cuerpo, por un archivo temporal (así no hay que preocuparse de qué lleva)
        snprintf(archivo, sizeof archivo, "/tmp/dskart-XXXXXX");
        int fd = mkstemp(archivo);
        if (fd < 0) return -1;
        if (write(fd, cuerpo, strlen(cuerpo)) < 0) { close(fd); unlink(archivo); return -1; }
        close(fd);
    }
    snprintf(orden, sizeof orden, "curl -s -m %d -X POST %s%s%s -w '\\n%%{http_code}' '%s' 2>/dev/null",
             (espera + 999) / 1000 + 5, archivo[0] ? "--data-binary '@" : "", archivo, archivo[0] ? "'" : "", url);
    FILE *f = popen(orden, "r");
    if (!f) { if (archivo[0]) unlink(archivo); return -1; }
    int n = (int)fread(r, 1, (size_t)max - 1, f);
    pclose(f);
    if (archivo[0]) unlink(archivo);
    if (n <= 0) return -1;
    r[n] = 0;
    char *ult = strrchr(r, '\n');  // la última línea es el código HTTP (-w)
    if (!ult) return -1;
    int codigo = atoi(ult + 1);
    *ult = 0;
    return codigo ? codigo : -1;
}
#endif

int http_post(const char *url, char *r, int max) { return http_pide(url, NULL, r, max, 5000); }
