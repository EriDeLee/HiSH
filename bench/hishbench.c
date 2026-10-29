/*
 * hishbench.c — portable CPU / memory / GPU benchmark for HiSH (HarmonyOS)
 * and Linux containers (Termux, PRoot, Alpine, Ubuntu...).
 *
 * One source file, no build-system dependency, no external data files.
 * CPU and memory use only libc + pthreads. GPU uses EGL/GLES2 loaded at
 * runtime with dlopen(), so the same binary links everywhere and the GPU
 * section reports SKIP with a reason instead of failing to start.
 *
 * Build (run this inside each environment you want to compare):
 *   cc -O2 -o hishbench hishbench.c -lpthread -ldl
 *   ./hishbench --label native
 *   ./hishbench --label container --json > container.json
 *
 * Fixed workloads: every sub-test performs the same number of operations on
 * both sides, so ops/s ratios are directly comparable. Each sub-test runs
 * 3 times; best and median are printed. --quick uses 1 run and half sizes.
 *
 * Licence: MIT (same as the project that ships it).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/utsname.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define HAVE_NEON 1
#else
#define HAVE_NEON 0
#endif

#define BENCH_VERSION "1.0.0"
#define MAX_RESULTS 64
#define MAX_THREADS 8

static int g_runs = 3;
static int g_json = 0;
static int g_no_gpu = 0;
static int g_quick = 0;
static const char *g_label = "unlabeled";

/* ------------------------------------------------------------------ */
/* timing                                                             */
/* ------------------------------------------------------------------ */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/* results                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *unit;
    double value;      /* ops/s, GB/s, or ns depending on unit */
    double best_ms;
    double median_ms;
    double runs[8];
    int n_runs;
    int status;        /* 0 ok, 1 skip, 2 fail */
    const char *note;
} Result;

static Result g_res[MAX_RESULTS];
static int g_nres = 0;

static Result *add_result(const char *name, const char *unit, int status,
                          const char *note) {
    Result *r = &g_res[g_nres++];
    memset(r, 0, sizeof(*r));
    r->name = name;
    r->unit = unit;
    r->status = status;
    r->note = note;
    return r;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* seconds -> best / median, sorted copy for median */
static void finish_times(Result *r, double *times, int n) {
    double sorted[8];
    memcpy(sorted, times, sizeof(double) * (size_t)n);
    qsort(sorted, (size_t)n, sizeof(double), cmp_double);
    r->n_runs = n;
    for (int i = 0; i < n; i++) r->runs[i] = times[i];
    r->best_ms = sorted[0] * 1e3;
    r->median_ms = sorted[n / 2] * 1e3;
}

/* ------------------------------------------------------------------ */
/* public workload knobs                                              */
/* ------------------------------------------------------------------ */

static uint64_t SZ_INT_MIX = 150000000ull;
static uint64_t SZ_INT_SCAN = 64ull * 1024 * 1024;      /* elements (u32) */
static uint64_t SZ_DOUBLE = 60000000ull;
static uint64_t SZ_FLOAT = 64000000ull;                 /* float ops, x4 */
static uint64_t SZ_BRANCH = 100000000ull;
static size_t MEM_BUF = 256u * 1024 * 1024;
static size_t MEM_COPY = 128u * 1024 * 1024;
static uint64_t LAT_ENTRIES = 8ull * 1024 * 1024;       /* 32 MiB of u32 */
static uint64_t LAT_ACCESS = 20000000ull;
static size_t FAULT_BYTES = 256ull * 1024 * 1024;
static uint64_t MALLOC_ITERS = 1000000ull;
static int GPU_SIZE = 1024;
static int GPU_ITER = 256;
static int GPU_PASSES = 24;
static int GPU_TRIS = 1000000;
static double g_scale = 1.0;

/* ------------------------------------------------------------------ */
/* CPU                                                                */
/* ------------------------------------------------------------------ */

static volatile uint64_t g_sink;

static double run_int_mix(uint64_t n) {
    uint64_t x = 0x9e3779b97f4a7c15ull, sum = 0;
    double t0 = now_sec();
    for (uint64_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        sum += x * 0x100000001b3ull;
    }
    double dt = now_sec() - t0;
    g_sink = sum ^ x;
    return dt;
}

static double run_int_scan(const uint32_t *a, uint64_t n) {
    uint64_t sum = 0;
    double t0 = now_sec();
    for (uint64_t i = 0; i < n; i++) sum += a[i] * 3u + (a[i] >> 3);
    double dt = now_sec() - t0;
    g_sink = sum;
    return dt;
}

static double run_double_chain(uint64_t n) {
    double a = 1.0, b = 0.5, c = 0.25;
    double t0 = now_sec();
    for (uint64_t i = 0; i < n; i++) {
        a = a * 1.0000001 + 0.0000001;
        b = b * 0.9999999 - 0.0000001;
        c = a + b * 0.5;
    }
    double dt = now_sec() - t0;
    g_sink = (uint64_t)(a + b + c);
    return dt;
}

static double run_float_simd(uint64_t n) {
    /* n = number of 4-wide iterations */
#if HAVE_NEON
    float32x4_t v = vdupq_n_f32(1.0f);
    float32x4_t k1 = vdupq_n_f32(1.0000001f);
    float32x4_t k2 = vdupq_n_f32(0.0000001f);
    double t0 = now_sec();
    for (uint64_t i = 0; i < n; i++) {
        v = vmulq_f32(v, k1);
        v = vaddq_f32(v, k2);
    }
    double dt = now_sec() - t0;
    float out[4];
    vst1q_f32(out, v);
    g_sink = (uint64_t)(out[0] + out[1] + out[2] + out[3]);
    return dt;
#else
    float a = 1.0f, b = 1.1f, c = 1.2f, d = 1.3f;
    const float k1 = 1.0000001f, k2 = 0.0000001f;
    double t0 = now_sec();
    for (uint64_t i = 0; i < n; i++) {
        a = a * k1 + k2; b = b * k1 + k2;
        c = c * k1 + k2; d = d * k1 + k2;
    }
    double dt = now_sec() - t0;
    g_sink = (uint64_t)(a + b + c + d);
    return dt;
#endif
}

static double run_branch(uint64_t n) {
    uint32_t x = 0x12345678u;
    uint64_t hits = 0;
    double t0 = now_sec();
    for (uint64_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        if (x & 0x10000u) hits += x & 7u;
        else hits -= x & 3u;
    }
    double dt = now_sec() - t0;
    g_sink = hits;
    return dt;
}

typedef struct {
    uint64_t n;
    uint64_t seed;
    uint64_t sum;
} ThreadArg;

static void *thread_int_mix(void *p) {
    ThreadArg *a = (ThreadArg *)p;
    uint64_t x = a->seed, sum = 0;
    for (uint64_t i = 0; i < a->n; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        sum += x * 0x100000001b3ull;
    }
    a->sum = sum ^ x;
    return NULL;
}

static void bench_scalar(const char *name, double (*fn)(uint64_t), uint64_t n,
                         const char *unit_ops) {
    Result *r = add_result(name, unit_ops, 0, NULL);
    double times[8];
    for (int i = 0; i < g_runs; i++) times[i] = fn(n);
    finish_times(r, times, g_runs);
    r->value = (double)n / r->median_ms * 1e3 / 1e6; /* Mops/s */
    (void)r;
}

/* ------------------------------------------------------------------ */
/* memory                                                             */
/* ------------------------------------------------------------------ */

static double mem_read_bw(const unsigned char *buf, size_t bytes) {
    uint64_t sum = 0;
    double t0 = now_sec();
    for (size_t i = 0; i < bytes; i += 8) {
        uint64_t v;
        memcpy(&v, buf + i, 8);
        sum += v;
    }
    double dt = now_sec() - t0;
    g_sink = sum;
    return dt;
}

static double mem_write_bw(unsigned char *buf, size_t bytes) {
    uint64_t v = 0x0102030405060708ull;
    double t0 = now_sec();
    for (size_t i = 0; i < bytes; i += 8) memcpy(buf + i, &v, 8);
    double dt = now_sec() - t0;
    g_sink = v;
    return dt;
}

static double mem_copy_bw(unsigned char *dst, const unsigned char *src,
                          size_t bytes) {
    double t0 = now_sec();
    for (size_t i = 0; i < bytes; i += 8) {
        uint64_t v;
        memcpy(&v, src + i, 8);
        memcpy(dst + i, &v, 8);
    }
    double dt = now_sec() - t0;
    g_sink = dst[0] + src[0];
    return dt;
}

static void build_perm(uint32_t *perm, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) perm[i] = (uint32_t)i;
    uint64_t s = 0x2545f4914f6cdd1dull;
    for (uint64_t i = n - 1; i > 0; i--) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        uint64_t j = s % (i + 1);
        uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
}

static double mem_latency(const uint32_t *perm, uint64_t n, uint64_t accesses) {
    uint32_t idx = 0;
    double t0 = now_sec();
    for (uint64_t i = 0; i < accesses; i++) idx = perm[idx];
    (void)n;
    double dt = now_sec() - t0;
    g_sink = idx;
    return dt;
}

static double mem_pagefault(size_t bytes) {
    double t0 = now_sec();
    unsigned char *p = (unsigned char *)mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    for (size_t i = 0; i < bytes; i += (size_t)ps) p[i] = 1;
    g_sink = p[0];
    munmap(p, bytes);
    return now_sec() - t0;
}

static double mem_malloc_churn(uint64_t iters, size_t block) {
    double t0 = now_sec();
    for (uint64_t i = 0; i < iters; i++) {
        void *p = malloc(block);
        if (!p) break;
        ((char *)p)[0] = (char)i;
        g_sink += (uint64_t)((char *)p)[0];
        free(p);
    }
    return now_sec() - t0;
}

/* ------------------------------------------------------------------ */
/* GPU: EGL + GLES2 via dlopen                                        */
/* ------------------------------------------------------------------ */

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef int32_t EGLint;
typedef uint32_t EGLBoolean;
typedef uint32_t EGLenum;

#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_LINEAR 0x2601
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_2D 0x0DE1

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;

typedef struct {
    void *libegl, *libgles;
    EGLDisplay (*eglGetDisplay)(void *);
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*eglBindAPI)(EGLenum);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLint (*eglGetError)(void);
    const char *(*eglQueryString)(EGLDisplay, EGLint);
    EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*eglTerminate)(EGLDisplay);
    void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
    void (*glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*glClear)(GLenum);
    GLuint (*glCreateShader)(GLenum);
    void (*glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
    void (*glCompileShader)(GLuint);
    void (*glGetShaderiv)(GLuint, GLenum, GLint *);
    void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
    GLuint (*glCreateProgram)(void);
    void (*glAttachShader)(GLuint, GLuint);
    void (*glLinkProgram)(GLuint);
    void (*glGetProgramiv)(GLuint, GLenum, GLint *);
    void (*glUseProgram)(GLuint);
    void (*glUniform2f)(GLint, GLfloat, GLfloat);
    void (*glUniform1f)(GLint, GLfloat);
    GLint (*glGetUniformLocation)(GLuint, const char *);
    void (*glGenFramebuffers)(GLsizei, GLuint *);
    void (*glBindFramebuffer)(GLenum, GLuint);
    void (*glGenRenderbuffers)(GLsizei, GLuint *);
    void (*glBindRenderbuffer)(GLenum, GLuint);
    void (*glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    void (*glGenTextures)(GLsizei, GLuint *);
    void (*glBindTexture)(GLenum, GLuint);
    void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*glTexParameteri)(GLenum, GLenum, GLint);
    void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*glDeleteTextures)(GLsizei, const GLuint *);
    GLenum (*glCheckFramebufferStatus)(GLenum);
    void (*glGenBuffers)(GLsizei, GLuint *);
    void (*glBindBuffer)(GLenum, GLuint);
    void (*glBufferData)(GLenum, long, const void *, GLenum);
    void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*glEnableVertexAttribArray)(GLuint);
    GLint (*glGetAttribLocation)(GLuint, const char *);
    void (*glDrawArrays)(GLenum, GLint, GLsizei);
    void (*glFinish)(void);
    const unsigned char *(*glGetString)(GLenum);
    GLenum (*glGetError)(void);
    void (*glDeleteShader)(GLuint);
    void (*glDeleteProgram)(GLuint);
    void (*glDeleteBuffers)(GLsizei, const GLuint *);
} Gles;

static void append_err(char *why, size_t whylen, const char *fmt, const char *a, const char *b) {
    size_t used = strlen(why);
    if (used + 8 >= whylen) return;
    snprintf(why + used, whylen - used, fmt, a ? a : "", b ? b : "");
}

static void *open_first(const char *const *names, char *why, size_t whylen) {
    for (int i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
        const char *e = dlerror();
        append_err(why, whylen, "%s: %s; ", names[i], e ? e : "unknown");
    }
    return NULL;
}

static int gles_load(Gles *g, char *why, size_t whylen) {
    static const char *const egl_names[] = {
        "/system/lib64/libEGL.so", "/system/lib64/chipset-pub-sdk/libEGL.so",
        "/vendor/lib64/libEGL.so", "libEGL.so.1", "libEGL.so",
        "/system/lib/libEGL.so", NULL
    };
    static const char *const gles_names[] = {
        "/system/lib64/libGLESv3.so", "/system/lib64/libGLESv2.so",
        "/system/lib64/chipset-pub-sdk/libGLESv3.so",
        "/vendor/lib64/libGLESv3.so", "/vendor/lib64/libGLESv2.so",
        "libGLESv3.so", "libGLESv2.so", "/system/lib/libGLESv2.so", NULL
    };
    memset(g, 0, sizeof(*g));
    g->libegl = open_first(egl_names, why, whylen);
    if (!g->libegl) return -1;
    g->libgles = open_first(gles_names, why, whylen);
    if (!g->libgles) return -1;
#define SYM(lib, name) do { g->name = (__typeof__(g->name))dlsym(g->lib, #name); if (!g->name) { snprintf(why, whylen, "missing " #name); return -1; } } while (0)
    SYM(libegl, eglGetDisplay);
    SYM(libegl, eglInitialize);
    SYM(libegl, eglChooseConfig);
    SYM(libegl, eglCreatePbufferSurface);
    SYM(libegl, eglCreateContext);
    SYM(libegl, eglMakeCurrent);
    SYM(libegl, eglBindAPI);
    SYM(libegl, eglGetError);
    SYM(libegl, eglQueryString);
    SYM(libegl, eglDestroySurface);
    SYM(libegl, eglDestroyContext);
    SYM(libegl, eglTerminate);
    SYM(libgles, glViewport);
    SYM(libgles, glClearColor);
    SYM(libgles, glClear);
    SYM(libgles, glCreateShader);
    SYM(libgles, glShaderSource);
    SYM(libgles, glCompileShader);
    SYM(libgles, glGetShaderiv);
    SYM(libgles, glCreateProgram);
    SYM(libgles, glAttachShader);
    SYM(libgles, glLinkProgram);
    SYM(libgles, glGetProgramiv);
    SYM(libgles, glUseProgram);
    SYM(libgles, glGetUniformLocation);
    SYM(libgles, glUniform2f);
    SYM(libgles, glUniform1f);
    SYM(libgles, glGenFramebuffers);
    SYM(libgles, glBindFramebuffer);
    SYM(libgles, glGenRenderbuffers);
    SYM(libgles, glBindRenderbuffer);
    SYM(libgles, glRenderbufferStorage);
    SYM(libgles, glFramebufferRenderbuffer);
    SYM(libgles, glGenTextures);
    SYM(libgles, glBindTexture);
    SYM(libgles, glTexImage2D);
    SYM(libgles, glTexParameteri);
    SYM(libgles, glFramebufferTexture2D);
    SYM(libgles, glDeleteTextures);
    SYM(libgles, glCheckFramebufferStatus);
    SYM(libgles, glGenBuffers);
    SYM(libgles, glBindBuffer);
    SYM(libgles, glBufferData);
    SYM(libgles, glVertexAttribPointer);
    SYM(libgles, glEnableVertexAttribArray);
    SYM(libgles, glGetAttribLocation);
    SYM(libgles, glDrawArrays);
    SYM(libgles, glFinish);
    SYM(libgles, glGetString);
    SYM(libgles, glGetError);
    SYM(libgles, glGetShaderInfoLog);
    SYM(libgles, glDeleteShader);
    SYM(libgles, glDeleteProgram);
    SYM(libgles, glDeleteBuffers);
#undef SYM
    return 0;
}

static char g_shader_log[512];

static GLuint compile_shader(Gles *g, GLenum type, const char *src) {
    GLuint s = g->glCreateShader(type);
    g->glShaderSource(s, 1, &src, NULL);
    g->glCompileShader(s);
    GLint ok = 0;
    g->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        g->glGetShaderInfoLog(s, sizeof(g_shader_log), NULL, g_shader_log);
        return 0;
    }
    return s;
}

static const char *VS_QUAD =
    "attribute vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *VS_QUAD3 =
    "#version 300 es\n"
    "in vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static void gpu_bench(char *skip_reason, size_t skip_len) {
    Gles g;
    char why[512] = {0};
    if (gles_load(&g, why, sizeof(why)) != 0) {
        snprintf(skip_reason, skip_len, "EGL/GLES unavailable (%s)", why);
        return;
    }
    EGLDisplay dpy = g.eglGetDisplay(NULL);
    if (!dpy || !g.eglInitialize(dpy, NULL, NULL)) {
        snprintf(skip_reason, skip_len, "eglInitialize failed (0x%x)", g.eglGetError());
        return;
    }
    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!g.eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        snprintf(skip_reason, skip_len, "no ES2 config (0x%x)", g.eglGetError());
        return;
    }
    EGLint pb_attr[] = { EGL_WIDTH, GPU_SIZE, EGL_HEIGHT, GPU_SIZE, EGL_NONE };
    EGLSurface surf = g.eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (!surf) {
        snprintf(skip_reason, skip_len, "pbuffer surface failed (0x%x)", g.eglGetError());
        return;
    }
    g.eglBindAPI(0x30A0 /* EGL_OPENGL_ES_API */);
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = g.eglCreateContext(dpy, cfg, NULL, ctx_attr);
    EGLBoolean made = ctx ? g.eglMakeCurrent(dpy, surf, surf, ctx) : 0;
    if (!ctx || !made) {
        snprintf(skip_reason, skip_len, "context failed (egl=0x%x)", g.eglGetError());
        return;
    }

    const char *renderer = (const char *)g.glGetString(GL_RENDERER);
    const char *vendor = (const char *)g.glGetString(GL_VENDOR);
    const char *version = (const char *)g.glGetString(GL_VERSION);
    if (!version) {
        snprintf(skip_reason, skip_len, "no GL context (gl=0x%x egl=0x%x vendor=%s)",
                 g.glGetError(), g.eglGetError(),
                 g.eglQueryString(dpy, EGL_VENDOR) ? g.eglQueryString(dpy, EGL_VENDOR) : "?");
        return;
    }

    /* draw straight into the pbuffer back buffer: no FBO, widest driver support */
    GLuint fbo = 0, tex = 0;
    (void)fbo; (void)tex;

    int es3 = version && strncmp(version, "OpenGL ES 3", 11) == 0;
    char *fs = (char *)malloc(4096);
    if (es3) {
        snprintf(fs, 4096,
            "#version 300 es\n"
            "precision highp float;\n"
            "uniform vec2 uRes;\n"
            "uniform float uSeed;\n"
            "out vec4 fragColor;\n"
            "void main() {\n"
            "  vec2 p = (gl_FragCoord.xy / uRes) * 2.0 - 1.0;\n"
            "  float a = uSeed + p.x, b = p.y, c = a * 0.5;\n"
            "  for (int i = 0; i < %d; i++) {\n"
            "    a = a * 1.000001 + b * 0.5;\n"
            "    b = b * 0.999999 - c * 0.25;\n"
            "    c = c * 1.0000003 + a;\n"
            "  }\n"
            "  fragColor = vec4(fract(a), fract(b), fract(c), 1.0);\n"
            "}\n", GPU_ITER);
    } else {
        snprintf(fs, 4096,
            "precision highp float;\n"
            "uniform vec2 uRes;\n"
            "uniform float uSeed;\n"
            "void main() {\n"
            "  vec2 p = (gl_FragCoord.xy / uRes) * 2.0 - 1.0;\n"
            "  float a = uSeed + p.x, b = p.y, c = a * 0.5;\n"
            "  for (int i = 0; i < %d; i++) {\n"
            "    a = a * 1.000001 + b * 0.5;\n"
            "    b = b * 0.999999 - c * 0.25;\n"
            "    c = c * 1.0000003 + a;\n"
            "  }\n"
            "  gl_FragColor = vec4(fract(a), fract(b), fract(c), 1.0);\n"
            "}\n", GPU_ITER);
    }
    GLuint vsh = compile_shader(&g, GL_VERTEX_SHADER, es3 ? VS_QUAD3 : VS_QUAD);
    GLuint fsh = vsh ? compile_shader(&g, GL_FRAGMENT_SHADER, fs) : 0;
    free(fs);
    if (!vsh || !fsh) {
        snprintf(skip_reason, skip_len, "shader build failed: %s | %s",
                 g_shader_log, version ? version : "?");
        return;
    }
    GLuint prog = g.glCreateProgram();
    g.glAttachShader(prog, vsh);
    g.glAttachShader(prog, fsh);
    g.glLinkProgram(prog);
    GLint ok = 0;
    g.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        snprintf(skip_reason, skip_len, "program link failed");
        return;
    }
    g.glUseProgram(prog);
    GLint uRes = g.glGetUniformLocation(prog, "uRes");
    GLint uSeed = g.glGetUniformLocation(prog, "uSeed");
    g.glUniform2f(uRes, (GLfloat)GPU_SIZE, (GLfloat)GPU_SIZE);
    g.glUniform1f(uSeed, 0.123f);

    static const GLfloat quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vbo = 0;
    g.glGenBuffers(1, &vbo);
    g.glBindBuffer(GL_ARRAY_BUFFER, vbo);
    g.glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    GLint aPos = g.glGetAttribLocation(prog, "aPos");
    g.glEnableVertexAttribArray((GLuint)aPos);
    g.glVertexAttribPointer((GLuint)aPos, 2, GL_FLOAT, 0, 0, NULL);
    g.glViewport(0, 0, GPU_SIZE, GPU_SIZE);

    double times[8];
    int n = g_runs > 3 ? 3 : g_runs;
    for (int r = 0; r < n; r++) {
        g.glFinish();
        double t0 = now_sec();
        for (int p = 0; p < GPU_PASSES; p++) {
            g.glClear(GL_COLOR_BUFFER_BIT);
            g.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        g.glFinish();
        times[r] = now_sec() - t0;
    }

    Result *rr = add_result("gpu-fill", "Gpix/s", 0, NULL);
    finish_times(rr, times, n);
    rr->value = (double)GPU_SIZE * GPU_SIZE * GPU_PASSES / rr->median_ms / 1e6;

    /* vertex throughput: 1M triangles, flat color fragment shader */
    const char *vs_tri = es3 ? VS_QUAD3 : VS_QUAD;
    const char *fs_flat = es3
        ? "#version 300 es\nprecision lowp float;\nout vec4 fragColor;\n"
          "void main() { fragColor = vec4(1.0, 0.5, 0.25, 1.0); }\n"
        : "precision lowp float;\n"
          "void main() { gl_FragColor = vec4(1.0, 0.5, 0.25, 1.0); }\n";
    GLuint v2 = compile_shader(&g, GL_VERTEX_SHADER, vs_tri);
    GLuint f2 = compile_shader(&g, GL_FRAGMENT_SHADER, fs_flat);
    if (v2 && f2) {
        GLuint p2 = g.glCreateProgram();
        g.glAttachShader(p2, v2);
        g.glAttachShader(p2, f2);
        g.glLinkProgram(p2);
        g.glGetProgramiv(p2, GL_LINK_STATUS, &ok);
        if (ok) {
            g.glUseProgram(p2);
            g.glViewport(0, 0, GPU_SIZE, GPU_SIZE);
            GLuint tvbo = 0;
            g.glGenBuffers(1, &tvbo);
            g.glBindBuffer(GL_ARRAY_BUFFER, tvbo);
            size_t vbytes = (size_t)GPU_TRIS * 3 * 2 * sizeof(GLfloat);
            GLfloat *tris = (GLfloat *)malloc(vbytes);
            for (int i = 0; i < GPU_TRIS * 3; i++) {
                float x = (float)(i % 97) / 97.0f - 0.5f;
                float y = (float)(i % 89) / 89.0f - 0.5f;
                tris[i * 2 + 0] = x * 0.1f;
                tris[i * 2 + 1] = y * 0.1f;
            }
            g.glBufferData(GL_ARRAY_BUFFER, (long)vbytes, tris, GL_STATIC_DRAW);
            GLint a2 = g.glGetAttribLocation(p2, "aPos");
            g.glEnableVertexAttribArray((GLuint)a2);
            g.glVertexAttribPointer((GLuint)a2, 2, GL_FLOAT, 0, 0, NULL);
            for (int r = 0; r < n; r++) {
                g.glFinish();
                double t0 = now_sec();
                for (int p = 0; p < 8; p++) g.glDrawArrays(GL_TRIANGLES, 0, GPU_TRIS * 3);
                g.glFinish();
                times[r] = now_sec() - t0;
            }
            Result *rt = add_result("gpu-vertex", "Mtris/s", 0, NULL);
            finish_times(rt, times, n);
            rt->value = (double)GPU_TRIS * 8 / rt->median_ms / 1e3;            free(tris);
            g.glDeleteBuffers(1, &tvbo);
            g.glDeleteProgram(p2);
        }
    }

    /* GPU identity is emitted as a note on the first GPU result */
    static char gpu_note[512];
    snprintf(gpu_note, sizeof(gpu_note), "%s | %s | %s",
             renderer ? renderer : "?", vendor ? vendor : "?", version ? version : "?");
    if (g_nres > 0) {
        for (int i = 0; i < g_nres; i++) {
            if (strcmp(g_res[i].name, "gpu-fill") == 0) g_res[i].note = gpu_note;
        }
    }

    g.glDeleteBuffers(1, &vbo);
    g.glDeleteProgram(prog);
    g.eglDestroySurface(dpy, surf);
    g.eglDestroyContext(dpy, ctx);
    g.eglTerminate(dpy);
}

/* ------------------------------------------------------------------ */
/* host info                                                          */
/* ------------------------------------------------------------------ */

static void read_cpu_model(char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) { snprintf(out, n, "unknown"); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Hardware", 8) == 0 || strncmp(line, "model name", 10) == 0 ||
            strncmp(line, "Processor", 9) == 0 || strncmp(line, "CPU part", 8) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                char *v = colon + 1;
                while (*v == ' ' || *v == '\t') v++;
                char *e = v + strlen(v);
                while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
                snprintf(out, n, "%s", v);
                fclose(f);
                return;
            }
        }
    }
    fclose(f);
    if (!out[0]) snprintf(out, n, "unknown");
}

static void json_str(FILE *out, const char *s) {
    fputc('"', out);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\r') fputs("\\r", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c < 0x20) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
    fputc('"', out);
}

static void print_host(FILE *out, int json) {
    struct utsname u;
    char model[256];
    read_cpu_model(model, sizeof(model));
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    long ps = sysconf(_SC_PAGESIZE);
    uname(&u);
    if (json) {
        fputs("  \"host\": {\"label\": ", out);
        json_str(out, g_label);
        fputs(", \"sysname\": ", out);
        json_str(out, u.sysname);
        fputs(", \"release\": ", out);
        json_str(out, u.release);
        fputs(", \"machine\": ", out);
        json_str(out, u.machine);
        fprintf(out, ", \"cores\": %ld, \"pagesize\": %ld, \"cpu\": ", cores, ps);
        json_str(out, model);
        fputs(", \"compiler\": ", out);
        json_str(out, __VERSION__);
        fprintf(out, ", \"version\": \"%s\"},\n", BENCH_VERSION);
    } else {
        printf("HiSH Bench %s  label=%s\n", BENCH_VERSION, g_label);
        printf("  kernel   %s %s %s\n", u.sysname, u.release, u.machine);
        printf("  cpu      %s  cores=%ld pagesize=%ld\n", model, cores, ps);
        printf("  compiler %s\n", __VERSION__);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                              */
/* ------------------------------------------------------------------ */

static void usage(void) {
    printf("usage: hishbench [--label NAME] [--json] [--quick] [--no-gpu] [--runs N] [--scale N]\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) g_json = 1;
        else if (!strcmp(argv[i], "--no-gpu")) g_no_gpu = 1;
        else if (!strcmp(argv[i], "--quick")) g_quick = 1;
        else if (!strcmp(argv[i], "--label") && i + 1 < argc) g_label = argv[++i];
        else if (!strcmp(argv[i], "--runs") && i + 1 < argc) g_runs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) g_scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { usage(); return 2; }
    }
    if (g_scale < 0.1) g_scale = 0.1;
    if (g_scale > 100.0) g_scale = 100.0;
    if (g_scale != 1.0) {
        SZ_INT_MIX = (uint64_t)(SZ_INT_MIX * g_scale);
        SZ_INT_SCAN = (uint64_t)(SZ_INT_SCAN * g_scale);
        SZ_DOUBLE = (uint64_t)(SZ_DOUBLE * g_scale);
        SZ_FLOAT = (uint64_t)(SZ_FLOAT * g_scale);
        SZ_BRANCH = (uint64_t)(SZ_BRANCH * g_scale);
        LAT_ACCESS = (uint64_t)(LAT_ACCESS * g_scale);
        MALLOC_ITERS = (uint64_t)(MALLOC_ITERS * g_scale);
        GPU_PASSES = (int)(GPU_PASSES * g_scale);
        GPU_TRIS = (int)(GPU_TRIS * g_scale);
        if (MEM_BUF * g_scale <= (size_t)2 * 1024 * 1024 * 1024)
            MEM_BUF = (size_t)(MEM_BUF * g_scale);
        if (MEM_COPY * g_scale <= (size_t)1024 * 1024 * 1024)
            MEM_COPY = (size_t)(MEM_COPY * g_scale);
        if (FAULT_BYTES * g_scale <= (size_t)2 * 1024 * 1024 * 1024)
            FAULT_BYTES = (size_t)(FAULT_BYTES * g_scale);
    }
    if (g_runs < 1) g_runs = 1;
    if (g_runs > 8) g_runs = 8;
    if (g_quick) {
        g_runs = 1;
        SZ_INT_MIX /= 4; SZ_INT_SCAN /= 4; SZ_DOUBLE /= 4; SZ_FLOAT /= 4;
        SZ_BRANCH /= 4; MEM_BUF /= 4; MEM_COPY /= 4; LAT_ACCESS /= 4;
        FAULT_BYTES /= 4; MALLOC_ITERS /= 4; GPU_PASSES = 6;
    }

    int json = g_json;
    if (!json) print_host(stdout, 0);
    if (json) {
        printf("{\n");
        print_host(stdout, 1);
        printf("  \"results\": [\n");
    }

    /* CPU */
    bench_scalar("int64-mix", run_int_mix, SZ_INT_MIX, "Mops/s");
    fprintf(stderr, "[phase] int64 done\n");

    /* int scan over a buffer */
    {
        uint32_t *arr = (uint32_t *)malloc((size_t)SZ_INT_SCAN * 4);
        if (arr) {
            for (uint64_t i = 0; i < SZ_INT_SCAN; i++) arr[i] = (uint32_t)(i * 2654435761u);
            Result *r = add_result("int32-scan", "Mops/s", 0, NULL);
            double times[8];
            for (int i = 0; i < g_runs; i++) times[i] = run_int_scan(arr, SZ_INT_SCAN);
            finish_times(r, times, g_runs);
            r->value = (double)SZ_INT_SCAN / r->median_ms * 1e3 / 1e6;
            free(arr);
        } else {
            add_result("int32-scan", "Mops/s", 1, "allocation failed");
        }
    }

    bench_scalar("double-chain", run_double_chain, SZ_DOUBLE, "Mops/s");
    bench_scalar("float32-wide", run_float_simd, SZ_FLOAT, "Mops/s");
    bench_scalar("branch-mix", run_branch, SZ_BRANCH, "Mops/s");
    fprintf(stderr, "[phase] scalar done\n");

    /* thread scaling: same total work as int64-mix */
    {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        int nt = (int)(cores > MAX_THREADS ? MAX_THREADS : cores);
        if (nt < 1) nt = 1;
        Result *r = add_result("int64-mt", "Mops/s", 0, NULL);
        double times[8];
        for (int rep = 0; rep < g_runs; rep++) {
            pthread_t th[MAX_THREADS];
            ThreadArg args[MAX_THREADS];
            uint64_t per = SZ_INT_MIX / (uint64_t)nt;
            double t0 = now_sec();
            for (int t = 0; t < nt; t++) {
                args[t].n = per;
                args[t].seed = 0x9e3779b97f4a7c15ull + (uint64_t)t * 0x100000001b3ull;
                args[t].sum = 0;
                pthread_create(&th[t], NULL, thread_int_mix, &args[t]);
            }
            uint64_t total = 0;
            for (int t = 0; t < nt; t++) {
                pthread_join(th[t], NULL);
                total += args[t].sum;
            }
            times[rep] = now_sec() - t0;
            g_sink = total;
        }
        finish_times(r, times, g_runs);
        r->value = (double)SZ_INT_MIX / r->median_ms * 1e3 / 1e6;
        static char note[64];
        snprintf(note, sizeof(note), "threads=%d", nt);
        r->note = note;
    }

    /* memory */
    fprintf(stderr, "[phase] mt done\n");
    unsigned char *mb = (unsigned char *)mmap(NULL, MEM_BUF, PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mb != MAP_FAILED) {
        memset(mb, 0x5a, MEM_BUF);
        Result *r = add_result("mem-read", "GB/s", 0, NULL);
        double times[8];
        for (int i = 0; i < g_runs; i++) times[i] = mem_read_bw(mb, MEM_BUF);
        finish_times(r, times, g_runs);
        r->value = (double)MEM_BUF / (r->median_ms / 1e3) / 1e9;

        r = add_result("mem-write", "GB/s", 0, NULL);
        for (int i = 0; i < g_runs; i++) times[i] = mem_write_bw(mb, MEM_BUF);
        finish_times(r, times, g_runs);
        r->value = (double)MEM_BUF / (r->median_ms / 1e3) / 1e9;

        unsigned char *dst = (unsigned char *)mmap(NULL, MEM_COPY, PROT_READ | PROT_WRITE,
                                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (dst != MAP_FAILED) {
            r = add_result("mem-copy", "GB/s", 0, "read+write");
            for (int i = 0; i < g_runs; i++) times[i] = mem_copy_bw(dst, mb, MEM_COPY);
            finish_times(r, times, g_runs);
            r->value = (double)MEM_COPY * 2 / (r->median_ms / 1e3) / 1e9;
            munmap(dst, MEM_COPY);
        }
        munmap(mb, MEM_BUF);
    } else {
        add_result("mem-read", "GB/s", 1, "mmap failed");
        add_result("mem-write", "GB/s", 1, "mmap failed");
        add_result("mem-copy", "GB/s", 1, "mmap failed");
    }

    {
        uint32_t *perm = (uint32_t *)malloc((size_t)LAT_ENTRIES * 4);
        if (perm) {
            build_perm(perm, LAT_ENTRIES);
            Result *r = add_result("mem-latency", "ns/access", 0, NULL);
            double times[8];
            for (int i = 0; i < g_runs; i++) times[i] = mem_latency(perm, LAT_ENTRIES, LAT_ACCESS);
            finish_times(r, times, g_runs);
            r->value = r->median_ms * 1e6 / (double)LAT_ACCESS;
            free(perm);
        } else {
            add_result("mem-latency", "ns/access", 1, "allocation failed");
        }
    }

    {
        Result *r = add_result("mem-fault", "MB/s", 0, NULL);
        double times[8];
        int good = 1;
        for (int i = 0; i < g_runs; i++) {
            times[i] = mem_pagefault(FAULT_BYTES);
            if (times[i] < 0) good = 0;
        }
        if (!good) {
            r->status = 1;
            r->note = "mmap failed";
        } else {
            finish_times(r, times, g_runs);
            r->value = (double)FAULT_BYTES / (r->median_ms / 1e3) / 1e6;
        }
    }

    {
        Result *r = add_result("malloc-small", "Mops/s", 0, "64 B");
        double times[8];
        for (int i = 0; i < g_runs; i++) times[i] = mem_malloc_churn(MALLOC_ITERS, 64);
        finish_times(r, times, g_runs);
        r->value = (double)MALLOC_ITERS / r->median_ms * 1e3 / 1e6;
    }

    /* GPU */
    fprintf(stderr, "[phase] mem done\n");
    if (!g_no_gpu) {
        char why[512] = {0};
        gpu_bench(why, sizeof(why));
        if (why[0]) {
            Result *r = add_result("gpu-fill", "Gpix/s", 1, why);
            (void)r;
        }
    } else {
        add_result("gpu-fill", "Gpix/s", 1, "disabled by --no-gpu");
    }

    /* optional sustained all-core load */
    fprintf(stderr, "[phase] gpu done\n");

    /* output */
    if (json) {
        for (int i = 0; i < g_nres; i++) {
            Result *r = &g_res[i];
            printf("    {\"name\": \"%s\", \"unit\": \"%s\", \"status\": \"%s\", "
                   "\"value\": %.4f, \"best_ms\": %.3f, \"median_ms\": %.3f, \"runs_ms\": [",
                   r->name, r->unit, r->status == 0 ? "ok" : (r->status == 1 ? "skip" : "fail"),
                   r->value, r->best_ms, r->median_ms);
            for (int k = 0; k < r->n_runs; k++)
                printf("%s%.3f", k ? ", " : "", r->runs[k] * 1e3);
            printf("], \"note\": ");
            json_str(stdout, r->note ? r->note : "");
            printf("}%s\n", i + 1 < g_nres ? "," : "");
        }
        printf("  ]\n}\n");
    } else {
        printf("\n%-14s %12s %10s %10s  %s\n", "test", "value", "best", "median", "note");
        printf("---------------------------------------------------------------\n");
        for (int i = 0; i < g_nres; i++) {
            Result *r = &g_res[i];
            if (r->status != 0) {
                printf("%-14s %12s %10s %10s  SKIP: %s\n", r->name, "-", "-", "-",
                       r->note ? r->note : "");
            } else {
                printf("%-14s %12.3f %9.1fms %9.1fms  %s\n", r->name, r->value,
                       r->best_ms, r->median_ms, r->note ? r->note : "");
            }
        }
        printf("\nunits: Mops/s=Mops per second, GB/s=bytes/s, ns/access=memory latency,\n");
        printf("       Gpix/s=pixels/s, Mtris/s=triangles/s. Same workload on every host.\n");
    }
    return 0;
}
