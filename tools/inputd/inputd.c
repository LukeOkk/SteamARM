// steamarm-inputd: the Mac's game controllers as Linux evdev devices for
// guest programs (tools/inputd/PROTOCOL.md). SDL2 reads the controllers
// (GameController / IOKit, background events allowed: the game has focus),
// each launcher player (controllers.json) becomes /tmp/lxrt-input/eventN with
// the identity of the controller type chosen in the launcher, and the
// runtime shows that to guests as /dev/input/eventN (runtime/evdev.c).
// Started by scripts/input.sh; --fake/--script drive it without hardware.
#define _DARWIN_C_SOURCE
#include <SDL.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/file.h>
#include <libproc.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <limits.h>

#define PLAYERS 4
#define CLIENTS 32
#define VALUES 48
/* Tiny bounded recursive JSON tokenizer. Children are traversed by end index. */
typedef struct {
    int start, len, end;
    char type;
} Token;
static Token tokens[8192];
static int nt, pos;
static const char *json;
static void space(void)
{
    while (json[pos] && strchr(" \r\n\t", json[pos]))
        pos++;
}
static int parse(int depth)
{
    space();
    if (depth > 32 || nt == 8192 || !json[pos])
        return -1;
    int n = nt++;
    Token *t = &tokens[n];
    t->start = pos;
    t->type = json[pos++];
    if (t->type == '{' || t->type == '[') {
        char close = t->type == '{' ? '}' : ']';
        space();
        if (json[pos] != close)
            for (;;) {
                if (t->type == '{') {
                    space();
                    if (json[pos] != '"' || parse(depth + 1) < 0)
                        return -1;
                    space();
                    if (json[pos++] != ':')
                        return -1;
                }
                if (parse(depth + 1) < 0)
                    return -1;
                space();
                if (json[pos] != ',')
                    break;
                pos++;
            }
        if (json[pos++] != close)
            return -1;
    } else if (t->type == '"') {
        while (json[pos] && json[pos] != '"') {
            if ((unsigned char)json[pos] < 32)
                return -1;
            if (json[pos] == '\\') {
                pos++;
                if (!json[pos])
                    return -1;
            }
            pos++;
        }
        if (json[pos++] != '"')
            return -1;
    } else {
        while (json[pos] && !strchr(" ,}\r\n\t]", json[pos]))
            pos++;
        int len = pos - t->start;
        if (!((len == 4 && !strncmp(json + t->start, "true", 4)) ||
              (len == 5 && !strncmp(json + t->start, "false", 5)) ||
              (len == 4 && !strncmp(json + t->start, "null", 4)))) {
            char *end;
            double v = strtod(json + t->start, &end);
            if (end != json + pos || !isfinite(v))
                return -1;
        }
    }
    t->len = pos - t->start;
    t->end = nt;
    return n;
}
static int eq(int t, const char *s)
{
    return t >= 0 && tokens[t].type == '"' && tokens[t].len == (int)strlen(s) + 2 &&
           !strncmp(json + tokens[t].start + 1, s, strlen(s));
}
static int field(int t, const char *s)
{
    if (t < 0 || tokens[t].type != '{')
        return -1;
    for (int i = t + 1; i < tokens[t].end;) {
        int v = i + 1;
        if (eq(i, s))
            return v;
        i = tokens[v].end;
    }
    return -1;
}
static void stringval(int t, char *out, size_t size)
{
    if (t < 0 || tokens[t].type != '"')
        return;
    size_t k = 0;
    int end = tokens[t].start + tokens[t].len - 1;
    for (int i = tokens[t].start + 1; i < end && k + 1 < size; i++) {
        char c = json[i];
        if (c == '\\' && ++i < end) {
            c = json[i];
            if (c == 'u') {
                if (i + 4 >= end)
                    break;
                unsigned v = 0;
                for (int j = 0; j < 4; j++) {
                    char h = json[++i];
                    v = v * 16 + (h >= '0' && h <= '9'   ? h - '0'
                                  : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                  : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                         : 0);
                }
                c = v < 128 ? (char)v : '?';
            } else if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
        }
        out[k++] = c;
    }
    out[k] = 0;
}
static double number(int t, double fallback)
{
    if (t < 0 || tokens[t].type == 'n')
        return fallback;
    if (tokens[t].type == 't')
        return 1;
    if (tokens[t].type == 'f')
        return 0;
    if (tokens[t].type == '"' || tokens[t].type == '{' || tokens[t].type == '[')
        return fallback;
    return strtod(json + tokens[t].start, NULL);
}
static double clamp(double x, double lo, double hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}
enum {
    A,
    B,
    X,
    Y,
    L,
    R,
    ZL,
    ZR,
    MINUS,
    PLUS,
    HOME,
    L3,
    R3,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    CAPTURE,
    P1,
    P2,
    P3,
    P4,
    LGRIP,
    RGRIP,
    LGRIP2,
    RGRIP2,
    LPAD,
    RPAD,
    LSTICK,
    RSTICK,
    SLOTS
};
static const char *slots[SLOTS] = {
    "a",        "b",        "x",         "y",       "l",        "r",         "zl",
    "zr",       "minus",    "plus",      "home",    "l3",       "r3",        "dpadUp",
    "dpadDown", "dpadLeft", "dpadRight", "capture", "paddle1",  "paddle2",   "paddle3",
    "paddle4",  "leftGrip", "rightGrip", "leftGrip2", "rightGrip2", "leftPad", "rightPad",
    "leftStick", "rightStick"};
static const char *defaults[SLOTS] = {"a",
                                      "b",
                                      "x",
                                      "y",
                                      "leftshoulder",
                                      "rightshoulder",
                                      "lefttrigger",
                                      "righttrigger",
                                      "back",
                                      "start",
                                      "guide",
                                      "leftstick",
                                      "rightstick",
                                      "dpup",
                                      "dpdown",
                                      "dpleft",
                                      "dpright",
                                      "misc1",
                                      "paddle1",
                                      "paddle2",
                                      "paddle3",
                                      "paddle4",
                                      "paddle2", /* SDL: paddle1 upper right, 2 upper left, */
                                      "paddle1", /* 3 lower right, 4 lower left */
                                      "paddle4",
                                      "paddle3",
                                      "touchpad",
                                      "rightstick",
                                      "left",
                                      "right"};
typedef struct {
    const char *type, *name, *id;
} Layout;
static const Layout layouts[] = {
    {"xbox360", "Microsoft X-Box 360 pad", "0003 045e 028e 0114"},
    {"xboxone", "Microsoft X-Box One S pad", "0003 045e 02ea 0408"},
    {"xboxseries", "Microsoft Xbox Series S|X Controller", "0003 045e 0b12 0507"},
    {"xboxelite2", "Microsoft X-Box One Elite 2 pad", "0003 045e 0b00 0511"},
    {"ds3", "Sony PLAYSTATION(R)3 Controller", "0003 054c 0268 8111"},
    {"ds4", "Sony Interactive Entertainment Wireless Controller", "0003 054c 09cc 8111"},
    {"dualsense", "Sony Interactive Entertainment DualSense Wireless Controller",
     "0003 054c 0ce6 8111"},
    {"dualsenseedge", "Sony Interactive Entertainment DualSense Edge Wireless Controller",
     "0003 054c 0df2 8111"},
    {"steamcontroller", "Valve Software Steam Controller", "0003 28de 1102 0111"},
    {"switchpro", "Nintendo Switch Pro Controller", "0003 057e 2009 8111"},
    {"steamcontroller2", "Steam Controller", "0003 28de 1302 0100"}};
#define LAYOUTS ((int)(sizeof layouts / sizeof layouts[0]))
typedef struct {
    char guid[64], name[256], map[SLOTS][64], led[32];
    int layout;
    double dz[2], range[2], threshold, strength;
    int invert[4], rotate[2], rumble, motion;
} Config;
typedef struct {
    int type, code, min, max, fuzz, flat, value, slot;
} Value;
typedef struct {
    int fd;
    unsigned char rx[24];
    size_t used;
} Client;
typedef struct {
    int fd, n, index;
    SDL_GameController *ctrl;
    SDL_JoystickID instance;
    Config config;
    Value values[VALUES];
    Client clients[CLIENTS];
    int rawaxes[6], rawbuttons[SDL_CONTROLLER_BUTTON_MAX];
    Uint16 strong, weak;
    uint64_t renew;
} Device;
static Device devices[PLAYERS];
static Config configs[PLAYERS];
static char directory[PATH_MAX] = "/tmp/lxrt-input", configpath[PATH_MAX];
static volatile sig_atomic_t stopping;
static int fake_count, pidfd = -1;
static uint64_t script_start;
static int script_started;
static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}
static uint64_t now(void)
{
    return SDL_GetTicks64();
}
static void path(char *out, size_t size, const char *prefix, int i)
{
    snprintf(out, size, "%s/%s%d", directory, prefix, i);
}
static int layout_id(const char *s)
{
    if (!strcmp(s, "Xbox"))
        return 2;
    if (!strcmp(s, "ProController"))
        return 9;
    for (int i = 0; i < LAYOUTS; i++)
        if (!strcmp(s, layouts[i].type))
            return i;
    return -1;
}
static void default_config(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->layout = 2;
    c->dz[0] = c->dz[1] = 0.1;
    c->range[0] = c->range[1] = c->strength = 1;
    c->threshold = .5;
    c->rumble = 1;
    for (int i = 0; i < SLOTS; i++)
        snprintf(c->map[i], 64, "%s", defaults[i]);
}
static int load_config(void)
{
    FILE *f = fopen(configpath, "rb");
    if (!f) {
        if (errno != ENOENT)
            return 0;
        for (int i = 0; i < 4; i++)
            default_config(&configs[i]);
        return 1;
    }
    char *data = calloc(1, 262145);
    if (!data) {
        fclose(f);
        return 0;
    }
    size_t len = fread(data, 1, 262144, f);
    int bad = ferror(f) || !feof(f);
    fclose(f);
    json = data;
    nt = pos = 0;
    int root = bad ? -1 : parse(0);
    space();
    if (root < 0 || tokens[root].type != '{' || (size_t)pos != len) {
        fprintf(stderr, "invalid controllers JSON; retaining configuration\n");
        free(data);
        return 0;
    }
    Config next[4];
    for (int i = 0; i < 4; i++)
        default_config(&next[i]);
    int players = field(root, "players");
    if (players >= 0 && tokens[players].type == '[') {
        int t = players + 1;
        for (int p = 0; p < 4 && t < tokens[players].end; p++, t = tokens[t].end) {
            Config *c = &next[p];
            stringval(field(t, "deviceGUID"), c->guid, sizeof(c->guid));
            stringval(field(t, "deviceName"), c->name, sizeof(c->name));
            stringval(field(t, "ledColor"), c->led, sizeof(c->led));
            char type[64] = "xboxseries";
            stringval(field(t, "controllerType"), type, sizeof(type));
            int id = layout_id(type);
            if (id >= 0)
                c->layout = id;
            int m = field(t, "mapping");
            for (int j = 0; j < SLOTS; j++)
                stringval(field(m, slots[j]), c->map[j], 64);
            const char *dz[] = {"deadzoneLeft", "deadzoneRight"},
                       *range[] = {"rangeLeft", "rangeRight"}, *rot[] = {"rotateL", "rotateR"},
                       *inv[] = {"invertLX", "invertLY", "invertRX", "invertRY"};
            for (int j = 0; j < 2; j++) {
                c->dz[j] = clamp(number(field(t, dz[j]), .1), 0, 1);
                c->range[j] = clamp(number(field(t, range[j]), 1), 0, 100);
                c->rotate[j] = number(field(t, rot[j]), 0) != 0;
            }
            for (int j = 0; j < 4; j++)
                c->invert[j] = number(field(t, inv[j]), 0) != 0;
            c->threshold = clamp(number(field(t, "triggerThreshold"), .5), 0, 1);
            c->strength = clamp(number(field(t, "rumbleStrength"), 1), 0, 1);
            c->rumble = number(field(t, "rumble"), 1) != 0;
            c->motion = number(field(t, "motion"), 0) != 0;
        }
    }
    memcpy(configs, next, sizeof(configs));
    free(data);
    return 1;
}
static void add(Device *d, int type, int code, int slot, int min, int max, int fuzz, int flat)
{
    d->values[d->n++] = (Value){type, code, min, max, fuzz, flat, 0, slot};
}
static void make_layout(Device *d)
{
    int l = d->config.layout, ps = l >= 4 && l <= 7;
    d->n = 0;
    const int ss[] = {A, B, X, Y, L, R, MINUS, PLUS, HOME, L3, R3};
    const int codes[] = {304, 305, 307, 308, 310, 311, 314, 315, 316, 317, 318};
    for (int i = 0; i < 11; i++) {
        int c = codes[i];
        if ((ps || l == 9) && (i == 2 || i == 3))
            c = i == 2 ? 308 : 307;
        add(d, 1, c, ss[i], 0, 1, 0, 0);
    }
    if (l >= 4) {
        add(d, 1, 312, ZL, 0, 1, 0, 0);
        add(d, 1, 313, ZR, 0, 1, 0, 0);
    }
    if (l == 2 || l == 9 || l == 10)
        add(d, 1, l == 2 ? 167 : l == 9 ? 309 : 294, CAPTURE, 0, 1, 0, 0); /* 294: BTN_BASE (QAM) */
    if (l == 3)
        for (int i = 0; i < 4; i++)
            add(d, 1, 708 + i, P1 + i, 0, 1, 0, 0);
    if (l == 8) {
        add(d, 1, 289, LPAD, 0, 1, 0, 0);
        add(d, 1, 290, RPAD, 0, 1, 0, 0);
        add(d, 1, 336, LGRIP, 0, 1, 0, 0);
        add(d, 1, 337, RGRIP, 0, 1, 0, 0);
    }
    if (l == 10) { /* hid-steam, 2026 model: four rear grips, trackpad clicks */
        add(d, 1, 548, LGRIP, 0, 1, 0, 0);  /* BTN_GRIPL */
        add(d, 1, 549, RGRIP, 0, 1, 0, 0);  /* BTN_GRIPR */
        add(d, 1, 550, LGRIP2, 0, 1, 0, 0); /* BTN_GRIPL2 */
        add(d, 1, 551, RGRIP2, 0, 1, 0, 0); /* BTN_GRIPR2 */
        add(d, 1, 289, LPAD, 0, 1, 0, 0);   /* BTN_THUMB: left pad pressed */
        add(d, 1, 290, RPAD, 0, 1, 0, 0);   /* BTN_THUMB2: right pad pressed */
    }
    if (l == 4 || l == 8 || l == 10)
        for (int i = 0; i < 4; i++)
            add(d, 1, 544 + i, UP + i, 0, 1, 0, 0);
    int axes[] = {0, 1, 3, 4};
    for (int i = 0; i < 4; i++)
        add(d, 3, axes[i], 100 + i,
            ps      ? 0
            : l < 4 ? -32768
                    : -32767,
            ps ? 255 : 32767,
            l < 4    ? 16
            : l == 9 ? 250
                     : 0,
            l < 4    ? 128
            : l == 9 ? 500
                     : 0);
    if (l != 9) {
        int top = l == 10 ? 32767 : l > 0 && l < 4 ? 1023 : 255;
        add(d, 3, l == 8 || l == 10 ? 21 : 2, ZL, 0, top, 0, 0); /* HAT2Y on hid-steam */
        add(d, 3, l == 8 || l == 10 ? 20 : 5, ZR, 0, top, 0, 0); /* HAT2X */
    }
    if (l == 10) {
        /* Trackpad positions (HAT0 left, HAT1 right): 0 when not touched. The
           Mac side has no per-pad finger positions to forward, so they rest. */
        for (int i = 0; i < 4; i++)
            add(d, 3, 16 + i, 106 + i, -32767, 32767, 0, 0);
    } else if (l != 4) {
        add(d, 3, 16, 104, l == 8 ? -32767 : -1, l == 8 ? 32767 : 1, 0, 0);
        add(d, 3, 17, 105, l == 8 ? -32767 : -1, l == 8 ? 32767 : 1, 0, 0);
    }
}
static double control(Device *d, const char *name)
{
    SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name);
    if (a != SDL_CONTROLLER_AXIS_INVALID) {
        int v = d->ctrl ? SDL_GameControllerGetAxis(d->ctrl, a) : d->rawaxes[a];
        return clamp((double)v / (v < 0 ? 32768 : 32767),
                     a >= SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 0 : -1, 1);
    }
    SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name);
    return b == SDL_CONTROLLER_BUTTON_INVALID ? 0
           : d->ctrl                          ? SDL_GameControllerGetButton(d->ctrl, b)
                                              : d->rawbuttons[b];
}
static void state(Device *d, int *out)
{
    double s[SLOTS] = {0}, axes[4];
    for (int i = 0; i < LSTICK; i++)
        s[i] = control(d, d->config.map[i]);
    for (int i = 0; i < 2; i++) {
        char name[80];
        snprintf(name, sizeof(name), "%sx", d->config.map[LSTICK + i]);
        double x = control(d, name);
        snprintf(name, sizeof(name), "%sy", d->config.map[LSTICK + i]);
        double y = control(d, name);
        double mag = hypot(x, y), dz = d->config.dz[i];
        if (mag <= dz) {
            x = y = 0;
        } else {
            double scale = (fmin(mag, 1) - dz) / (1 - dz) / mag * d->config.range[i];
            x *= scale;
            y *= scale;
        }
        if (d->config.invert[i * 2])
            x = -x;
        if (d->config.invert[i * 2 + 1])
            y = -y;
        if (d->config.rotate[i]) {
            double old = x;
            x = -y;
            y = old;
        }
        axes[i * 2] = clamp(x, -1, 1);
        axes[i * 2 + 1] = clamp(y, -1, 1);
    }
    for (int i = 0; i < d->n; i++) {
        Value *v = &d->values[i];
        double x;
        if (v->slot >= 100 && v->slot < 104) {
            x = axes[v->slot - 100];
            out[i] = (int)lround(v->min == 0 ? 128 + x * (x < 0 ? 128 : 127)
                                             : x * (x < 0 ? -v->min : v->max));
        } else if (v->slot >= 106) {
            out[i] = 0;
        } else if (v->slot >= 104) {
            x = v->slot == 104 ? (s[RIGHT] > .5) - (s[LEFT] > .5) : (s[DOWN] > .5) - (s[UP] > .5);
            out[i] = (int)(x * v->max);
        } else if (v->type == 1)
            out[i] = s[v->slot] > (v->slot == ZL || v->slot == ZR ? d->config.threshold : .5);
        else
            out[i] = (int)lround(clamp(s[v->slot], 0, 1) * v->max);
    }
}
static void put(unsigned char *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (unsigned char)v;
        v >>= 8;
    }
}
static uint32_t get(const unsigned char *p, int n)
{
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}
static void record(unsigned char *p, int type, int code, int value, struct timeval *tv)
{
    put(p, (uint64_t)tv->tv_sec, 8);
    put(p + 8, (uint64_t)tv->tv_usec, 8);
    put(p + 16, (unsigned)type, 2);
    put(p + 18, (unsigned)code, 2);
    put(p + 20, (uint32_t)value, 4);
}
/* A short write cannot be retried without splitting a frame: disconnect instead. */
static void send_frame(Client *c, unsigned char *buf, size_t len)
{
    ssize_t n;
    do {
        n = write(c->fd, buf, len);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)len) {
        close(c->fd);
        c->fd = -1;
        c->used = 0;
    }
}
static void frame(Device *d, Client *snapshot)
{
    int values[VALUES];
    state(d, values);
    unsigned char buf[(VALUES + 1) * 24];
    size_t len = 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    for (int i = 0; i < d->n; i++) {
        Value *v = &d->values[i];
        if (snapshot ? (v->type == 3 || values[i]) : values[i] != v->value) {
            record(buf + len, v->type, v->code, values[i], &tv);
            len += 24;
        }
        if (!snapshot)
            v->value = values[i];
    }
    if (!len && !snapshot)
        return;
    record(buf + len, 0, 0, 0, &tv);
    len += 24;
    if (snapshot)
        send_frame(snapshot, buf, len);
    else
        for (int i = 0; i < CLIENTS; i++)
            if (d->clients[i].fd >= 0)
                send_frame(&d->clients[i], buf, len);
}
static void destroy(Device *d)
{
    d->renew = 0;
    d->strong = d->weak = 0;
    char p[PATH_MAX];
    if (d->fd >= 0) {
        path(p, sizeof(p), "event", d->index);
        unlink(p);
        close(d->fd);
        d->fd = -1;
        path(p, sizeof(p), "meta/event", d->index);
        unlink(p);
    }
    for (int i = 0; i < CLIENTS; i++)
        if (d->clients[i].fd >= 0) {
            close(d->clients[i].fd);
            d->clients[i].fd = -1;
        }
    if (d->ctrl) {
        SDL_GameControllerRumble(d->ctrl, 0, 0, 0);
        SDL_GameControllerClose(d->ctrl);
        d->ctrl = NULL;
    }
}
static int create(Device *d)
{
    make_layout(d);
    int values[VALUES];
    state(d, values);
    for (int i = 0; i < d->n; i++)
        d->values[i].value = values[i];
    char tmp[PATH_MAX], meta[PATH_MAX], sock[PATH_MAX];
    path(tmp, sizeof(tmp), "meta/.event", d->index);
    path(meta, sizeof(meta), "meta/event", d->index);
    path(sock, sizeof(sock), "event", d->index);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    const Layout *l = &layouts[d->config.layout];
    fprintf(f, "name %s\nid %s\nphys usb-steamarm-%d/input0\nuniq %s\nkey", l->name, l->id,
            d->index, d->config.guid);
    for (int i = 0; i < d->n; i++)
        if (d->values[i].type == 1)
            fprintf(f, " %d", d->values[i].code);
    fputc('\n', f);
    for (int i = 0; i < d->n; i++) {
        Value *v = &d->values[i];
        if (v->type == 3)
            fprintf(f, "abs %d %d %d %d %d 0\n", v->code, v->min, v->max, v->fuzz, v->flat);
    }
    fprintf(f, "ff %s\neffects 16\n", d->config.layout < 4 ? "80 81 88 89 90 96" : "80");
    int failed = ferror(f);
    if (fclose(f))
        failed = 1;
    if (failed || rename(tmp, meta)) {
        unlink(tmp);
        return -1;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(sock) >= sizeof(addr.sun_path)) {
        unlink(meta);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(addr.sun_path, sock);
    d->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (d->fd < 0 || fcntl(d->fd, F_SETFL, O_NONBLOCK) < 0 ||
        bind(d->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(d->fd, 16) < 0) {
        int saved = errno;
        if (d->fd >= 0)
            close(d->fd);
        d->fd = -1;
        unlink(sock);
        unlink(meta);
        errno = saved;
        return -1;
    }
    unsigned r, g, b;
    if (d->ctrl && strlen(d->config.led) == 7 && d->config.led[0] == '#' &&
        sscanf(d->config.led + 1, "%2x%2x%2x", &r, &g, &b) == 3)
        SDL_GameControllerSetLED(d->ctrl, (Uint8)r, (Uint8)g, (Uint8)b);
    return 0;
}
static int rebuild(int force)
{
    int count = SDL_NumJoysticks();
    int *chosen = calloc((size_t)(count > 0 ? count : 1), sizeof(int));
    if (!chosen)
        return -1;
    int indices[4] = {-1, -1, -1, -1};
    /* Reserve explicit GUIDs before assigning automatic players. */
    for (int pass = 0; pass < 2; pass++)
        for (int p = 0; p < 4; p++) {
            int explicit = configs[p].guid[0] && strcmp(configs[p].guid, "auto");
            if (explicit != (pass == 0))
                continue;
            for (int j = 0; j < count; j++) {
                if (chosen[j] || !SDL_IsGameController(j))
                    continue;
                char guid[33];
                SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(j), guid, sizeof(guid));
                if (explicit && strcasecmp(guid, configs[p].guid))
                    continue;
                chosen[j] = 1;
                indices[p] = j;
                break;
            }
        }
    free(chosen);
    for (int p = 0; p < 4; p++) {
        Device *d = &devices[p];
        int j = indices[p];
        SDL_JoystickID id = j < 0 ? -1 : SDL_JoystickGetDeviceInstanceID(j);
        if (!force && d->ctrl && d->instance == id && SDL_GameControllerGetAttached(d->ctrl))
            continue;
        destroy(d);
        d->config = configs[p];
        if (j >= 0) {
            d->ctrl = SDL_GameControllerOpen(j);
            d->instance = id;
            if (d->ctrl && create(d) < 0)
                return -1;
        }
    }
    return 0;
}
static void rumble(Device *d, Client *c)
{
    unsigned strong = get(c->rx + 4, 2), weak = get(c->rx + 6, 2), ms = get(c->rx + 8, 4);
    if (memcmp(c->rx, "RMBL", 4))
        return;
    fprintf(stderr, "rumble strong=%u weak=%u ms=%u\n", strong, weak, ms);
    fflush(stderr);
    if (d->ctrl && d->config.rumble) {
        d->strong = (Uint16)lround(strong * d->config.strength);
        d->weak = (Uint16)lround(weak * d->config.strength);
        int indefinite = ms == 0 || ms == 65535;
        SDL_GameControllerRumble(d->ctrl, d->strong, d->weak, indefinite ? 10000 : ms);
        d->renew = indefinite && (strong || weak) ? now() + 9000 : 0;
    }
}
typedef struct {
    uint64_t ms;
    char slot[64];
    int value;
} Script;
static Script *script;
static size_t script_count, script_index;
static int load_script(const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return -1;
    char line[256];
    uint64_t last = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long ms;
        char slot[64], extra;
        int value;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%llu %63s %d %c", &ms, slot, &value, &extra) != 3 || ms < last) {
            fclose(f);
            return -1;
        }
        Script *next = realloc(script, (script_count + 1) * sizeof(*script));
        if (!next) {
            fclose(f);
            return -1;
        }
        script = next;
        script[script_count].ms = ms;
        strcpy(script[script_count].slot, slot);
        script[script_count++].value = value;
        last = ms;
    }
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}
static void apply_script(void)
{
    if (!script_started)
        return;
    while (script_index < script_count && now() - script_start >= script[script_index].ms) {
        Script *s = &script[script_index++];
        Device *d = &devices[0];
        const char *name = s->slot;
        for (int i = 0; i < LSTICK; i++)
            if (!strcmp(name, slots[i])) {
                name = d->config.map[i];
                break;
            }
        SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name);
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name);
        if (a != SDL_CONTROLLER_AXIS_INVALID)
            d->rawaxes[a] = (int)clamp(s->value, -32768, 32767);
        else if (b != SDL_CONTROLLER_BUTTON_INVALID)
            d->rawbuttons[b] = s->value != 0;
        frame(d, NULL);
    }
}
static int setup_directory(void)
{
    if (mkdir(directory, 0700) < 0 && errno != EEXIST)
        return -1;
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/inputd.pid", directory);
    pidfd = open(p, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (pidfd < 0)
        return -1;
    if (flock(pidfd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "steamarm-inputd already running\n");
        close(pidfd);
        pidfd = -1;
        return 1;
    }
    char buf[64] = {0};
    ssize_t n = read(pidfd, buf, sizeof(buf) - 1);
    if (n > 0) {
        pid_t pid = (pid_t)strtol(buf, NULL, 10);
        char name[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (pid > 0 && kill(pid, 0) == 0 && proc_name(pid, name, sizeof(name)) > 0 &&
            !strcmp(name, "steamarm-inputd")) {
            fprintf(stderr, "steamarm-inputd already running\n");
            close(pidfd);
            pidfd = -1;
            return 1;
        }
    }
    if (ftruncate(pidfd, 0) < 0 || lseek(pidfd, 0, SEEK_SET) < 0 ||
        dprintf(pidfd, "%ld\n", (long)getpid()) < 0)
        return -1;
    snprintf(p, sizeof(p), "%s/meta", directory);
    if (mkdir(p, 0700) < 0 && errno != EEXIST)
        return -1;
    for (int i = 0; i < 4; i++) {
        path(p, sizeof(p), "event", i);
        unlink(p);
        path(p, sizeof(p), "meta/event", i);
        unlink(p);
        path(p, sizeof(p), "meta/.event", i);
        unlink(p);
    }
    return 0;
}
static void cleanup(void)
{
    for (int i = 0; i < 4; i++)
        destroy(&devices[i]);
    if (pidfd >= 0) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/inputd.pid", directory);
        unlink(p);
        close(pidfd);
        snprintf(p, sizeof(p), "%s/meta", directory);
        rmdir(p);
    }
    free(script);
    SDL_Quit();
}
static void usage(void)
{
    fprintf(stderr, "Usage: steamarm-inputd [--dir DIR] [--config PATH] [--fake TYPE[,TYPE...]] "
                    "[--script FILE] [--foreground]\n");
}
int main(int argc, char **argv)
{
    const char *fake = NULL, *scriptfile = NULL;
    int foreground = 0;
    const char *base = getenv("STEAMARM_STATE"), *home = getenv("HOME");
    if (base)
        snprintf(configpath, sizeof(configpath), "%s/launcher/controllers.json", base);
    else
        snprintf(configpath, sizeof(configpath), "%s/SteamARM-roots/launcher/controllers.json",
                 home ? home : ".");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--foreground")) {
            foreground = 1;
            continue;
        }
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        const char *key = argv[i], *value = argv[++i];
        if (!strcmp(key, "--dir")) {
            if (strlen(value) > 80) {
                fprintf(stderr, "socket directory too long\n");
                return 2;
            }
            strcpy(directory, value);
        } else if (!strcmp(key, "--config"))
            snprintf(configpath, sizeof(configpath), "%s", value);
        else if (!strcmp(key, "--fake"))
            fake = value;
        else if (!strcmp(key, "--script"))
            scriptfile = value;
        else {
            usage();
            return 2;
        }
    }
    for (int i = 0; i < 4; i++) {
        devices[i].index = i;
        devices[i].fd = -1;
        for (int j = 0; j < CLIENTS; j++)
            devices[i].clients[j].fd = -1;
        default_config(&configs[i]);
    }
    if (!load_config())
        fprintf(stderr, "using default configuration\n");
    if (fake) {
        char *copy = strdup(fake);
        if (!copy)
            return 1;
        char *save = NULL;
        for (char *s = strtok_r(copy, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
            int id = layout_id(s);
            if (id < 0 || fake_count == 4) {
                fprintf(stderr, "invalid fake controller list\n");
                free(copy);
                return 2;
            }
            configs[fake_count++].layout = id;
        }
        free(copy);
        if (!fake_count)
            return 2;
    }
    if (scriptfile && (!fake_count || load_script(scriptfile) < 0)) {
        fprintf(stderr, "invalid fake script\n");
        free(script);
        return 2;
    }
    if (!foreground) {
        pid_t pid = fork();
        if (pid < 0)
            return 1;
        if (pid > 0)
            return 0;
        if (setsid() < 0)
            return 1;
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            if (fd != STDIN_FILENO)
                close(fd);
        }
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    int result = setup_directory();
    if (result) {
        if (result < 0) {
            perror("input directory");
            cleanup();
        }
        return result > 0 ? 0 : 1;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        cleanup();
        return 1;
    }
    int status = 0;
    if (fake_count) {
        for (int i = 0; i < fake_count; i++) {
            devices[i].config = configs[i];
            if (create(&devices[i]) < 0) {
                perror("create device");
                status = 1;
                stopping = 1;
            }
        }
    } else if (rebuild(1) < 0) {
        status = 1;
        stopping = 1;
    }
    struct stat previous = {0};
    int existed = stat(configpath, &previous) == 0;
    uint64_t check = now() + 1000;
    while (!stopping) {
        SDL_PumpEvents();
        SDL_Event e;
        int hotplug = 0;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED)
                hotplug = 1;
        if (now() >= check) {
            struct stat current = {0};
            int exists = stat(configpath, &current) == 0;
            int changed =
                exists != existed ||
                (exists && (current.st_mtimespec.tv_sec != previous.st_mtimespec.tv_sec ||
                            current.st_mtimespec.tv_nsec != previous.st_mtimespec.tv_nsec ||
                            current.st_size != previous.st_size));
            if (changed && load_config()) {
                if (fake_count) {
                    for (int i = 0; i < fake_count; i++) {
                        Device *d = &devices[i];
                        int layout = d->config.layout;
                        destroy(d);
                        d->config = configs[i];
                        d->config.layout = layout;
                        if (create(d) < 0) {
                            perror("rebuild fake device");
                            status = 1;
                            stopping = 1;
                            break;
                        }
                    }
                } else if (rebuild(1) < 0) {
                    status = 1;
                    break;
                }
                previous = current;
                existed = exists;
            }
            check = now() + 1000;
        }
        if (hotplug && !fake_count && rebuild(0) < 0) {
            status = 1;
            break;
        }
        apply_script();
        struct pollfd polls[4 * (CLIENTS + 1)];
        Device *owners[4 * (CLIENTS + 1)];
        int which[4 * (CLIENTS + 1)], np = 0;
        for (int i = 0; i < 4; i++) {
            Device *d = &devices[i];
            if (d->fd < 0)
                continue;
            frame(d, NULL);
            if (d->renew && now() >= d->renew) {
                SDL_GameControllerRumble(d->ctrl, d->strong, d->weak, 10000);
                d->renew = now() + 9000;
            }
            polls[np] = (struct pollfd){d->fd, POLLIN, 0};
            owners[np] = d;
            which[np++] = -1;
            for (int j = 0; j < CLIENTS; j++)
                if (d->clients[j].fd >= 0) {
                    polls[np] = (struct pollfd){d->clients[j].fd, POLLIN, 0};
                    owners[np] = d;
                    which[np++] = j;
                }
        }
        int ready = poll(polls, (nfds_t)np, 4);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            status = 1;
            break;
        }
        for (int i = 0; i < np; i++) {
            if (!polls[i].revents)
                continue;
            Device *d = owners[i];
            if (which[i] < 0) {
                int fd = accept(d->fd, NULL, NULL);
                if (fd < 0)
                    continue;
                if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
                    close(fd);
                    continue;
                }
                int j;
                for (j = 0; j < CLIENTS; j++)
                    if (d->clients[j].fd < 0)
                        break;
                if (j == CLIENTS) {
                    close(fd);
                    continue;
                }
                Client *c = &d->clients[j];
                c->fd = fd;
                c->used = 0;
                frame(d, c);
                if (d->index == 0 && !script_started) {
                    script_start = now();
                    script_started = 1;
                }
            } else {
                Client *c = &d->clients[which[i]]; /* Limit work per client to keep the 4 ms loop
                                                      responsive. */
                for (int records = 0; records < 64;) {
                    ssize_t n = read(c->fd, c->rx + c->used, 24 - c->used);
                    if (n > 0) {
                        c->used += (size_t)n;
                        if (c->used == 24) {
                            rumble(d, c);
                            c->used = 0;
                            records++;
                        }
                    } else {
                        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                            close(c->fd);
                            c->fd = -1;
                        }
                        break;
                    }
                }
            }
        }
    }
    cleanup();
    return status;
}
