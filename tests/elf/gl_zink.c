// OpenGL through Mesa's Zink on the guest's Vulkan (FEX's Vulkan thunk, then
// MoltenVK or KosmicKrisp): a surfaceless EGL context, the highest core
// program of 4.5, 3.3 or 3.2, drawing a triangle into a framebuffer object, and the pixels read back --
// the centre must be the triangle's colour and a corner the clear colour.
// Run with GALLIUM_DRIVER=zink and EGL_PLATFORM=surfaceless
// (tests/elf/run_vk_device.sh). Everything comes from libEGL.so.1 by
// eglGetProcAddress, so no GL headers or libraries are needed to build it.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef void *EGLDisplay, *EGLContext, *EGLSurface, *EGLConfig;
typedef int32_t EGLint;
typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef intptr_t GLsizeiptr;

#define EGL_PLATFORM_SURFACELESS_MESA       0x31DD
#define EGL_OPENGL_API                      0x30A2
#define EGL_CONTEXT_MAJOR_VERSION           0x3098
#define EGL_CONTEXT_MINOR_VERSION           0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK     0x30FD
#define EGL_NONE                            0x3038

#define GL_FRAMEBUFFER          0x8D40
#define GL_RENDERBUFFER         0x8D41
#define GL_RGBA8                0x8058
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT     0x4000
#define GL_VERTEX_SHADER        0x8B31
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_ARRAY_BUFFER         0x8892
#define GL_STATIC_DRAW          0x88E4
#define GL_FLOAT                0x1406
#define GL_TRIANGLES            0x0004
#define GL_RGBA                 0x1908
#define GL_UNSIGNED_BYTE        0x1401
#define GL_RENDERER             0x1F01
#define GL_VERSION              0x1F02

static void *(*getproc)(const char *);
#define F(ret, name, args) static ret (*name) args;
#define GLFUNCS \
    F(const char *, glGetString, (GLenum)) \
    F(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    F(void, glBindFramebuffer, (GLenum, GLuint)) \
    F(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    F(void, glBindRenderbuffer, (GLenum, GLuint)) \
    F(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    F(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    F(GLenum, glCheckFramebufferStatus, (GLenum)) \
    F(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    F(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    F(void, glClear, (GLbitfield)) \
    F(GLuint, glCreateShader, (GLenum)) \
    F(void, glShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)) \
    F(void, glCompileShader, (GLuint)) \
    F(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    F(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *)) \
    F(GLuint, glCreateProgram, (void)) \
    F(void, glAttachShader, (GLuint, GLuint)) \
    F(void, glLinkProgram, (GLuint)) \
    F(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    F(void, glUseProgram, (GLuint)) \
    F(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    F(void, glBindVertexArray, (GLuint)) \
    F(void, glGenBuffers, (GLsizei, GLuint *)) \
    F(void, glBindBuffer, (GLenum, GLuint)) \
    F(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    F(void, glVertexAttribPointer, (GLuint, GLint, GLenum, unsigned char, GLsizei, const void *)) \
    F(void, glEnableVertexAttribArray, (GLuint)) \
    F(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    F(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    F(void, glFinish, (void))
GLFUNCS
#undef F

static GLuint shader(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        printf("shader: %s\n", log);
    }
    return s;
}

int main(void)
{
    void *egl = dlopen("libEGL.so.1", RTLD_NOW);
    if (!egl) { printf("no libEGL.so.1: %s\n== gl_zink: FAIL\n", dlerror()); return 1; }
    getproc = (void *(*)(const char *))dlsym(egl, "eglGetProcAddress");
    EGLDisplay (*getPlatformDisplay)(GLenum, void *, const intptr_t *) = getproc("eglGetPlatformDisplay");
    unsigned (*initialize)(EGLDisplay, EGLint *, EGLint *) = getproc("eglInitialize");
    unsigned (*bindAPI)(GLenum) = getproc("eglBindAPI");
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = getproc("eglCreateContext");
    unsigned (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = getproc("eglMakeCurrent");

    EGLDisplay dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    EGLint maj = 0, min = 0;
    if (!dpy || !initialize(dpy, &maj, &min) || !bindAPI(EGL_OPENGL_API)) {
        printf("EGL init failed\n== gl_zink: FAIL\n");
        return 1;
    }
    // The highest core context the driver gives: 4.5 (Zink on MoltenVK with
    // the two extensions scripts/settings-env.py announces), 3.3, else 3.2
    // (Zink on MoltenVK as it is).
    static const struct { int major, minor; const char *glsl; } want[] = {
        { 4, 5, "#version 450 core\n" }, { 3, 3, "#version 330 core\n" }, { 3, 2, "#version 150 core\n" },
    };
    EGLContext ctx = NULL;
    unsigned k;
    for (k = 0; k < 3 && !ctx; k++) {
        const EGLint attrs[] = { EGL_CONTEXT_MAJOR_VERSION, want[k].major, EGL_CONTEXT_MINOR_VERSION, want[k].minor,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK, 1, EGL_NONE };
        ctx = createContext(dpy, NULL, NULL, attrs);              // EGL_KHR_no_config_context
    }
    k--;
    if (!ctx || !makeCurrent(dpy, NULL, NULL, ctx)) {            // EGL_KHR_surfaceless_context
        printf("no 3.2 core context\n== gl_zink: FAIL\n");
        return 1;
    }
    const char *glsl = want[k].glsl;
    char vs[256], fs[256];
    snprintf(vs, sizeof vs, "%sin vec2 p;\nvoid main(){gl_Position=vec4(p,0.0,1.0);}\n", glsl);
    snprintf(fs, sizeof fs, "%sout vec4 c;\nvoid main(){c=vec4(1.0,0.0,0.0,1.0);}\n", glsl);
#define F(ret, name, args) name = (ret (*) args)getproc(#name);
    GLFUNCS
#undef F
    printf("renderer: %s\nversion:  %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    enum { W = 64, H = 64 };
    GLuint fbo, rb;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("framebuffer incomplete\n== gl_zink: FAIL\n");
        return 1;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(prog);                 // one attribute: it gets location 0
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    glUseProgram(prog);
    static const GLfloat tri[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.6f };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, NULL);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, W, H);

    static uint8_t px[W * H * 4];
    struct timespec t0, t1;
    enum { FRAMES = 200 };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < FRAMES; f++) {
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const uint8_t *mid = px + ((H / 2) * W + W / 2) * 4, *corner = px;
    int ok = linked && mid[0] == 255 && mid[1] == 0 && mid[2] == 0 &&
             corner[0] == 0 && corner[1] == 0 && corner[2] == 255;
    double ms = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e6 / FRAMES;
    printf("readback: centre %u,%u,%u corner %u,%u,%u; %.2f ms per draw+readback\n",
           mid[0], mid[1], mid[2], corner[0], corner[1], corner[2], ms);
    printf("== gl_zink: %s\n", ok ? "ok" : "FAIL");
    return !ok;
}
