/* What a root's GLX client library finds on the X server: the probe of
 * tests/arm64/frame_glx.sh (benchmarks/stage23-frame-root.txt). Built against
 * the root itself (it has no GL/glx.h, hence the prototypes below).
 * Prints one "key: value" line per fact; exit 0 when an RGBA double-buffered
 * visual gets a current context, 1 otherwise. */
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef struct __GLXcontextRec *GLXContext;
extern const char *glXQueryServerString(Display *, int, int);
extern const char *glXGetClientString(Display *, int);
extern int glXGetConfig(Display *, XVisualInfo *, int, int *);
extern XVisualInfo *glXChooseVisual(Display *, int, int *);
extern GLXFBConfig *glXGetFBConfigs(Display *, int, int *);
extern GLXContext glXCreateContext(Display *, XVisualInfo *, GLXContext, int);
extern int glXIsDirect(Display *, GLXContext);
extern int glXMakeCurrent(Display *, unsigned long, GLXContext);
extern const unsigned char *glGetString(unsigned);

#define GLX_USE_GL 1
#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_RED_SIZE 8
#define GLX_VENDOR 1
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

static const char *s(const void *p) { return p ? (const char *)p : "(null)"; }

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { puts("display: none"); return 1; }
    printf("server vendor: %s\n", s(glXQueryServerString(d, 0, GLX_VENDOR)));
    printf("client vendor: %s\n", s(glXGetClientString(d, GLX_VENDOR)));
    XVisualInfo t = { .screen = 0 };
    int n = 0, gl = 0;
    XVisualInfo *v = XGetVisualInfo(d, VisualScreenMask, &t, &n);
    for (int i = 0; i < n; i++) {
        int use = 0;
        if (glXGetConfig(d, &v[i], GLX_USE_GL, &use) == 0 && use) gl++;
    }
    printf("visuals: %d X, %d GL\n", n, gl);
    int nfb = 0;
    glXGetFBConfigs(d, 0, &nfb);
    printf("fbconfigs: %d\n", nfb);
    int attr[] = { GLX_RGBA, GLX_RED_SIZE, 1, GLX_DOUBLEBUFFER, 0 };
    XVisualInfo *cv = glXChooseVisual(d, 0, attr);
    printf("glXChooseVisual: %s\n", cv ? "ok" : "failed");
    int ok = 0;
    if (cv) {
        GLXContext ctx = glXCreateContext(d, cv, NULL, 1);
        XSetWindowAttributes swa = { 0 };
        swa.colormap = XCreateColormap(d, RootWindow(d, 0), cv->visual, AllocNone);
        Window w = XCreateWindow(d, RootWindow(d, 0), 0, 0, 16, 16, 0, cv->depth, InputOutput,
                                 cv->visual, CWColormap, &swa);
        if (ctx && glXMakeCurrent(d, w, ctx)) {
            printf("context: %s\n", glXIsDirect(d, ctx) ? "direct" : "indirect");
            printf("GL_RENDERER: %s\n", s(glGetString(GL_RENDERER)));
            printf("GL_VERSION: %s\n", s(glGetString(GL_VERSION)));
            ok = glGetString(GL_RENDERER) != NULL;
            glXMakeCurrent(d, 0, NULL);
        } else {
            puts("context: failed");
        }
        XDestroyWindow(d, w);
    }
    XCloseDisplay(d);
    return ok ? 0 : 1;
}
