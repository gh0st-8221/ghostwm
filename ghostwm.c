#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>

#define MAX_VELOCITY 1500.0
#define MAX_KEYS 128
#define MAX_AUTOSTART 64

int border_width = 1;
unsigned long color_bg = 0x101010;
unsigned long color_fg = 0xffffff;
unsigned long color_border = 0x303030;
unsigned long color_focus = 0x0000ff;
int default_width = 1920;
int default_height = 1080;
char hdmi_pos[64] = "2560x310";
char dp_pos[64] = "0x0";

struct KeyBinding {
    unsigned int mod;
    KeySym keysym;
    char *cmd;
};

struct KeyBinding keys[MAX_KEYS];
int keys_count = 0;
char *autostart_cmds[MAX_AUTOSTART];
int autostart_count = 0;

typedef struct WinState {
    Window w;
    int x, y;
    unsigned int width, height;
    int is_minimized;
    struct WinState *next;
} WinState;

WinState *saved_states = NULL;
XWindowAttributes start_attr;
Atom wm_delete_window;
Atom wm_protocols;
Window focused_window = None;

double zoom_factor = 1.0;
int is_panning = 0;
int is_tiling = 1;
double vel_x = 0.0;
double vel_y = 0.0;
struct timeval last_motion_time;
int last_dx = 0;
int last_dy = 0;
volatile sig_atomic_t reload_requested = 0;

void handle_sigusr1(int sig) {
    (void)sig;
    reload_requested = 1;
}

void parse_key_str(char *kstr, char *cmd) {
    if (keys_count >= MAX_KEYS) return;
    unsigned int mod = 0;
    if (strstr(kstr, "Mod4")) mod |= Mod4Mask;
    if (strstr(kstr, "Mod1")) mod |= Mod1Mask;
    if (strstr(kstr, "Shift")) mod |= ShiftMask;
    if (strstr(kstr, "Control")) mod |= ControlMask;
    
    char *plus = strrchr(kstr, '+');
    char *k = plus ? plus + 1 : kstr;
    while (*k == ' ') k++;
    
    KeySym sym;
    if (strstr(k, "Print")) sym = XK_Print;
    else if (strcmp(k, "Left") == 0) sym = XK_Left;
    else if (strcmp(k, "Right") == 0) sym = XK_Right;
    else if (strcmp(k, "Up") == 0) sym = XK_Up;
    else if (strcmp(k, "Down") == 0) sym = XK_Down;
    else if (strcmp(k, "Tab") == 0) sym = XK_Tab;
    else if (strcmp(k, "minus") == 0) sym = XK_minus;
    else if (strcmp(k, "equal") == 0) sym = XK_equal;
    else {
        char clean_k[64] = {0};
        int j = 0;
        for (int i = 0; k[i] && j < 63; i++) {
            if (k[i] != ' ' && k[i] != '\t' && k[i] != '\n' && k[i] != '\r') clean_k[j++] = k[i];
        }
        if (clean_k[0] >= 'A' && clean_k[0] <= 'Z' && strlen(clean_k) == 1) clean_k[0] += 32;
        sym = XStringToKeysym(clean_k);
        if (sym == NoSymbol && strlen(clean_k) == 1) sym = clean_k[0];
    }
    
    if (sym != NoSymbol) {
        keys[keys_count].mod = mod;
        keys[keys_count].keysym = sym;
        keys[keys_count].cmd = strdup(cmd);
        keys_count++;
    }
}

void load_config(void) {
    for (int i = 0; i < keys_count; i++) free(keys[i].cmd);
    keys_count = 0;
    for (int i = 0; i < autostart_count; i++) free(autostart_cmds[i]);
    autostart_count = 0;

    char path[1024];
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return;

    snprintf(path, sizeof(path), "%s/.config/ghostwm/config.toml", home);
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), fp)) {
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (*start == '#' || *start == '\n' || *start == '\r' || *start == '\0') continue;

        if (*start == '[') {
            char *end = strchr(start, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", start + 1);
            }
            continue;
        }

        char *eq = strchr(start, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = start;
        char *val = eq + 1;

        char *k_end = key + strlen(key) - 1;
        while (k_end > key && (*k_end == ' ' || *k_end == '\t' || *k_end == '\r' || *k_end == '\n')) *k_end-- = '\0';

        while (*val == ' ' || *val == '\t') val++;
        char *v_end = val + strlen(val) - 1;
        while (v_end > val && (*v_end == ' ' || *v_end == '\t' || *v_end == '\r' || *v_end == '\n')) *v_end-- = '\0';

        int is_quoted = 0;
        if ((*val == '"' && *v_end == '"') || (*val == '\'' && *v_end == '\'')) {
            val++;
            *v_end = '\0';
            v_end--;
            is_quoted = 1;
        }

        if (strcmp(section, "general") == 0) {
            if (strcmp(key, "border_width") == 0) border_width = atoi(val);
            else if (strcmp(key, "default_width") == 0) default_width = atoi(val);
            else if (strcmp(key, "default_height") == 0) default_height = atoi(val);
        } else if (strcmp(section, "colors") == 0 && is_quoted) {
            if (strcmp(key, "bg") == 0) color_bg = strtoul(val, NULL, 0);
            else if (strcmp(key, "fg") == 0) color_fg = strtoul(val, NULL, 0);
            else if (strcmp(key, "border") == 0) color_border = strtoul(val, NULL, 0);
            else if (strcmp(key, "focus") == 0) color_focus = strtoul(val, NULL, 0);
        } else if (strcmp(section, "monitors") == 0 && is_quoted) {
            if (strcmp(key, "hdmi_pos") == 0) snprintf(hdmi_pos, sizeof(hdmi_pos), "%s", val);
            else if (strcmp(key, "dp_pos") == 0) snprintf(dp_pos, sizeof(dp_pos), "%s", val);
        } else if (strcmp(section, "autostart") == 0 && is_quoted) {
            if (strcmp(key, "cmd") == 0 && autostart_count < MAX_AUTOSTART) {
                autostart_cmds[autostart_count++] = strdup(val);
            }
        } else if (strcmp(section, "keys") == 0 && is_quoted) {
            parse_key_str(key, val);
        }
    }
    fclose(fp);
    parse_key_str("Mod4+f", "fullscreen");
}

void save_window_state(Window w, int x, int y, unsigned int width, unsigned int height) {
    WinState *curr = saved_states;
    while (curr) {
        if (curr->w == w) return;
        curr = curr->next;
    }
    WinState *node = malloc(sizeof(WinState));
    if (!node) return;
    node->w = w;
    node->x = x;
    node->y = y;
    node->width = width;
    node->height = height;
    node->is_minimized = 0;
    node->next = saved_states;
    saved_states = node;
}

WinState* get_window_state(Window w) {
    WinState *curr = saved_states;
    while (curr) {
        if (curr->w == w) return curr;
        curr = curr->next;
    }
    return NULL;
}

void remove_window_state(Window w) {
    WinState **curr = &saved_states;
    while (*curr) {
        if ((*curr)->w == w) {
            WinState *tmp = *curr;
            *curr = (*curr)->next;
            free(tmp);
            return;
        }
        curr = &((*curr)->next);
    }
}

double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

void setup_monitors(void) {
    FILE *fp = popen("xrandr --query", "r");
    if (!fp) return;
    char line[256], edp_name[64] = {0}, hdmi_name[64] = {0}, dp_name[64] = {0};
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "eDP", 3) == 0 && strstr(line, " connected")) sscanf(line, "%63s", edp_name);
        if (strncmp(line, "HDMI", 4) == 0 && strstr(line, " connected")) sscanf(line, "%63s", hdmi_name);
        if (strncmp(line, "DP", 2) == 0 && strstr(line, " connected")) sscanf(line, "%63s", dp_name);
    }
    pclose(fp);
    char cmd[1024] = "xrandr";
    int run = 0;
    if (edp_name[0] != '\0') {
        snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " --output %s --mode 1920x1080 --rate 60 --auto", edp_name);
        run = 1;
    }
    if (hdmi_name[0] != '\0') {
        snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " --output %s --pos %s --auto", hdmi_name, hdmi_pos);
        run = 1;
    }
    if (dp_name[0] != '\0') {
        snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " --output %s --pos %s --auto", dp_name, dp_pos);
        run = 1;
    }
    if (run) system(cmd);
}

void spawn(const char *cmd) {
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        exit(0);
    }
}

void run_autostart(void) {
    for (int i = 0; i < autostart_count; i++) {
        if (autostart_cmds[i]) spawn(autostart_cmds[i]);
    }
}

void send_event(Display *dpy, Window w, Atom proto) {
    int n, exists = 0;
    Atom *protocols = NULL;
    if (XGetWMProtocols(dpy, w, &protocols, &n)) {
        while (--n >= 0) {
            if (protocols[n] == proto) { exists = 1; break; }
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
        ev.xclient.data.l[0] = wm_delete_window;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, w, False, NoEventMask, &ev);
    } else {
        XKillClient(dpy, w);
    }
}

void warp_to_window(Display *dpy, Window root, Window w) {
    if (w == None || w == root) return;
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, w, &wa)) {
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, wa.x + wa.width / 2, wa.y + wa.height / 2);
    }
}

void set_focus(Display *dpy, Window w) {
    if (focused_window != None && focused_window != DefaultRootWindow(dpy)) {
        XSetWindowBorder(dpy, focused_window, color_border);
    }
    focused_window = w;
    if (focused_window != None && focused_window != DefaultRootWindow(dpy)) {
        XSetInputFocus(dpy, focused_window, RevertToParent, CurrentTime);
        XSetWindowBorder(dpy, focused_window, color_focus);
    }
}

void apply_tiling(Display *dpy, Window root) {
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren;
    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        Window viewable[256];
        int count = 0;
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, children[i], &wa) && wa.map_state == IsViewable && !wa.override_redirect) {
                if (count < 256) viewable[count++] = children[i];
            }
        }
        if (count > 0) {
            int screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
            int screen_h = DisplayHeight(dpy, DefaultScreen(dpy));
            int half_w = screen_w / 2;

            for (int idx = 0; idx < count; idx++) {
                int x, y, w, h;
                if (count == 1) {
                    x = 0; y = 0; w = screen_w - (border_width * 2); h = screen_h - (border_width * 2);
                } else if (count == 2) {
                    x = (idx == 0) ? 0 : half_w; y = 0; w = half_w - (border_width * 2); h = screen_h - (border_width * 2);
                } else {
                    if (idx == 0) {
                        x = 0; y = 0; w = half_w - (border_width * 2); h = screen_h - (border_width * 2);
                    } else {
                        int sub_h = screen_h / (count - 1);
                        x = half_w; y = (idx - 1) * sub_h; w = half_w - (border_width * 2); h = sub_h - (border_width * 2);
                    }
                }
                XMoveResizeWindow(dpy, viewable[idx], x, y, w, h);
                WinState *st = get_window_state(viewable[idx]);
                if (st) { st->x = x; st->y = y; st->width = w; st->height = h; }
            }
        }
        if (children) XFree(children);
    }
}

void toggle_minimize(Display *dpy, Window root, Window w) {
    if (w == None || w == DefaultRootWindow(dpy)) return;
    WinState *st = get_window_state(w);
    if (!st) return;

    if (st->is_minimized) {
        st->is_minimized = 0;
        XMapWindow(dpy, w);
        if (is_tiling) apply_tiling(dpy, root);
        XRaiseWindow(dpy, w);
        set_focus(dpy, w);
    } else {
        st->is_minimized = 1;
        XUnmapWindow(dpy, w);
        if (focused_window == w) set_focus(dpy, None);
        if (is_tiling) apply_tiling(dpy, root);
    }
}

void alt_tab(Display *dpy, Window root) {
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren;
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) return;

    char window_list[4096] = "";
    int count = 0;

    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);

    for (unsigned int i = 0; i < nchildren && count < 256; i++) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, children[i], &wa) && wa.map_state == IsViewable && !wa.override_redirect) {
            char *name = NULL;
            XTextProperty prop;
            if (XGetTextProperty(dpy, children[i], &prop, net_wm_name) && prop.value) {
                name = (char *)prop.value;
            } else if (XGetWMName(dpy, children[i], &prop) && prop.value) {
                name = (char *)prop.value;
            }

            char line[256];
            snprintf(line, sizeof(line), "%s\n", name ? name : "Unnamed");
            strncat(window_list, line, sizeof(window_list) - strlen(window_list) - 1);
            if (name) XFree(prop.value);
            count++;
        }
    }
    if (children) XFree(children);
    if (count == 0) return;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "echo \"%s\" | rofi -dmenu -p \"Windows\"", window_list);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    char selected[256];
    if (fgets(selected, sizeof(selected), fp)) {
        selected[strcspn(selected, "\n")] = 0;
    }
    pclose(fp);

    if (selected[0] == '\0') return;

    children = NULL;
    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            char *name = NULL;
            XTextProperty prop;
            if (XGetTextProperty(dpy, children[i], &prop, net_wm_name) && prop.value) {
                name = (char *)prop.value;
            } else if (XGetWMName(dpy, children[i], &prop) && prop.value) {
                name = (char *)prop.value;
            }
            if (name && strcmp(name, selected) == 0) {
                if (name) XFree(prop.value);
                WinState *st = get_window_state(children[i]);
                if (st && st->is_minimized) {
                    st->is_minimized = 0;
                    XMapWindow(dpy, children[i]);
                    if (is_tiling) apply_tiling(dpy, root);
                }
                XRaiseWindow(dpy, children[i]);
                set_focus(dpy, children[i]);
                warp_to_window(dpy, root, children[i]);
                break;
            }
            if (name) XFree(prop.value);
        }
        if (children) XFree(children);
    }
}

void apply_zoom(Display *dpy, Window root) {
    if (is_tiling) { apply_tiling(dpy, root); return; }
    int center_x = DisplayWidth(dpy, DefaultScreen(dpy)) / 2;
    int center_y = DisplayHeight(dpy, DefaultScreen(dpy)) / 2;
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren;
    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, children[i], &wa) && wa.map_state == IsViewable && !wa.override_redirect) {
                WinState *st = get_window_state(children[i]);
                if (!st) continue;
                if (zoom_factor == 1.0) {
                    XMoveResizeWindow(dpy, children[i], st->x, st->y, st->width, st->height);
                } else {
                    int new_w = (int)(st->width * zoom_factor);
                    int new_h = (int)(st->height * zoom_factor);
                    if (new_w < 50) new_w = 50;
                    if (new_h < 50) new_h = 50;
                    int new_center_x = center_x + (int)(((st->x + (st->width / 2)) - center_x) * zoom_factor);
                    int new_center_y = center_y + (int)(((st->y + (st->height / 2)) - center_y) * zoom_factor);
                    XMoveResizeWindow(dpy, children[i], new_center_x - (new_w / 2), new_center_y - (new_h / 2), new_w, new_h);
                }
            }
        }
        if (children) XFree(children);
    }
}

void tile_move(Display *dpy, Window root, int direction) {
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren;
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) return;
    Window viewable[256];
    int v_count = 0;
    for (unsigned int i = 0; i < nchildren; i++) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, children[i], &wa) && wa.map_state == IsViewable && !wa.override_redirect) {
            if (v_count < 256) viewable[v_count++] = children[i];
        }
    }
    if (v_count < 2) { if (children) XFree(children); return; }
    int focused_idx = -1;
    for (int i = 0; i < v_count; i++) {
        if (viewable[i] == focused_window) { focused_idx = i; break; }
    }
    if (focused_idx == -1) { if (children) XFree(children); return; }
    int target_idx = focused_idx + direction;
    if (target_idx < 0 || target_idx >= v_count) { if (children) XFree(children); return; }
    Window winA = viewable[focused_idx], winB = viewable[target_idx];
    Window *t2b = malloc(nchildren * sizeof(Window));
    if (!t2b) { if (children) XFree(children); return; }
    for (unsigned int i = 0; i < nchildren; i++) t2b[i] = children[nchildren - 1 - i];
    int posA = -1, posB = -1;
    for (unsigned int i = 0; i < nchildren; i++) {
        if (t2b[i] == winA) posA = i;
        if (t2b[i] == winB) posB = i;
    }
    if (posA != -1 && posB != -1) {
        Window temp = t2b[posA]; t2b[posA] = t2b[posB]; t2b[posB] = temp;
        XRestackWindows(dpy, t2b, nchildren);
    }
    free(t2b);
    if (children) XFree(children);
    apply_tiling(dpy, root);
    set_focus(dpy, winA);
    warp_to_window(dpy, root, winA);
}

void focus_direction(Display *dpy, Window root, int direction) {
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren;
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) return;
    Window viewable[256];
    int v_count = 0;
    for (unsigned int i = 0; i < nchildren; i++) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, children[i], &wa) && wa.map_state == IsViewable && !wa.override_redirect) {
            if (v_count < 256) viewable[v_count++] = children[i];
        }
    }
    if (v_count == 0) { if (children) XFree(children); return; }
    int focused_idx = 0;
    for (int i = 0; i < v_count; i++) {
        if (viewable[i] == focused_window) { focused_idx = i; break; }
    }
    int target_idx = focused_idx + direction;
    if (target_idx < 0) target_idx = v_count - 1;
    if (target_idx >= v_count) target_idx = 0;
    Window target_win = viewable[target_idx];
    set_focus(dpy, target_win);
    warp_to_window(dpy, root, target_win);
    if (children) XFree(children);
}

void float_move(Display *dpy, Window root, int dx, int dy) {
    if (focused_window == None || focused_window == DefaultRootWindow(dpy)) return;
    WinState *st = get_window_state(focused_window);
    if (!st) return;
    st->x += dx; st->y += dy;
    XMoveWindow(dpy, focused_window, st->x, st->y);
    warp_to_window(dpy, root, focused_window);
}

void grab_keys(Display *dpy, Window root) {
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    unsigned int ign[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask, 0x2000, 0x2000 | LockMask, 0x2000 | Mod2Mask, 0x2000 | LockMask | Mod2Mask};
    for (int i = 0; i < keys_count; i++) {
        KeyCode code = XKeysymToKeycode(dpy, keys[i].keysym);
        if (code) {
            for (int j = 0; j < 8; j++) XGrabKey(dpy, code, keys[i].mod | ign[j], root, True, GrabModeAsync, GrabModeAsync);
        }
    }
}

void grab_buttons(Display *dpy, Window root) {
    XGrabButton(dpy, Button1, Mod1Mask, root, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, Mod1Mask, root, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button1, Mod4Mask, root, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button2, Mod4Mask, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button4, Mod4Mask, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button5, Mod4Mask, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
}

void set_ewmh_wm_name(Display *dpy, Window root) {
    Atom net_supporting_wm_check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    Window wm_window = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, root, net_supporting_wm_check, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&wm_window, 1);
    XChangeProperty(dpy, wm_window, net_supporting_wm_check, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&wm_window, 1);
    XChangeProperty(dpy, wm_window, net_wm_name, utf8_string, 8, PropModeReplace, (unsigned char *)"ghostwm", 7);
    XChangeProperty(dpy, root, net_wm_name, utf8_string, 8, PropModeReplace, (unsigned char *)"ghostwm", 7);
}

int x_error_handler(Display *dpy, XErrorEvent *ee) {
    (void)dpy;
    (void)ee;
    return 0;
}

void pan_viewport(Display *dpy, Window root, int dx, int dy) {
    if (is_tiling) return;
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren;
    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, children[i], &wa) && wa.map_state == IsViewable) {
                XMoveWindow(dpy, children[i], wa.x - dx, wa.y - dy);
                WinState *st = get_window_state(children[i]);
                if (st) { st->x -= dx; st->y -= dy; }
            }
        }
        if (children) XFree(children);
    }
}

void process_xevent(Display *dpy, Window root, XEvent *ev, XButtonEvent *det_cursor) {
    switch (ev->type) {
        case MapRequest: {
            XSelectInput(dpy, ev->xmap.window, EnterWindowMask | FocusChangeMask | StructureNotifyMask);
            int rx, ry, wx, wy, spawn_x = 0, spawn_y = 0;
            unsigned int mask;
            Window r_ret, c_ret;
            int scr_w = DisplayWidth(dpy, DefaultScreen(dpy));
            int scr_h = DisplayHeight(dpy, DefaultScreen(dpy));

            if (XQueryPointer(dpy, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask)) {
                spawn_x = rx - (default_width / 2); spawn_y = ry - (default_height / 2);
            } else {
                spawn_x = (scr_w - default_width) / 2; spawn_y = (scr_h - default_height) / 2;
            }

            XMoveResizeWindow(dpy, ev->xmap.window, spawn_x, spawn_y, default_width, default_height);
            save_window_state(ev->xmap.window, spawn_x, spawn_y, default_width, default_height);
            XMapWindow(dpy, ev->xmap.window);
            XSetWindowBorderWidth(dpy, ev->xmap.window, border_width);
            set_focus(dpy, ev->xmap.window);
            warp_to_window(dpy, root, ev->xmap.window);

            if (is_tiling) apply_tiling(dpy, root);
            break;
        }
        case UnmapNotify: {
            Window w = ev->xunmap.window;
            WinState *st = get_window_state(w);
            if (st && st->is_minimized) {
                if (w == focused_window) set_focus(dpy, None);
                break;
            }
            remove_window_state(w);
            if (w == focused_window) set_focus(dpy, None);
            if (is_tiling) apply_tiling(dpy, root);
            break;
        }
        case DestroyNotify: {
            Window w = ev->xdestroywindow.window;
            remove_window_state(w);
            if (w == focused_window) set_focus(dpy, None);
            if (is_tiling) apply_tiling(dpy, root);
            break;
        }
        case EnterNotify: { set_focus(dpy, ev->xcrossing.window); break; }
        case ButtonPress: {
            vel_x = 0.0; vel_y = 0.0;
            if ((ev->xbutton.state & Mod4Mask)) {
                if (ev->xbutton.button == Button1) {
                    is_panning = 1; gettimeofday(&last_motion_time, NULL); last_dx = 0; last_dy = 0;
                } else if (ev->xbutton.button == Button5) {
                    if (!is_tiling) { zoom_factor -= 0.1; if (zoom_factor < 0.2) zoom_factor = 0.2; apply_zoom(dpy, root); }
                    break;
                } else if (ev->xbutton.button == Button4) {
                    if (!is_tiling) { zoom_factor += 0.1; if (zoom_factor > 2.5) zoom_factor = 2.5; apply_zoom(dpy, root); }
                    break;
                } else if (ev->xbutton.button == Button2) {
                    zoom_factor = 1.0; apply_zoom(dpy, root); break;
                }
            }
            if (ev->xbutton.subwindow != None) {
                set_focus(dpy, ev->xbutton.subwindow);
                XGetWindowAttributes(dpy, focused_window, &start_attr);
                *det_cursor = ev->xbutton;
                XRaiseWindow(dpy, focused_window);
            } else *det_cursor = ev->xbutton;
            break;
        }
        case ButtonRelease: {
            if (ev->xbutton.button == Button1 && is_panning) {
                is_panning = 0;
                double now = get_time_ms();
                double last_time = (double)last_motion_time.tv_sec * 1000.0 + (double)last_motion_time.tv_usec / 1000.0;
                double dt = now - last_time;
                if (dt < 50.0 && dt > 0.0) {
                    vel_x = (double)last_dx / (dt / 1000.0); vel_y = (double)last_dy / (dt / 1000.0);
                    if (vel_x > MAX_VELOCITY) vel_x = MAX_VELOCITY;
                    if (vel_x < -MAX_VELOCITY) vel_x = -MAX_VELOCITY;
                    if (vel_y > MAX_VELOCITY) vel_y = MAX_VELOCITY;
                    if (vel_y < -MAX_VELOCITY) vel_y = -MAX_VELOCITY;
                } else { vel_x = 0.0; vel_y = 0.0; }
            }
            break;
        }
        case MotionNotify: {
            while (XCheckTypedEvent(dpy, MotionNotify, ev));
            int xdiff = ev->xmotion.x_root - det_cursor->x_root;
            int ydiff = ev->xmotion.y_root - det_cursor->y_root;
            if ((ev->xmotion.state & Mod4Mask) && det_cursor->button == Button1) {
                if (!is_tiling) {
                    pan_viewport(dpy, root, -xdiff, -ydiff);
                    last_dx = -xdiff; last_dy = -ydiff; gettimeofday(&last_motion_time, NULL);
                }
                det_cursor->x_root = ev->xmotion.x_root; det_cursor->y_root = ev->xmotion.y_root;
            } else if ((ev->xmotion.state & Mod1Mask) && focused_window != None && !is_tiling) {
                if (det_cursor->button == Button1) {
                    int nx = start_attr.x + xdiff, ny = start_attr.y + ydiff;
                    XMoveWindow(dpy, focused_window, nx, ny);
                    WinState *st = get_window_state(focused_window);
                    if (st) { st->x = nx; st->y = ny; }
                } else if (det_cursor->button == Button3) {
                    int new_w = start_attr.width + xdiff, new_h = start_attr.height + ydiff;
                    if (new_w < 100) new_w = 100;
                    if (new_h < 100) new_h = 100;
                    XResizeWindow(dpy, focused_window, new_w, new_h);
                    WinState *st = get_window_state(focused_window);
                    if (st) { st->width = new_w; st->height = new_h; }
                }
            }
            break;
        }
        case KeyPress: {
            KeySym keysym = XLookupKeysym(&ev->xkey, 0);
            unsigned int mod = ev->xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);

            for (int i = 0; i < keys_count; i++) {
                if (keysym == keys[i].keysym && mod == keys[i].mod) {
                    if (strcmp(keys[i].cmd, "close-window") == 0) {
                        if (focused_window != None && focused_window != root) send_event(dpy, focused_window, wm_delete_window);
                    } else if (strcmp(keys[i].cmd, "alt-tab") == 0) {
                        alt_tab(dpy, root);
                    } else if (strcmp(keys[i].cmd, "toggle-tiling") == 0) {
                        is_tiling = !is_tiling; apply_tiling(dpy, root);
                    } else if (strcmp(keys[i].cmd, "minimize") == 0) {
                        toggle_minimize(dpy, root, focused_window);
                    } else if (strcmp(keys[i].cmd, "fullscreen") == 0) {
                        if (focused_window != None && focused_window != root) {
                            Window root_r, parent_r, *childs = NULL;
                            unsigned int nc;
                            if (XQueryTree(dpy, root, &root_r, &parent_r, &childs, &nc)) {
                                for (unsigned int c = 0; c < nc; c++) {
                                    if (childs[c] != focused_window && childs[c] != root) {
                                        WinState *s = get_window_state(childs[c]);
                                        if (s && !s->is_minimized) {
                                            s->is_minimized = 1;
                                            XUnmapWindow(dpy, childs[c]);
                                        }
                                    }
                                }
                                if (childs) XFree(childs);
                            }
                            int sw = DisplayWidth(dpy, DefaultScreen(dpy));
                            int sh = DisplayHeight(dpy, DefaultScreen(dpy));
                            XMoveResizeWindow(dpy, focused_window, 0, 0, sw, sh);
                            XRaiseWindow(dpy, focused_window);
                            WinState *fs = get_window_state(focused_window);
                            if (fs) { fs->x = 0; fs->y = 0; fs->width = sw; fs->height = sh; }
                        }
                    } else if (strcmp(keys[i].cmd, "focus-left") == 0) { focus_direction(dpy, root, -1);
                    } else if (strcmp(keys[i].cmd, "focus-right") == 0) { focus_direction(dpy, root, 1);
                    } else if (strcmp(keys[i].cmd, "focus-up") == 0) { focus_direction(dpy, root, -1);
                    } else if (strcmp(keys[i].cmd, "focus-down") == 0) { focus_direction(dpy, root, 1);
                    } else if (strcmp(keys[i].cmd, "move-left") == 0) {
                        if (is_tiling) tile_move(dpy, root, -1); else float_move(dpy, root, -50, 0);
                    } else if (strcmp(keys[i].cmd, "move-right") == 0) {
                        if (is_tiling) tile_move(dpy, root, 1); else float_move(dpy, root, 50, 0);
                    } else if (strcmp(keys[i].cmd, "move-up") == 0) {
                        if (is_tiling) tile_move(dpy, root, -1); else float_move(dpy, root, 0, -50);
                    } else if (strcmp(keys[i].cmd, "move-down") == 0) {
                        if (is_tiling) tile_move(dpy, root, 1); else float_move(dpy, root, 0, 50);
                    } else if (strcmp(keys[i].cmd, "workspace_1") == 0) {
                        if (focused_window != None && focused_window != root) {
                            int dp_x = 0, dp_y = 0; sscanf(dp_pos, "%dx%d", &dp_x, &dp_y);
                            int new_w = DisplayWidth(dpy, DefaultScreen(dpy)) - (border_width * 2);
                            int new_h = DisplayHeight(dpy, DefaultScreen(dpy)) - (border_width * 2);
                            XMoveResizeWindow(dpy, focused_window, dp_x, dp_y, new_w, new_h);
                            WinState *st = get_window_state(focused_window);
                            if (st) { st->x = dp_x; st->y = dp_y; st->width = new_w; st->height = new_h; }
                        }
                    } else if (strcmp(keys[i].cmd, "workspace_2") == 0) {
                        if (focused_window != None && focused_window != root) {
                            int hdmi_x = 2560, hdmi_y = 310; sscanf(hdmi_pos, "%dx%d", &hdmi_x, &hdmi_y);
                            int new_w = 1920 - (border_width * 2), new_h = 1080 - (border_width * 2);
                            XMoveResizeWindow(dpy, focused_window, hdmi_x, hdmi_y, new_w, new_h);
                            WinState *st = get_window_state(focused_window);
                            if (st) { st->x = hdmi_x; st->y = hdmi_y; st->width = new_w; st->height = new_h; }
                        }
                    } else if (strcmp(keys[i].cmd, "cursor_workspace_1") == 0) {
                        int dp_x = 0, dp_y = 0; sscanf(dp_pos, "%dx%d", &dp_x, &dp_y);
                        XWarpPointer(dpy, None, root, 0, 0, 0, 0, dp_x + (DisplayWidth(dpy, DefaultScreen(dpy)) / 2), dp_y + (DisplayHeight(dpy, DefaultScreen(dpy)) / 2));
                    } else if (strcmp(keys[i].cmd, "cursor_workspace_2") == 0) {
                        int hdmi_x = 2560, hdmi_y = 310; sscanf(hdmi_pos, "%dx%d", &hdmi_x, &hdmi_y);
                        XWarpPointer(dpy, None, root, 0, 0, 0, 0, hdmi_x + (1920 / 2), hdmi_y + (1080 / 2));
                    } else {
                        spawn(keys[i].cmd);
                    }
                }
            }
            break;
        }
        default: break;
    }
}

int main(void) {
    Display *dpy;
    Window root;
    XEvent ev;
    Cursor cursor;

    signal(SIGUSR1, handle_sigusr1);
    load_config();
    setup_monitors();

    if (!(dpy = XOpenDisplay(NULL))) exit(1);

    XSetErrorHandler(x_error_handler);
    root = DefaultRootWindow(dpy);
    set_ewmh_wm_name(dpy, root);
    wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    cursor = XCreateFontCursor(dpy, XC_dot);
    XDefineCursor(dpy, root, cursor);
    XWarpPointer(dpy, None, root, 0, 0, 0, 0, DisplayWidth(dpy, DefaultScreen(dpy)) / 2, DisplayHeight(dpy, DefaultScreen(dpy)) / 2);

    grab_keys(dpy, root);
    grab_buttons(dpy, root);
    run_autostart();
    
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | ExposureMask);
    XSync(dpy, False);
    XButtonEvent det_cursor = {0};

    while (1) {
        if (reload_requested) {
            load_config();
            setup_monitors();
            grab_keys(dpy, root);
            run_autostart();
            reload_requested = 0;
        }

        if (!is_panning && fabs(vel_x) < 1.0 && fabs(vel_y) < 1.0) {
            XNextEvent(dpy, &ev);
            process_xevent(dpy, root, &ev, &det_cursor);
        }

        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            process_xevent(dpy, root, &ev, &det_cursor);
        }

        if (!is_panning && (fabs(vel_x) >= 1.0 || fabs(vel_y) >= 1.0)) {
            double dt = 0.016;
            int step_x = (int)(vel_x * dt), step_y = (int)(vel_y * dt);
            if (step_x != 0 || step_y != 0) pan_viewport(dpy, root, step_x, step_y);
            vel_x *= 0.95; vel_y *= 0.95;
            XFlush(dpy);
            usleep(16000);
        }
    }
    XCloseDisplay(dpy);
    return 0;
}
