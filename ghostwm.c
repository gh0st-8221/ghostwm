#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>

#define MAX_VELOCITY 1500.0
#define MAX_KEYS 256
#define MAX_AUTOSTART 64
#define MAX_CLIENTS 1024
#define MAX_MONITORS 32
#define MAX_WS 9
#define MENU_ITEMS 256
#define PARK 30000
#define PL_MOVE 0
#define PL_FULL 3

typedef struct Monitor Monitor;
typedef struct Client Client;

typedef struct {
    double cx, cy, zoom;
} View;

struct Client {
    Window w;
    Monitor *mon;
    int ws;
    int fx, fy, fw, fh;
    int gx, gy, gw, gh;
    int fullscreen, minimized, mapped, ignore_unmap, redir;
    unsigned long z, last_focus;
    Damage damage;
    Pixmap named;
    Picture pic;
    Visual *visual;
};

struct Monitor {
    char name[64];
    int x, y, w, h;
    Window box, veil;
    int ws, tiling, overview, dirty;
    View view[MAX_WS];
    Client *sel[MAX_WS];
    Pixmap back;
    Picture back_pic, veil_pic;
};

typedef struct {
    char name[64];
    int x, y, w, h;
} MonInfo;

struct KeyBinding {
    unsigned int mod;
    KeySym keysym;
    char *cmd;
};

int border_width = 1;
unsigned long color_bg = 0x101010;
unsigned long color_fg = 0xffffff;
unsigned long color_border = 0x303030;
unsigned long color_focus = 0x0000ff;
int default_width = 1920;
int default_height = 1080;
int nws = 4;
double zoom_min = 0.4;
char setup_cmd[1024] = "";

struct KeyBinding keys[MAX_KEYS];
int keys_count = 0;
char *autostart_cmds[MAX_AUTOSTART];
int autostart_count = 0;

Display *dpy;
Window root;
int screen;
Atom wm_delete_window, wm_protocols, net_wm_name, utf8_string, net_wm_state, net_wm_fullscreen;
int damage_event_base, rr_event_base, have_composite;

Client *clients[MAX_CLIENTS];
int nclients = 0;
Monitor *mons[MAX_MONITORS];
int nmons = 0;
Client *focused = NULL;
unsigned long zcounter = 0, focus_counter = 0;

volatile sig_atomic_t reload_requested = 0;

int panning = 0, pan_select = 0, pan_moved = 0;
Monitor *pan_mon = NULL, *inertia_mon = NULL;
int pan_x, pan_y, pan_start_x, pan_start_y;
double vel_x = 0.0, vel_y = 0.0, last_move_ms = 0.0, last_tick_ms = 0.0, last_paint_ms = 0.0;

int drag_mode = 0, drag_px, drag_py, drag_fx, drag_fy, drag_fw, drag_fh;
double drag_ox, drag_oy;
Client *drag_c = NULL;

pid_t menu_pid = 0;
int menu_fd = -1, menu_n = 0, menu_len = 0;
Window menu_wins[MENU_ITEMS];
char menu_buf[64];

int wm_detected = 0;

static void layout(Monitor *m, int full);
static void set_focus(Client *c);
static void activate(Client *c);
static void switch_ws(Monitor *m, int n);
static void overview_begin(Monitor *m);
static void overview_end(Monitor *m);
static void set_fullscreen(Client *c, int on);
static void client_to_monitor(Client *c, Monitor *t);
static void unmanage(Client *c, int destroyed);

void handle_sigusr1(int sig) {
    (void)sig;
    reload_requested = 1;
}

static int clampi(int v, int lo, int hi) {
    if (hi < lo) hi = lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int fixi(double v) {
    return clampi((int)lround(v), -PARK, PARK);
}

static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = '\0';
    return s;
}

static void parse_key_str(const char *kstr, const char *cmd) {
    if (keys_count >= MAX_KEYS) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", kstr);
    char *tokens[8];
    int nt = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "+", &save); t && nt < 8; t = strtok_r(NULL, "+", &save)) tokens[nt++] = trim(t);
    if (nt == 0) return;
    unsigned int mod = 0;
    for (int i = 0; i < nt - 1; i++) {
        if (!strcmp(tokens[i], "Mod4") || !strcmp(tokens[i], "Super")) mod |= Mod4Mask;
        else if (!strcmp(tokens[i], "Mod1") || !strcmp(tokens[i], "Alt")) mod |= Mod1Mask;
        else if (!strcmp(tokens[i], "Shift")) mod |= ShiftMask;
        else if (!strcmp(tokens[i], "Control") || !strcmp(tokens[i], "Ctrl")) mod |= ControlMask;
    }
    char clean[64];
    snprintf(clean, sizeof(clean), "%s", tokens[nt - 1]);
    if (strlen(clean) == 1) clean[0] = (char)tolower((unsigned char)clean[0]);
    KeySym sym = XStringToKeysym(clean);
    if (sym == NoSymbol && strlen(clean) == 1) sym = (KeySym)clean[0];
    if (sym == NoSymbol) return;
    keys[keys_count].mod = mod;
    keys[keys_count].keysym = sym;
    keys[keys_count].cmd = strdup(cmd);
    keys_count++;
}

static void load_config(void) {
    for (int i = 0; i < keys_count; i++) free(keys[i].cmd);
    keys_count = 0;
    for (int i = 0; i < autostart_count; i++) free(autostart_cmds[i]);
    autostart_count = 0;
    setup_cmd[0] = '\0';

    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/.config/ghostwm/config.toml", home);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        parse_key_str("Mod4+f", "fullscreen");
        return;
    }

    char line[2048];
    char section[64] = "";
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim(line);
        if (*s == '#' || *s == '\0') continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", trim(s + 1));
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        size_t vl = strlen(val);
        int quoted = 0;
        if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') || (val[0] == '\'' && val[vl - 1] == '\''))) {
            val[vl - 1] = '\0';
            val++;
            quoted = 1;
        }

        if (!strcmp(section, "general")) {
            if (!strcmp(key, "border_width")) border_width = clampi(atoi(val), 0, 20);
            else if (!strcmp(key, "default_width")) default_width = clampi(atoi(val), 100, 16000);
            else if (!strcmp(key, "default_height")) default_height = clampi(atoi(val), 100, 16000);
            else if (!strcmp(key, "workspaces")) nws = clampi(atoi(val), 1, MAX_WS);
            else if (!strcmp(key, "zoom_min")) zoom_min = clampd(atof(val), 0.15, 0.95);
        } else if (!strcmp(section, "colors") && quoted) {
            if (!strcmp(key, "bg")) color_bg = strtoul(val, NULL, 0);
            else if (!strcmp(key, "fg")) color_fg = strtoul(val, NULL, 0);
            else if (!strcmp(key, "border")) color_border = strtoul(val, NULL, 0);
            else if (!strcmp(key, "focus")) color_focus = strtoul(val, NULL, 0);
        } else if (!strcmp(section, "monitors") && quoted) {
            if (!strcmp(key, "setup")) snprintf(setup_cmd, sizeof(setup_cmd), "%s", val);
        } else if (!strcmp(section, "autostart") && quoted) {
            if (!strcmp(key, "cmd") && autostart_count < MAX_AUTOSTART) autostart_cmds[autostart_count++] = strdup(val);
        } else if (!strcmp(section, "keys") && quoted) {
            parse_key_str(key, val);
        }
    }
    fclose(fp);
    parse_key_str("Mod4+f", "fullscreen");
}

static void spawn(const char *cmd) {
    if (fork() == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(1);
    }
}

static void run_autostart(void) {
    for (int i = 0; i < autostart_count; i++) spawn(autostart_cmds[i]);
}

static void run_setup(void) {
    if (!setup_cmd[0]) return;
    signal(SIGCHLD, SIG_DFL);
    if (system(setup_cmd) == -1) fprintf(stderr, "ghostwm: monitor setup failed\n");
    signal(SIGCHLD, SIG_IGN);
    XSync(dpy, False);
}

static void pointer_pos(int *x, int *y) {
    Window a, b;
    int wx, wy;
    unsigned int mask;
    if (!XQueryPointer(dpy, root, &a, &b, x, y, &wx, &wy, &mask)) {
        *x = 0;
        *y = 0;
    }
}

static Monitor *mon_at(int x, int y) {
    Monitor *best = mons[0];
    long bd = -1;
    for (int i = 0; i < nmons; i++) {
        Monitor *m = mons[i];
        int dx = x < m->x ? m->x - x : (x >= m->x + m->w ? x - (m->x + m->w - 1) : 0);
        int dy = y < m->y ? m->y - y : (y >= m->y + m->h ? y - (m->y + m->h - 1) : 0);
        long d = (long)dx * dx + (long)dy * dy;
        if (d == 0) return m;
        if (bd < 0 || d < bd) {
            bd = d;
            best = m;
        }
    }
    return best;
}

static Monitor *cur_mon(void) {
    int x, y;
    pointer_pos(&x, &y);
    return mon_at(x, y);
}

static int mon_index(Monitor *m) {
    for (int i = 0; i < nmons; i++) if (mons[i] == m) return i;
    return 0;
}

static Monitor *mon_target(Monitor *from, const char *arg) {
    int i = mon_index(from);
    if (!strcmp(arg, "next")) return mons[(i + 1) % nmons];
    if (!strcmp(arg, "prev")) return mons[(i + nmons - 1) % nmons];
    int n = atoi(arg);
    if (n >= 1 && n <= nmons) return mons[n - 1];
    return NULL;
}

static Monitor *neighbor(Monitor *m, int dir) {
    double mx = m->x + m->w / 2.0, my = m->y + m->h / 2.0;
    Monitor *best = NULL;
    double best_score = 0;
    for (int i = 0; i < nmons; i++) {
        Monitor *o = mons[i];
        if (o == m) continue;
        double dx = o->x + o->w / 2.0 - mx, dy = o->y + o->h / 2.0 - my;
        double prim, sec;
        if (dir == 0) { prim = -dx; sec = fabs(dy); }
        else if (dir == 1) { prim = dx; sec = fabs(dy); }
        else if (dir == 2) { prim = -dy; sec = fabs(dx); }
        else { prim = dy; sec = fabs(dx); }
        if (prim <= 0.5) continue;
        double score = prim + 2.0 * sec;
        if (!best || score < best_score) {
            best = o;
            best_score = score;
        }
    }
    return best;
}

static Client *find_client(Window w) {
    for (int i = 0; i < nclients; i++) if (clients[i]->w == w) return clients[i];
    return NULL;
}

static int should_show(Client *c) {
    return c->ws == c->mon->ws && !c->minimized;
}

static int has_fullscreen(Monitor *m) {
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->mon == m && c->fullscreen && should_show(c)) return 1;
    }
    return 0;
}

static Client *fallback_client(Monitor *m) {
    Client *s = m->sel[m->ws];
    if (s && s->mon == m && should_show(s)) return s;
    Client *best = NULL;
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->mon == m && should_show(c) && (!best || c->last_focus > best->last_focus)) best = c;
    }
    return best;
}

static void send_configure(Client *c) {
    XConfigureEvent ce;
    memset(&ce, 0, sizeof(ce));
    ce.type = ConfigureNotify;
    ce.display = dpy;
    ce.event = c->w;
    ce.window = c->w;
    ce.x = c->mon->x + c->gx;
    ce.y = c->mon->y + c->gy;
    ce.width = c->gw;
    ce.height = c->gh;
    ce.border_width = c->fullscreen ? 0 : border_width;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(dpy, c->w, False, StructureNotifyMask, (XEvent *)&ce);
}

static void client_drop_pixmap(Client *c) {
    if (c->pic) {
        XRenderFreePicture(dpy, c->pic);
        c->pic = None;
    }
    if (c->named) {
        XFreePixmap(dpy, c->named);
        c->named = None;
    }
}

static void client_redirect(Client *c) {
    if (c->redir || !have_composite) return;
    XCompositeRedirectWindow(dpy, c->w, CompositeRedirectManual);
    c->damage = XDamageCreate(dpy, c->w, XDamageReportNonEmpty);
    c->redir = 1;
}

static void client_unredirect(Client *c) {
    if (!c->redir) return;
    client_drop_pixmap(c);
    if (c->damage) {
        XDamageDestroy(dpy, c->damage);
        c->damage = None;
    }
    XCompositeUnredirectWindow(dpy, c->w, CompositeRedirectManual);
    c->redir = 0;
}

static void place(Client *c, int flags) {
    if (c->gw < 1) c->gw = 1;
    if (c->gh < 1) c->gh = 1;
    if (flags & 1) {
        if (c->redir) client_drop_pixmap(c);
        XMoveResizeWindow(dpy, c->w, c->gx, c->gy, c->gw, c->gh);
    } else {
        XMoveWindow(dpy, c->w, c->gx, c->gy);
    }
    if (flags & 2) send_configure(c);
}

static void sync_visibility(Monitor *m, int show_pass) {
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->mon != m) continue;
        int want = should_show(c);
        if (show_pass && want && !c->mapped) {
            XMapWindow(dpy, c->w);
            c->mapped = 1;
        } else if (!show_pass && !want && c->mapped) {
            client_unredirect(c);
            c->ignore_unmap++;
            XUnmapWindow(dpy, c->w);
            c->mapped = 0;
        }
    }
}

static void float_update(Client *c, int flags) {
    Monitor *m = c->mon;
    View *v = &m->view[c->ws];
    c->gx = fixi(m->w / 2.0 + (c->fx - v->cx));
    c->gy = fixi(m->h / 2.0 + (c->fy - v->cy));
    c->gw = c->fw;
    c->gh = c->fh;
    place(c, flags);
}

static void float_place(Monitor *m, int full) {
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->mon == m && should_show(c) && !c->fullscreen) float_update(c, full ? PL_FULL : PL_MOVE);
    }
}

static void tile(Monitor *m) {
    Client *list[MAX_CLIENTS];
    int n = 0;
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->mon == m && should_show(c) && !c->fullscreen) list[n++] = c;
    }
    int b2 = border_width * 2;
    int half = m->w / 2;
    for (int idx = 0; idx < n; idx++) {
        int x, y, w, h;
        if (n == 1) {
            x = 0; y = 0; w = m->w - b2; h = m->h - b2;
        } else if (n == 2) {
            x = idx == 0 ? 0 : half; y = 0; w = half - b2; h = m->h - b2;
        } else if (idx == 0) {
            x = 0; y = 0; w = half - b2; h = m->h - b2;
        } else {
            int sub = m->h / (n - 1);
            x = half; y = (idx - 1) * sub; w = half - b2; h = sub - b2;
        }
        Client *c = list[idx];
        c->gx = x;
        c->gy = y;
        c->gw = w;
        c->gh = h;
        place(c, PL_FULL);
    }
}

static void place_fullscreen(Monitor *m) {
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->mon != m || !c->fullscreen || !should_show(c)) continue;
        c->gx = 0;
        c->gy = 0;
        c->gw = m->w;
        c->gh = m->h;
        place(c, PL_FULL);
        XRaiseWindow(dpy, c->w);
    }
}

static void overview_sync(Monitor *m) {
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->mon != m || !c->mapped || c->redir) continue;
        client_redirect(c);
        c->gx = 0;
        c->gy = 0;
        c->gw = c->fw;
        c->gh = c->fh;
        place(c, 1);
    }
}

static void overview_begin(Monitor *m) {
    if (m->overview || !have_composite) return;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
    if (!fmt) return;
    m->overview = 1;
    m->back = XCreatePixmap(dpy, root, m->w, m->h, DefaultDepth(dpy, screen));
    m->back_pic = XRenderCreatePicture(dpy, m->back, fmt, 0, NULL);
    m->veil_pic = XRenderCreatePicture(dpy, m->veil, fmt, 0, NULL);
    overview_sync(m);
    XMoveResizeWindow(dpy, m->veil, m->x, m->y, m->w, m->h);
    XMapRaised(dpy, m->veil);
    m->dirty = 1;
}

static void overview_end(Monitor *m) {
    if (!m->overview) return;
    m->overview = 0;
    m->dirty = 0;
    for (int i = 0; i < nclients; i++) if (clients[i]->mon == m) client_unredirect(clients[i]);
    XUnmapWindow(dpy, m->veil);
    if (m->veil_pic) XRenderFreePicture(dpy, m->veil_pic);
    if (m->back_pic) XRenderFreePicture(dpy, m->back_pic);
    if (m->back) XFreePixmap(dpy, m->back);
    m->veil_pic = None;
    m->back_pic = None;
    m->back = None;
}

static void layout(Monitor *m, int full) {
    sync_visibility(m, 0);
    if (m->tiling) {
        m->view[m->ws].zoom = 1.0;
        if (m->overview) overview_end(m);
        tile(m);
        place_fullscreen(m);
        sync_visibility(m, 1);
        return;
    }
    View *v = &m->view[m->ws];
    if (v->zoom < 1.0 && !has_fullscreen(m) && have_composite) {
        sync_visibility(m, 1);
        if (!m->overview) overview_begin(m);
        else overview_sync(m);
        m->dirty = 1;
        return;
    }
    v->zoom = 1.0;
    if (m->overview) overview_end(m);
    float_place(m, full);
    place_fullscreen(m);
    sync_visibility(m, 1);
}

static void paint(Monitor *m) {
    if (!m->overview) return;
    View *v = &m->view[m->ws];
    double z = v->zoom;
    XRenderColor bg;
    bg.red = (unsigned short)(((color_bg >> 16) & 0xff) * 257);
    bg.green = (unsigned short)(((color_bg >> 8) & 0xff) * 257);
    bg.blue = (unsigned short)((color_bg & 0xff) * 257);
    bg.alpha = 0xffff;
    XRenderFillRectangle(dpy, PictOpSrc, m->back_pic, &bg, 0, 0, m->w, m->h);

    Client *order[MAX_CLIENTS];
    int n = 0;
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->mon == m && c->mapped && c->redir) order[n++] = c;
    }
    for (int i = 1; i < n; i++) {
        Client *k = order[i];
        int j = i - 1;
        while (j >= 0 && order[j]->z > k->z) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }

    XTransform xf;
    memset(&xf, 0, sizeof(xf));
    xf.matrix[0][0] = XDoubleToFixed(1.0 / z);
    xf.matrix[1][1] = XDoubleToFixed(1.0 / z);
    xf.matrix[2][2] = XDoubleToFixed(1.0);

    for (int i = 0; i < n; i++) {
        Client *c = order[i];
        if (!c->named) {
            XRenderPictFormat *f = XRenderFindVisualFormat(dpy, c->visual);
            if (!f) continue;
            c->named = XCompositeNameWindowPixmap(dpy, c->w);
            c->pic = XRenderCreatePicture(dpy, c->named, f, 0, NULL);
        }
        int dx = (int)lround(m->w / 2.0 + (c->fx - v->cx) * z);
        int dy = (int)lround(m->h / 2.0 + (c->fy - v->cy) * z);
        int dw = (int)lround((c->fw + 2 * border_width) * z);
        int dh = (int)lround((c->fh + 2 * border_width) * z);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if (dx >= m->w || dy >= m->h || dx + dw <= 0 || dy + dh <= 0) continue;
        XRenderSetPictureTransform(dpy, c->pic, &xf);
        XRenderSetPictureFilter(dpy, c->pic, FilterBilinear, NULL, 0);
        XRenderComposite(dpy, PictOpOver, c->pic, None, m->back_pic, 0, 0, 0, 0, dx, dy, dw, dh);
    }
    XRenderComposite(dpy, PictOpSrc, m->back_pic, None, m->veil_pic, 0, 0, 0, 0, 0, 0, m->w, m->h);
    m->dirty = 0;
}

static void client_center(Client *c, int *px, int *py) {
    Monitor *m = c->mon;
    double bw2 = c->fullscreen ? 0 : 2.0 * border_width;
    if (m->overview) {
        View *v = &m->view[m->ws];
        *px = (int)lround(m->x + m->w / 2.0 + (c->fx + (c->fw + bw2) / 2.0 - v->cx) * v->zoom);
        *py = (int)lround(m->y + m->h / 2.0 + (c->fy + (c->fh + bw2) / 2.0 - v->cy) * v->zoom);
    } else {
        *px = m->x + c->gx + (int)((c->gw + bw2) / 2.0);
        *py = m->y + c->gy + (int)((c->gh + bw2) / 2.0);
    }
}

static void warp_to(Client *c) {
    if (!c || !c->mapped) return;
    int px, py;
    client_center(c, &px, &py);
    XWarpPointer(dpy, None, root, 0, 0, 0, 0, px, py);
}

static Client *client_at(Monitor *m, int sx, int sy) {
    Client *best = NULL;
    View *v = &m->view[m->ws];
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->mon != m || !c->mapped || !should_show(c)) continue;
        double bw2 = c->fullscreen ? 0 : 2.0 * border_width;
        double x, y, w, h;
        if (m->overview) {
            x = m->w / 2.0 + (c->fx - v->cx) * v->zoom;
            y = m->h / 2.0 + (c->fy - v->cy) * v->zoom;
            w = (c->fw + bw2) * v->zoom;
            h = (c->fh + bw2) * v->zoom;
        } else {
            x = c->gx;
            y = c->gy;
            w = c->gw + bw2;
            h = c->gh + bw2;
        }
        if (sx >= x && sx < x + w && sy >= y && sy < y + h && (!best || c->z > best->z)) best = c;
    }
    return best;
}

static void raise_client(Client *c) {
    c->z = ++zcounter;
    if (c->mapped) XRaiseWindow(dpy, c->w);
}

static void reveal(Client *c) {
    Monitor *m = c->mon;
    if (m->tiling || c->fullscreen || c->ws != m->ws) return;
    View *v = &m->view[m->ws];
    double vw = m->w / v->zoom, vh = m->h / v->zoom;
    double l = v->cx - vw / 2.0, t = v->cy - vh / 2.0;
    double cw = c->fw + 2.0 * border_width, ch = c->fh + 2.0 * border_width;
    if (c->fx < l || c->fy < t || c->fx + cw > l + vw || c->fy + ch > t + vh) {
        v->cx = c->fx + cw / 2.0;
        v->cy = c->fy + ch / 2.0;
        layout(m, 1);
    }
}

static void set_focus(Client *c) {
    if (focused && focused != c) {
        XSetWindowBorder(dpy, focused->w, color_border);
        if (c && focused->fullscreen && focused->mon == c->mon) set_fullscreen(focused, 0);
    }
    focused = c;
    if (c) {
        XSetInputFocus(dpy, c->w, RevertToPointerRoot, CurrentTime);
        XSetWindowBorder(dpy, c->w, color_focus);
        c->last_focus = ++focus_counter;
        c->mon->sel[c->ws] = c;
    } else {
        XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    }
}

static void set_fullscreen(Client *c, int on) {
    if (c->fullscreen == on) return;
    Monitor *m = c->mon;
    c->fullscreen = on;
    if (on) {
        m->view[c->ws].zoom = 1.0;
        XSetWindowBorderWidth(dpy, c->w, 0);
        XChangeProperty(dpy, c->w, net_wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&net_wm_fullscreen, 1);
    } else {
        XSetWindowBorderWidth(dpy, c->w, border_width);
        XDeleteProperty(dpy, c->w, net_wm_state);
    }
    layout(m, 1);
    if (on) XRaiseWindow(dpy, c->w);
}

static void activate(Client *c) {
    Monitor *m = c->mon;
    if (c->ws != m->ws) switch_ws(m, c->ws);
    if (c->minimized) {
        c->minimized = 0;
        layout(m, 1);
    }
    set_focus(c);
    raise_client(c);
    reveal(c);
    warp_to(c);
}

static void switch_ws(Monitor *m, int n) {
    if (n < 0 || n >= nws || m->ws == n) return;
    if (m->overview) overview_end(m);
    m->ws = n;
    layout(m, 1);
    Client *t = fallback_client(m);
    if (t) {
        set_focus(t);
        warp_to(t);
    } else {
        set_focus(NULL);
    }
}

static void move_to_ws(Client *c, int n) {
    if (n < 0 || n >= nws || c->ws == n) return;
    Monitor *m = c->mon;
    int was_focused = focused == c;
    for (int k = 0; k < MAX_WS; k++) if (m->sel[k] == c) m->sel[k] = NULL;
    c->ws = n;
    layout(m, 1);
    if (was_focused) {
        Client *t = fallback_client(m);
        set_focus(t);
        warp_to(t);
    }
}

static void move_to_end(Client *c) {
    int idx = -1;
    for (int i = 0; i < nclients; i++) if (clients[i] == c) idx = i;
    if (idx < 0) return;
    for (int i = idx; i < nclients - 1; i++) clients[i] = clients[i + 1];
    clients[nclients - 1] = c;
}

static void client_to_monitor(Client *c, Monitor *t) {
    Monitor *s = c->mon;
    if (s == t) return;
    if (t->overview) {
        t->view[t->ws].zoom = 1.0;
        overview_end(t);
    }
    client_unredirect(c);
    if (c->fullscreen) {
        c->fullscreen = 0;
        XSetWindowBorderWidth(dpy, c->w, border_width);
        XDeleteProperty(dpy, c->w, net_wm_state);
    }
    if (c->mapped) c->ignore_unmap++;
    XReparentWindow(dpy, c->w, t->box, 0, 0);
    for (int k = 0; k < MAX_WS; k++) if (s->sel[k] == c) s->sel[k] = NULL;
    c->mon = t;
    c->ws = t->ws;
    c->minimized = 0;
    c->fw = clampi(c->fw, 1, t->w - 2 * border_width);
    c->fh = clampi(c->fh, 1, t->h - 2 * border_width);
    View *v = &t->view[t->ws];
    c->fx = (int)lround(v->cx - (c->fw + 2.0 * border_width) / 2.0);
    c->fy = (int)lround(v->cy - (c->fh + 2.0 * border_width) / 2.0);
    c->z = ++zcounter;
    move_to_end(c);
    layout(s, 1);
    layout(t, 1);
    if (s->overview) s->dirty = 1;
}

static void set_zoom(Monitor *m, double z, int ax, int ay) {
    if (m->tiling || !have_composite || has_fullscreen(m)) return;
    View *v = &m->view[m->ws];
    z = clampd(z, zoom_min, 1.0);
    if (z > 0.995) z = 1.0;
    if (z == v->zoom) return;
    double wx = v->cx + (ax - m->w / 2.0) / v->zoom;
    double wy = v->cy + (ay - m->h / 2.0) / v->zoom;
    v->zoom = z;
    v->cx = wx - (ax - m->w / 2.0) / z;
    v->cy = wy - (ay - m->h / 2.0) / z;
    layout(m, 1);
}

static void zoom_step(Monitor *m, double f, int ax, int ay) {
    if (ax < 0) ax = m->w / 2;
    if (ay < 0) ay = m->h / 2;
    set_zoom(m, m->view[m->ws].zoom * f, ax, ay);
}

static void pan_apply(Monitor *m, double dx, double dy) {
    View *v = &m->view[m->ws];
    v->cx -= dx / v->zoom;
    v->cy -= dy / v->zoom;
    if (m->overview) m->dirty = 1;
    else float_place(m, 0);
}

static void zoom_to_client(Client *c) {
    Monitor *m = c->mon;
    View *v = &m->view[m->ws];
    v->zoom = 1.0;
    v->cx = c->fx + (c->fw + 2.0 * border_width) / 2.0;
    v->cy = c->fy + (c->fh + 2.0 * border_width) / 2.0;
    layout(m, 1);
    set_focus(c);
    raise_client(c);
    warp_to(c);
}

static void lrect(Client *c, double *x, double *y, double *w, double *h) {
    double bw2 = c->fullscreen ? 0 : 2.0 * border_width;
    if (c->mon->tiling || c->fullscreen) {
        *x = c->gx; *y = c->gy; *w = c->gw + bw2; *h = c->gh + bw2;
    } else {
        *x = c->fx; *y = c->fy; *w = c->fw + bw2; *h = c->fh + bw2;
    }
}

static Client *pick_dir(Client *cur, int dir) {
    double x, y, w, h;
    lrect(cur, &x, &y, &w, &h);
    double ccx = x + w / 2.0, ccy = y + h / 2.0;
    Client *best = NULL;
    double best_score = 0;
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c == cur || c->mon != cur->mon || !should_show(c)) continue;
        double ox, oy, ow, oh;
        lrect(c, &ox, &oy, &ow, &oh);
        double dx = ox + ow / 2.0 - ccx, dy = oy + oh / 2.0 - ccy;
        double prim, sec;
        if (dir == 0) { prim = -dx; sec = fabs(dy); }
        else if (dir == 1) { prim = dx; sec = fabs(dy); }
        else if (dir == 2) { prim = -dy; sec = fabs(dx); }
        else { prim = dy; sec = fabs(dx); }
        if (prim <= 0.5) continue;
        double score = prim + 2.0 * sec;
        if (!best || score < best_score) {
            best = c;
            best_score = score;
        }
    }
    return best;
}

static void focus_monitor(Monitor *t) {
    Client *c = fallback_client(t);
    if (c) {
        set_focus(c);
        warp_to(c);
    } else {
        set_focus(NULL);
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, t->x + t->w / 2, t->y + t->h / 2);
    }
}

static Client *current_client(Monitor *m) {
    if (focused && focused->mon == m && should_show(focused)) return focused;
    return fallback_client(m);
}

static void focus_dir(int dir) {
    Monitor *m = cur_mon();
    Client *cur = current_client(m);
    if (cur) {
        Client *t = pick_dir(cur, dir);
        if (t) {
            activate(t);
            return;
        }
    }
    Monitor *n = neighbor(m, dir);
    if (n) focus_monitor(n);
}

static void move_dir(int dir) {
    Monitor *m = cur_mon();
    Client *c = current_client(m);
    if (!c) return;
    if (m->tiling) {
        Client *t = pick_dir(c, dir);
        if (t) {
            int ia = -1, ib = -1;
            for (int i = 0; i < nclients; i++) {
                if (clients[i] == c) ia = i;
                if (clients[i] == t) ib = i;
            }
            if (ia >= 0 && ib >= 0) {
                clients[ia] = t;
                clients[ib] = c;
                layout(m, 1);
                warp_to(c);
            }
        } else {
            Monitor *n = neighbor(m, dir);
            if (n) {
                client_to_monitor(c, n);
                set_focus(c);
                warp_to(c);
            }
        }
    } else {
        double step = 50.0 / m->view[m->ws].zoom;
        if (dir == 0) c->fx -= (int)step;
        else if (dir == 1) c->fx += (int)step;
        else if (dir == 2) c->fy -= (int)step;
        else c->fy += (int)step;
        layout(m, 1);
        warp_to(c);
    }
}

static void toggle_tiling(Monitor *m) {
    m->tiling = !m->tiling;
    if (m->tiling) m->view[m->ws].zoom = 1.0;
    layout(m, 1);
    Client *c = current_client(m);
    if (c) {
        set_focus(c);
        warp_to(c);
    }
}

static void toggle_minimize(Client *c) {
    Monitor *m = c->mon;
    c->minimized = !c->minimized;
    layout(m, 1);
    if (c->minimized) {
        if (focused == c) {
            Client *n = fallback_client(m);
            set_focus(n);
            warp_to(n);
        }
    } else {
        set_focus(c);
        raise_client(c);
    }
}

static void send_event(Window w, Atom proto) {
    int n, exists = 0;
    Atom *protocols = NULL;
    if (XGetWMProtocols(dpy, w, &protocols, &n)) {
        while (--n >= 0) {
            if (protocols[n] == proto) {
                exists = 1;
                break;
            }
        }
        if (protocols) XFree(protocols);
    }
    if (exists) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ClientMessage;
        ev.xclient.window = w;
        ev.xclient.message_type = wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)wm_delete_window;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, w, False, NoEventMask, &ev);
    } else {
        XKillClient(dpy, w);
    }
}

static void get_title(Window w, char *out, size_t n) {
    XTextProperty prop;
    out[0] = '\0';
    if (XGetTextProperty(dpy, w, &prop, net_wm_name) && prop.value) {
        snprintf(out, n, "%s", (char *)prop.value);
        XFree(prop.value);
    } else if (XGetWMName(dpy, w, &prop) && prop.value) {
        snprintf(out, n, "%s", (char *)prop.value);
        XFree(prop.value);
    }
    if (!out[0]) snprintf(out, n, "Unnamed");
    for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
}

static void open_menu(void) {
    if (menu_fd >= 0 || nclients == 0) return;
    Client *list[MENU_ITEMS];
    int n = 0;
    for (int i = 0; i < nclients && n < MENU_ITEMS; i++) list[n++] = clients[i];
    for (int i = 1; i < n; i++) {
        Client *k = list[i];
        int j = i - 1;
        while (j >= 0 && list[j]->last_focus < k->last_focus) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = k;
    }
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        return;
    }
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execlp("rofi", "rofi", "-dmenu", "-i", "-p", "Windows", "-format", "i", "-selected-row", n > 1 ? "1" : "0", (char *)NULL);
        _exit(1);
    }
    close(in[0]);
    close(out[1]);
    char text[MENU_ITEMS * 140];
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        char title[100];
        get_title(list[i]->w, title, sizeof(title));
        int w = snprintf(text + len, sizeof(text) - len, "[%d:%d] %s\n", mon_index(list[i]->mon) + 1, list[i]->ws + 1, title);
        if (w > 0) len += (size_t)w;
        menu_wins[i] = list[i]->w;
    }
    if (write(in[1], text, len) < 0) {
    }
    close(in[1]);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    menu_fd = out[0];
    menu_pid = pid;
    menu_n = n;
    menu_len = 0;
    menu_buf[0] = '\0';
}

static void menu_poll_result(void) {
    if (menu_fd < 0) return;
    for (;;) {
        char tmp[32];
        ssize_t r = read(menu_fd, tmp, sizeof(tmp));
        if (r > 0) {
            for (ssize_t i = 0; i < r && menu_len < (int)sizeof(menu_buf) - 1; i++) menu_buf[menu_len++] = tmp[i];
            menu_buf[menu_len] = '\0';
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        break;
    }
    close(menu_fd);
    menu_fd = -1;
    menu_pid = 0;
    if (menu_len == 0) return;
    int idx = atoi(menu_buf);
    if (idx < 0 || idx >= menu_n) return;
    Client *c = find_client(menu_wins[idx]);
    if (c) activate(c);
}

static void unmanage(Client *c, int destroyed) {
    Monitor *m = c->mon;
    int was_focused = focused == c;
    client_unredirect(c);
    if (!destroyed) {
        XSelectInput(dpy, c->w, NoEventMask);
        XSetWindowBorderWidth(dpy, c->w, 0);
        XReparentWindow(dpy, c->w, root, m->x + c->gx, m->y + c->gy);
    }
    int idx = -1;
    for (int i = 0; i < nclients; i++) if (clients[i] == c) idx = i;
    if (idx >= 0) {
        for (int i = idx; i < nclients - 1; i++) clients[i] = clients[i + 1];
        nclients--;
    }
    for (int k = 0; k < MAX_WS; k++) if (m->sel[k] == c) m->sel[k] = NULL;
    if (focused == c) focused = NULL;
    if (drag_c == c) {
        drag_c = NULL;
        drag_mode = 0;
    }
    free(c);
    layout(m, 1);
    if (m->overview) m->dirty = 1;
    if (was_focused) {
        Client *n = fallback_client(m);
        set_focus(n);
        warp_to(n);
    }
}

static void manage(Window w) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa) || wa.override_redirect) return;
    if (nclients >= MAX_CLIENTS) {
        XMapWindow(dpy, w);
        return;
    }
    Monitor *m = cur_mon();
    View *v = &m->view[m->ws];
    if (m->overview) {
        v->zoom = 1.0;
        overview_end(m);
    }
    Client *c = calloc(1, sizeof(Client));
    if (!c) return;
    c->w = w;
    c->mon = m;
    c->ws = m->ws;
    c->visual = wa.visual;
    c->fw = clampi(default_width, 1, m->w - 2 * border_width);
    c->fh = clampi(default_height, 1, m->h - 2 * border_width);
    int px, py;
    pointer_pos(&px, &py);
    c->fx = (int)lround(v->cx + (px - m->x - m->w / 2.0) - (c->fw + 2.0 * border_width) / 2.0);
    c->fy = (int)lround(v->cy + (py - m->y - m->h / 2.0) - (c->fh + 2.0 * border_width) / 2.0);
    c->z = ++zcounter;
    clients[nclients++] = c;
    XSelectInput(dpy, w, EnterWindowMask | FocusChangeMask | StructureNotifyMask);
    XSetWindowBorderWidth(dpy, w, border_width);
    XSetWindowBorder(dpy, w, color_border);
    if (wa.map_state != IsUnmapped) c->ignore_unmap++;
    XReparentWindow(dpy, w, m->box, 0, 0);
    if (wa.map_state != IsUnmapped) c->mapped = 1;
    layout(m, 1);
    set_focus(c);
    raise_client(c);
    warp_to(c);
}

static void exec_command(const char *cmd) {
    Monitor *m = cur_mon();
    int n;
    if (!strcmp(cmd, "close-window")) {
        if (focused) send_event(focused->w, wm_delete_window);
    } else if (!strcmp(cmd, "alt-tab")) {
        open_menu();
    } else if (!strcmp(cmd, "toggle-tiling")) {
        toggle_tiling(m);
    } else if (!strcmp(cmd, "minimize")) {
        Client *c = current_client(m);
        if (c) toggle_minimize(c);
    } else if (!strcmp(cmd, "fullscreen")) {
        Client *c = current_client(m);
        if (c) set_fullscreen(c, !c->fullscreen);
    } else if (!strcmp(cmd, "focus-left")) {
        focus_dir(0);
    } else if (!strcmp(cmd, "focus-right")) {
        focus_dir(1);
    } else if (!strcmp(cmd, "focus-up")) {
        focus_dir(2);
    } else if (!strcmp(cmd, "focus-down")) {
        focus_dir(3);
    } else if (!strcmp(cmd, "move-left")) {
        move_dir(0);
    } else if (!strcmp(cmd, "move-right")) {
        move_dir(1);
    } else if (!strcmp(cmd, "move-up")) {
        move_dir(2);
    } else if (!strcmp(cmd, "move-down")) {
        move_dir(3);
    } else if (!strcmp(cmd, "zoom-in")) {
        zoom_step(m, 1.15, -1, -1);
    } else if (!strcmp(cmd, "zoom-out")) {
        zoom_step(m, 1.0 / 1.15, -1, -1);
    } else if (!strcmp(cmd, "zoom-reset")) {
        set_zoom(m, 1.0, m->w / 2, m->h / 2);
    } else if (!strncmp(cmd, "workspace ", 10)) {
        n = atoi(cmd + 10);
        switch_ws(m, n - 1);
    } else if (!strncmp(cmd, "move-to-workspace ", 18)) {
        n = atoi(cmd + 18);
        Client *c = current_client(m);
        if (c) move_to_ws(c, n - 1);
    } else if (!strncmp(cmd, "focus-monitor ", 14)) {
        Monitor *t = mon_target(m, cmd + 14);
        if (t) focus_monitor(t);
    } else if (!strncmp(cmd, "send-to-monitor ", 16)) {
        Monitor *t = mon_target(m, cmd + 16);
        Client *c = current_client(m);
        if (t && c && t != m) {
            client_to_monitor(c, t);
            set_focus(c);
            raise_client(c);
            warp_to(c);
        }
    } else {
        spawn(cmd);
    }
}

static void grab_keys(void) {
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    unsigned int ign[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask, 0x2000, 0x2000 | LockMask, 0x2000 | Mod2Mask, 0x2000 | LockMask | Mod2Mask};
    for (int i = 0; i < keys_count; i++) {
        KeyCode code = XKeysymToKeycode(dpy, keys[i].keysym);
        if (!code) continue;
        for (int j = 0; j < 8; j++) XGrabKey(dpy, code, keys[i].mod | ign[j], root, True, GrabModeAsync, GrabModeAsync);
    }
}

static void grab_button_all(unsigned int button, unsigned int mod, unsigned int mask) {
    unsigned int ign[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask, 0x2000, 0x2000 | LockMask, 0x2000 | Mod2Mask, 0x2000 | LockMask | Mod2Mask};
    for (int j = 0; j < 8; j++) XGrabButton(dpy, button, mod | ign[j], root, False, mask, GrabModeAsync, GrabModeAsync, None, None);
}

static void grab_buttons(void) {
    unsigned int full = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    grab_button_all(Button1, Mod1Mask, full);
    grab_button_all(Button3, Mod1Mask, full);
    grab_button_all(Button1, Mod4Mask, full);
    grab_button_all(Button2, Mod4Mask, ButtonPressMask);
    grab_button_all(Button4, Mod4Mask, ButtonPressMask);
    grab_button_all(Button5, Mod4Mask, ButtonPressMask);
}

static void set_ewmh(void) {
    Atom check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    Atom supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    Window wm_window = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, root, check, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&wm_window, 1);
    XChangeProperty(dpy, wm_window, check, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&wm_window, 1);
    XChangeProperty(dpy, wm_window, net_wm_name, utf8_string, 8, PropModeReplace, (unsigned char *)"ghostwm", 7);
    XChangeProperty(dpy, root, net_wm_name, utf8_string, 8, PropModeReplace, (unsigned char *)"ghostwm", 7);
    Atom list[] = {check, supported, net_wm_name, net_wm_state, net_wm_fullscreen};
    XChangeProperty(dpy, root, supported, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, 5);
}

static int cmp_mon(const void *a, const void *b) {
    const MonInfo *x = a, *y = b;
    if (x->x != y->x) return x->x < y->x ? -1 : 1;
    if (x->y != y->y) return x->y < y->y ? -1 : 1;
    return 0;
}

static Monitor *create_monitor(const MonInfo *mi) {
    Monitor *m = calloc(1, sizeof(Monitor));
    if (!m) return NULL;
    snprintf(m->name, sizeof(m->name), "%.63s", mi->name);
    m->x = mi->x;
    m->y = mi->y;
    m->w = mi->w;
    m->h = mi->h;
    m->tiling = 1;
    for (int k = 0; k < MAX_WS; k++) m->view[k].zoom = 1.0;
    m->box = XCreateSimpleWindow(dpy, root, m->x, m->y, m->w, m->h, 0, 0, color_bg);
    XSelectInput(dpy, m->box, SubstructureRedirectMask | SubstructureNotifyMask);
    XSetWindowAttributes sa;
    sa.override_redirect = True;
    sa.background_pixel = color_bg;
    sa.event_mask = ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | ExposureMask;
    m->veil = XCreateWindow(dpy, root, m->x, m->y, m->w, m->h, 0, CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel | CWEventMask, &sa);
    XMapWindow(dpy, m->box);
    XLowerWindow(dpy, m->box);
    return m;
}

static void destroy_monitor(Monitor *m) {
    overview_end(m);
    if (pan_mon == m) {
        pan_mon = NULL;
        panning = 0;
    }
    if (inertia_mon == m) {
        inertia_mon = NULL;
        vel_x = 0;
        vel_y = 0;
    }
    XDestroyWindow(dpy, m->veil);
    XDestroyWindow(dpy, m->box);
    free(m);
}

static void refresh_monitors(void) {
    int n = 0;
    XRRMonitorInfo *xm = XRRGetMonitors(dpy, root, True, &n);
    MonInfo info[MAX_MONITORS];
    int cnt = 0;
    for (int i = 0; xm && i < n && cnt < MAX_MONITORS; i++) {
        if (xm[i].width <= 0 || xm[i].height <= 0) continue;
        int dup = 0;
        for (int j = 0; j < cnt; j++) {
            if (info[j].x == xm[i].x && info[j].y == xm[i].y && info[j].w == xm[i].width && info[j].h == xm[i].height) dup = 1;
        }
        if (dup) continue;
        char *nm = XGetAtomName(dpy, xm[i].name);
        snprintf(info[cnt].name, sizeof(info[cnt].name), "%.63s", nm ? nm : "monitor");
        if (nm) XFree(nm);
        info[cnt].x = xm[i].x;
        info[cnt].y = xm[i].y;
        info[cnt].w = xm[i].width;
        info[cnt].h = xm[i].height;
        cnt++;
    }
    if (xm) XRRFreeMonitors(xm);
    if (cnt == 0) {
        snprintf(info[0].name, sizeof(info[0].name), "screen");
        info[0].x = 0;
        info[0].y = 0;
        info[0].w = DisplayWidth(dpy, screen);
        info[0].h = DisplayHeight(dpy, screen);
        cnt = 1;
    }
    qsort(info, (size_t)cnt, sizeof(info[0]), cmp_mon);

    Monitor *old[MAX_MONITORS];
    int oldn = nmons;
    int used[MAX_MONITORS] = {0};
    memcpy(old, mons, sizeof(Monitor *) * (size_t)oldn);

    Monitor *fresh[MAX_MONITORS];
    for (int i = 0; i < cnt; i++) {
        Monitor *m = NULL;
        for (int j = 0; j < oldn; j++) {
            if (!used[j] && !strcmp(old[j]->name, info[i].name)) {
                m = old[j];
                used[j] = 1;
                break;
            }
        }
        if (m) {
            if (m->x != info[i].x || m->y != info[i].y || m->w != info[i].w || m->h != info[i].h) {
                overview_end(m);
                m->x = info[i].x;
                m->y = info[i].y;
                m->w = info[i].w;
                m->h = info[i].h;
                XMoveResizeWindow(dpy, m->box, m->x, m->y, m->w, m->h);
            }
        } else {
            m = create_monitor(&info[i]);
        }
        fresh[i] = m;
    }
    memcpy(mons, fresh, sizeof(Monitor *) * (size_t)cnt);
    nmons = cnt;

    for (int j = 0; j < oldn; j++) {
        if (used[j]) continue;
        Client *moving[MAX_CLIENTS];
        int nm = 0;
        for (int i = 0; i < nclients; i++) if (clients[i]->mon == old[j]) moving[nm++] = clients[i];
        for (int i = 0; i < nm; i++) client_to_monitor(moving[i], mons[0]);
        destroy_monitor(old[j]);
    }
    for (int i = 0; i < nmons; i++) layout(mons[i], 1);
}

static void apply_config_to_state(void) {
    XSetWindowBackground(dpy, root, color_bg);
    XClearWindow(dpy, root);
    for (int i = 0; i < nmons; i++) {
        Monitor *m = mons[i];
        if (m->ws >= nws) m->ws = nws - 1;
        XSetWindowBackground(dpy, m->box, color_bg);
        XSetWindowBackground(dpy, m->veil, color_bg);
        XClearWindow(dpy, m->box);
    }
    for (int i = 0; i < nclients; i++) {
        Client *c = clients[i];
        if (c->ws >= nws) c->ws = nws - 1;
        XSetWindowBorderWidth(dpy, c->w, c->fullscreen ? 0 : border_width);
        XSetWindowBorder(dpy, c->w, c == focused ? color_focus : color_border);
    }
    for (int i = 0; i < nmons; i++) layout(mons[i], 1);
}

static void end_pan(int rx, int ry) {
    Monitor *m = pan_mon;
    panning = 0;
    if (!m) return;
    if (pan_select && !pan_moved) {
        vel_x = 0;
        vel_y = 0;
        Client *c = client_at(m, rx - m->x, ry - m->y);
        if (c) zoom_to_client(c);
        return;
    }
    if (get_time_ms() - last_move_ms > 60.0) {
        vel_x = 0;
        vel_y = 0;
    }
    vel_x = clampd(vel_x, -MAX_VELOCITY, MAX_VELOCITY);
    vel_y = clampd(vel_y, -MAX_VELOCITY, MAX_VELOCITY);
    inertia_mon = m;
    last_tick_ms = get_time_ms();
    if (fabs(vel_x) < 8.0 && fabs(vel_y) < 8.0) {
        vel_x = 0;
        vel_y = 0;
        if (!m->overview) float_place(m, 1);
    }
}

static void process_event(XEvent *ev) {
    if (have_composite && ev->type == damage_event_base + XDamageNotify) {
        XDamageNotifyEvent *de = (XDamageNotifyEvent *)ev;
        XDamageSubtract(dpy, de->damage, None, None);
        for (int i = 0; i < nclients; i++) {
            if (clients[i]->damage == de->damage) clients[i]->mon->dirty = 1;
        }
        return;
    }
    if (ev->type == rr_event_base + RRScreenChangeNotify) {
        XRRUpdateConfiguration(ev);
        refresh_monitors();
        return;
    }

    switch (ev->type) {
        case MapRequest: {
            Window w = ev->xmaprequest.window;
            Client *c = find_client(w);
            if (c) {
                if (!c->mapped && should_show(c)) layout(c->mon, 1);
            } else {
                manage(w);
            }
            break;
        }
        case ConfigureRequest: {
            XConfigureRequestEvent *e = &ev->xconfigurerequest;
            Client *c = find_client(e->window);
            if (c) {
                send_configure(c);
            } else {
                XWindowChanges wc;
                wc.x = e->x;
                wc.y = e->y;
                wc.width = e->width;
                wc.height = e->height;
                wc.border_width = e->border_width;
                wc.sibling = e->above;
                wc.stack_mode = e->detail;
                XConfigureWindow(dpy, e->window, (unsigned int)e->value_mask, &wc);
            }
            break;
        }
        case UnmapNotify: {
            XUnmapEvent *e = &ev->xunmap;
            if (e->event != e->window) break;
            Client *c = find_client(e->window);
            if (!c) break;
            if (c->ignore_unmap > 0) {
                c->ignore_unmap--;
                break;
            }
            c->mapped = 0;
            unmanage(c, 0);
            break;
        }
        case DestroyNotify: {
            XDestroyWindowEvent *e = &ev->xdestroywindow;
            if (e->event != e->window) break;
            Client *c = find_client(e->window);
            if (c) unmanage(c, 1);
            break;
        }
        case EnterNotify: {
            XCrossingEvent *e = &ev->xcrossing;
            if (e->mode != NotifyNormal || e->detail == NotifyInferior) break;
            Client *c = find_client(e->window);
            if (c && c != focused && c->mapped && should_show(c)) set_focus(c);
            break;
        }
        case Expose: {
            for (int i = 0; i < nmons; i++) if (mons[i]->veil == ev->xexpose.window) mons[i]->dirty = 1;
            break;
        }
        case ClientMessage: {
            XClientMessageEvent *e = &ev->xclient;
            if (e->message_type != net_wm_state) break;
            Client *c = find_client(e->window);
            if (!c) break;
            if ((Atom)e->data.l[1] == net_wm_fullscreen || (Atom)e->data.l[2] == net_wm_fullscreen) {
                long a = e->data.l[0];
                int on = a == 1 ? 1 : (a == 0 ? 0 : !c->fullscreen);
                set_fullscreen(c, on);
            }
            break;
        }
        case ButtonPress: {
            XButtonEvent *b = &ev->xbutton;
            Monitor *m = mon_at(b->x_root, b->y_root);
            int lx = b->x_root - m->x, ly = b->y_root - m->y;
            int mod4 = (b->state & Mod4Mask) != 0, mod1 = (b->state & Mod1Mask) != 0;
            vel_x = 0;
            vel_y = 0;
            if (b->button == Button4 || b->button == Button5) {
                if (mod4 || m->overview) zoom_step(m, b->button == Button4 ? 1.12 : 1.0 / 1.12, lx, ly);
            } else if (b->button == Button2) {
                if (mod4) set_zoom(m, 1.0, lx, ly);
            } else if (b->button == Button1 && (mod4 || m->overview) && !m->tiling) {
                panning = 1;
                pan_mon = m;
                pan_select = m->overview && !mod4;
                pan_moved = 0;
                pan_x = pan_start_x = b->x_root;
                pan_y = pan_start_y = b->y_root;
                last_move_ms = get_time_ms();
            } else if (mod1 && !m->overview && !m->tiling && (b->button == Button1 || b->button == Button3)) {
                Client *c = client_at(m, lx, ly);
                if (c) {
                    View *v = &m->view[m->ws];
                    set_focus(c);
                    raise_client(c);
                    drag_c = c;
                    drag_mode = b->button == Button1 ? 1 : 2;
                    drag_px = b->x_root;
                    drag_py = b->y_root;
                    drag_fx = c->fx;
                    drag_fy = c->fy;
                    drag_fw = c->fw;
                    drag_fh = c->fh;
                    drag_ox = v->cx + (lx - m->w / 2.0) - c->fx;
                    drag_oy = v->cy + (ly - m->h / 2.0) - c->fy;
                }
            }
            break;
        }
        case ButtonRelease: {
            XButtonEvent *b = &ev->xbutton;
            if (b->button == Button1 && panning) end_pan(b->x_root, b->y_root);
            if (drag_mode && (b->button == Button1 || b->button == Button3)) {
                Monitor *m = drag_c ? drag_c->mon : NULL;
                drag_mode = 0;
                drag_c = NULL;
                if (m && !m->overview) float_place(m, 1);
            }
            break;
        }
        case MotionNotify: {
            while (XCheckTypedEvent(dpy, MotionNotify, ev)) {
            }
            int rx = ev->xmotion.x_root, ry = ev->xmotion.y_root;
            if (panning && pan_mon) {
                int dx = rx - pan_x, dy = ry - pan_y;
                if (dx || dy) {
                    double now = get_time_ms();
                    double dt = now - last_move_ms;
                    if (dt > 0.5) {
                        vel_x = 0.5 * vel_x + 0.5 * (dx / (dt / 1000.0));
                        vel_y = 0.5 * vel_y + 0.5 * (dy / (dt / 1000.0));
                    }
                    last_move_ms = now;
                    pan_x = rx;
                    pan_y = ry;
                    if (abs(rx - pan_start_x) + abs(ry - pan_start_y) > 5) pan_moved = 1;
                    pan_apply(pan_mon, dx, dy);
                }
            } else if (drag_mode && drag_c) {
                Client *c = drag_c;
                Monitor *m = c->mon;
                Monitor *mp = mon_at(rx, ry);
                if (drag_mode == 1) {
                    if (mp != m && !mp->tiling) {
                        client_to_monitor(c, mp);
                        m = mp;
                        View *v = &m->view[m->ws];
                        c->fx = (int)lround(v->cx + (rx - m->x - m->w / 2.0) - drag_ox);
                        c->fy = (int)lround(v->cy + (ry - m->y - m->h / 2.0) - drag_oy);
                        drag_px = rx;
                        drag_py = ry;
                        drag_fx = c->fx;
                        drag_fy = c->fy;
                        layout(m, 1);
                    } else if (!m->tiling) {
                        c->fx = drag_fx + (rx - drag_px);
                        c->fy = drag_fy + (ry - drag_py);
                        float_update(c, PL_MOVE);
                    }
                } else if (!m->tiling) {
                    c->fw = clampi(drag_fw + (rx - drag_px), 100, 16000);
                    c->fh = clampi(drag_fh + (ry - drag_py), 100, 16000);
                    float_update(c, 1);
                }
            }
            break;
        }
        case KeyPress: {
            KeySym keysym = XLookupKeysym(&ev->xkey, 0);
            unsigned int mod = ev->xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
            for (int i = 0; i < keys_count; i++) {
                if (keysym == keys[i].keysym && mod == keys[i].mod) exec_command(keys[i].cmd);
            }
            break;
        }
        default:
            break;
    }
}

static int x_error_handler(Display *d, XErrorEvent *ee) {
    (void)d;
    (void)ee;
    return 0;
}

static int x_error_detect(Display *d, XErrorEvent *ee) {
    (void)d;
    (void)ee;
    wm_detected = 1;
    return 0;
}

int main(void) {
    signal(SIGUSR1, handle_sigusr1);
    signal(SIGPIPE, SIG_IGN);
    load_config();

    if (!(dpy = XOpenDisplay(NULL))) return 1;
    fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);
    screen = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    XSetErrorHandler(x_error_detect);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(dpy, False);
    if (wm_detected) {
        fprintf(stderr, "ghostwm: another window manager is already running\n");
        return 1;
    }
    XSetErrorHandler(x_error_handler);

    wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    net_wm_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);

    int err;
    int comp_ev;
    have_composite = XCompositeQueryExtension(dpy, &comp_ev, &err) && XDamageQueryExtension(dpy, &damage_event_base, &err)
                     && XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen)) != NULL;
    if (!have_composite) fprintf(stderr, "ghostwm: Composite/Damage/Render unavailable, zoom disabled\n");
    if (!XRRQueryExtension(dpy, &rr_event_base, &err)) {
        fprintf(stderr, "ghostwm: RandR is required\n");
        return 1;
    }

    run_setup();
    signal(SIGCHLD, SIG_IGN);
    XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);

    XSetWindowBackground(dpy, root, color_bg);
    XClearWindow(dpy, root);
    XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_dot));
    set_ewmh();

    refresh_monitors();
    grab_keys();
    grab_buttons();
    run_autostart();
    XWarpPointer(dpy, None, root, 0, 0, 0, 0, mons[0]->x + mons[0]->w / 2, mons[0]->y + mons[0]->h / 2);
    XSync(dpy, False);

    for (;;) {
        if (reload_requested) {
            reload_requested = 0;
            load_config();
            run_setup();
            refresh_monitors();
            apply_config_to_state();
            grab_keys();
            run_autostart();
        }

        XEvent ev;
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            process_event(&ev);
        }

        double now = get_time_ms();
        int animating = 0;

        if (!panning && inertia_mon && (fabs(vel_x) >= 8.0 || fabs(vel_y) >= 8.0)) {
            double dt = (now - last_tick_ms) / 1000.0;
            last_tick_ms = now;
            if (dt > 0.1) dt = 0.1;
            pan_apply(inertia_mon, vel_x * dt, vel_y * dt);
            double decay = pow(0.04, dt);
            vel_x *= decay;
            vel_y *= decay;
            if (fabs(vel_x) < 8.0 && fabs(vel_y) < 8.0) {
                vel_x = 0;
                vel_y = 0;
                if (!inertia_mon->overview) float_place(inertia_mon, 1);
            } else {
                animating = 1;
            }
        }

        for (int i = 0; i < nmons; i++) {
            if (mons[i]->overview && mons[i]->dirty) {
                if (now - last_paint_ms >= 10.0) paint(mons[i]);
                if (mons[i]->dirty) animating = 1;
            }
        }
        if (now - last_paint_ms >= 10.0) last_paint_ms = now;

        XFlush(dpy);
        if (XPending(dpy)) continue;

        struct pollfd pfd[2];
        int np = 0;
        pfd[np].fd = ConnectionNumber(dpy);
        pfd[np].events = POLLIN;
        np++;
        if (menu_fd >= 0) {
            pfd[np].fd = menu_fd;
            pfd[np].events = POLLIN;
            np++;
        }
        int rc = poll(pfd, (nfds_t)np, animating ? 8 : -1);
        if (rc > 0 && menu_fd >= 0 && np == 2 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) menu_poll_result();
    }

    XCloseDisplay(dpy);
    return 0;
}
