extern int printf(const char *, ...); extern void *dlopen(const char *, int); extern void *dlsym(void *, const char *); extern void exit(int) __attribute__((noreturn));
static void run(void) {
    void *h = dlopen("libX11.so.6", 2); if (!h) { printf("no libX11\n"); return; }
    void *(*XOpenDisplay)(const char *) = dlsym(h, "XOpenDisplay");
    int (*XDefaultScreen)(void *) = dlsym(h, "XDefaultScreen");
    int (*XDisplayWidth)(void *, int) = dlsym(h, "XDisplayWidth");
    int (*XDisplayHeight)(void *, int) = dlsym(h, "XDisplayHeight");
    void *d = XOpenDisplay(0);   /* $DISPLAY: the native server, :2, by default */
    if (!d) { printf("== x11: MAL (XOpenDisplay failed)\n"); return; }
    int s = XDefaultScreen(d);
    printf("== x11: ok, screen %dx%d\n", XDisplayWidth(d, s), XDisplayHeight(d, s));
}
void _start(void) { run(); exit(0); }
