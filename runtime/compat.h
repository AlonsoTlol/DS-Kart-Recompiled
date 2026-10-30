// Lo poco de Windows que usa el runtime, para compilarlo también en Linux.
//
// En Windows es <windows.h> tal cual. En Linux se hace aquí lo mismo con lo de siempre de POSIX:
//   - hilos (CreateThread) con pthreads, y eventos de reinicio automático (CreateEvent, SetEvent,
//     WaitForSingleObject, WaitForMultipleObjects con "todos") con mutex + condición;
//   - fibras (los hilos de la DS, ver hilos.c) con ucontext: cada una con su pila;
//   - el reloj de alta resolución (QueryPerformanceCounter) con CLOCK_MONOTONIC, en nanosegundos;
//   - Sleep, GetTickCount, Interlocked*, secciones críticas, UTF-8 → UTF-16/32 (MultiByteToWideChar).
// Solo lo que hace falta y como lo usa el runtime (por ejemplo, ningún evento es de reinicio manual).
// Incluir antes que cpu.h, igual que windows.h (su macro R choca).
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <wchar.h>

typedef void *HANDLE;
typedef void *LPVOID;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef uintptr_t DWORD_PTR;
typedef union { int64_t QuadPart; } LARGE_INTEGER;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define MAX_PATH 4096
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_ABOVE_NORMAL 1

static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *t) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    return 1;
}
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000; return 1; }
static inline DWORD GetTickCount(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (DWORD)(t.QuadPart / 1000000);
}
static inline void Sleep(DWORD ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
static inline unsigned timeBeginPeriod(unsigned ms) { (void)ms; return 0; }  // en Linux Sleep(1) ya es de 1 ms

static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }

// ---- hilos ----
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef struct { LPTHREAD_START_ROUTINE f; LPVOID arg; } CompatArranque;
static void *compat_hilo(void *p) {
    CompatArranque a = *(CompatArranque *)p;
    free(p);
    a.f(a.arg);
    return NULL;
}
static inline HANDLE CreateThread(void *seg, size_t pila, LPTHREAD_START_ROUTINE f, LPVOID arg, DWORD flags, DWORD *id) {
    (void)seg; (void)flags; (void)id;
    CompatArranque *a = malloc(sizeof *a);
    a->f = f;
    a->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, pila > (8u << 20) ? pila : (8u << 20));
    pthread_t h;
    int r = pthread_create(&h, &at, compat_hilo, a);
    pthread_attr_destroy(&at);
    return r ? NULL : (HANDLE)(uintptr_t)1;
}
static inline BOOL SwitchToThread(void) { return sched_yield() == 0; }
static inline BOOL SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return 1; }
static inline BOOL CloseHandle(HANDLE h) { (void)h; return 1; }

// ---- eventos (siempre de reinicio automático) ----
typedef struct { pthread_mutex_t m; pthread_cond_t c; int puesto; } CompatEvento;
static inline HANDLE CreateEventW(void *seg, BOOL manual, BOOL inicial, const void *nombre) {
    (void)seg; (void)manual; (void)nombre;
    CompatEvento *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->puesto = inicial ? 1 : 0;
    return e;
}
static inline BOOL SetEvent(HANDLE h) {
    CompatEvento *e = h;
    pthread_mutex_lock(&e->m);
    e->puesto = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
    return 1;
}
static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    CompatEvento *e = h;
    pthread_mutex_lock(&e->m);
    if (ms == INFINITE) {
        while (!e->puesto) pthread_cond_wait(&e->c, &e->m);
    } else if (!e->puesto && ms) {
        struct timespec fin;
        clock_gettime(CLOCK_REALTIME, &fin);
        fin.tv_sec += ms / 1000;
        fin.tv_nsec += (long)(ms % 1000) * 1000000;
        if (fin.tv_nsec >= 1000000000) { fin.tv_sec++; fin.tv_nsec -= 1000000000; }
        while (!e->puesto && pthread_cond_timedwait(&e->c, &e->m, &fin) != ETIMEDOUT) {}
    }
    DWORD r = e->puesto ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    e->puesto = 0;
    pthread_mutex_unlock(&e->m);
    return r;
}
// solo "esperar a todos" sin límite (lo único que usa gpu3d.c): de uno en uno da lo mismo
static inline DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL todos, DWORD ms) {
    (void)todos; (void)ms;
    for (DWORD i = 0; i < n; i++) WaitForSingleObject(hs[i], INFINITE);
    return WAIT_OBJECT_0;
}

// ---- secciones críticas ----
typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *c) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(c, &a);
    pthread_mutexattr_destroy(&a);
}
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_lock(c); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_unlock(c); }

// ---- fibras (ucontext) ----
typedef void (*LPFIBER_START_ROUTINE)(void *);
typedef struct { ucontext_t ctx; LPFIBER_START_ROUTINE f; void *arg; } CompatFibra;
extern __thread CompatFibra *compat_fibra_actual;  // una sola para todo el programa (está en main_mkds.c)
static void compat_fibra_entra(unsigned hi, unsigned lo) {
    CompatFibra *fb = (CompatFibra *)(((uintptr_t)hi << 32) | (uintptr_t)lo);
    fb->f(fb->arg);
    exit(8);  // una fibra no debería terminar (hilos.c sale antes)
}
static inline void *ConvertThreadToFiber(void *p) {
    (void)p;
    CompatFibra *fb = calloc(1, sizeof *fb);
    compat_fibra_actual = fb;
    return fb;
}
static inline void *GetCurrentFiber(void) { return compat_fibra_actual; }
// pila de 'reserva' bytes; la memoria solo se gasta según se usa (MAP_NORESERVE)
static inline void *CreateFiberEx(size_t comprometida, size_t reserva, DWORD flags, LPFIBER_START_ROUTINE f, void *arg) {
    (void)comprometida; (void)flags;
    CompatFibra *fb = calloc(1, sizeof *fb);
    void *pila = mmap(NULL, reserva, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (pila == MAP_FAILED) { free(fb); return NULL; }
    getcontext(&fb->ctx);
    fb->ctx.uc_stack.ss_sp = pila;
    fb->ctx.uc_stack.ss_size = reserva;
    fb->ctx.uc_link = NULL;
    fb->f = f;
    fb->arg = arg;
    uintptr_t d = (uintptr_t)fb;
    makecontext(&fb->ctx, (void (*)(void))compat_fibra_entra, 2, (unsigned)(d >> 32), (unsigned)(d & 0xFFFFFFFFu));
    return fb;
}
static inline void SwitchToFiber(void *f) {
    CompatFibra *antes = compat_fibra_actual, *despues = f;
    if (antes == despues) return;
    compat_fibra_actual = despues;
    swapcontext(&antes->ctx, &despues->ctx);
}

// ---- texto ----
#define CP_UTF8 65001
// UTF-8 → wchar_t (en Linux, de 32 bits); devuelve cuántos caracteres escribe, el 0 final incluido
static inline int MultiByteToWideChar(unsigned cp, DWORD flags, const char *s, int n, wchar_t *w, int max) {
    (void)cp; (void)flags; (void)n;
    int k = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && k < max - 1) {
        uint32_t c = *p++;
        int mas = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= mas == 3 ? 0x07 : mas == 2 ? 0x0F : mas == 1 ? 0x1F : 0x7F;
        while (mas-- && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3F);
        w[k++] = (wchar_t)c;
    }
    if (max > 0) w[k++] = 0;
    return k;
}

static inline void ExitProcess(unsigned codigo) { fflush(stderr); _exit((int)codigo); }
#define _popen popen
#define _pclose pclose
static inline int _putenv_s(const char *var, const char *valor) { return setenv(var, valor, 1); }

static inline BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proceso, DWORD_PTR *sistema) {
    (void)p;
    cpu_set_t s;
    DWORD_PTR m = 0;
    if (!sched_getaffinity(0, sizeof s, &s))
        for (int i = 0; i < 64 && i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &s)) m |= (DWORD_PTR)1 << i;
    if (!m) m = 1;
    *proceso = *sistema = m;
    return 1;
}
static inline HANDLE GetCurrentProcess(void) { return NULL; }
#endif
