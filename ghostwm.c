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

#define MAIN_MONITOR_WIDTH  2560
#define MAIN_MONITOR_HEIGHT 1440
#define MAX_VELOCITY 1500.0
#define MAX_KEYS 128
#define MAX_AUTOSTART 64

int border_width = 1;
unsigned long color_bg = 0x101010;
unsigned long color_fg = 0xffffff;
unsigned long color_border = 0x303030;
unsigned long color_focus = 0x0000ff;

int default_width = 2560;
int default_height = 1440;

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
    struct WinState *next;
} WinState;

WinState *saved_states = NULL;
XWindowAttributes start_attr;
Atom wm_delete_window;
Atom wm_protocols;
Window focused_window = None;

double zoom_factor = 1.0;
int is_panning = 0;
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
    if (strstr(k, "Print")) {
        sym = XK_Print;
    } else {
        char clean_k[64] = {0};
        int j = 0;
        for (int i = 0; k[i] && j < 63; i++) {
            if (k[i] != ' ' && k[i] != '\t' && k[i] != '\n' && k[i] != '\r') {
                clean_k[j++] = k[i];
            }
        }
        if (clean_k[0] >= 'A' && clean_k[0] <= 'Z' && strlen(clean_k) == 1) {
            clean_k[0] += 32;
        }
        sym = XStringToKeysym(clean_k);
        if (sym == NoSymbol && strlen(clean_k) == 1) {
            sym = clean_k[0];
        }
    }
    
    if (sym != NoSymbol) {
        keys[keys_count].mod = mod;
        keys[keys_count].keysym = sym;
        keys[keys_count].cmd = strdup(cmd);
        keys_count++;
    }
}

void load_config(void) {
    for (int i = 0; i < keys_count; i++) {
        free(keys[i].cmd);
    }
    keys_count = 0;
    
    for (int i = 0; i < autostart_count; i++) {
        free(autostart_cmds[i]);
    }
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
        } else if (strcmp(section, "colors") == 0) {
            if (is_quoted) {
                if (strcmp(key, "bg") == 0) color_bg = strtoul(val, NULL, 0);
                else if (strcmp(key, "fg") == 0) color_fg = strtoul(val, NULL, 0);
                else if (strcmp(key, "border") == 0) color_border = strtoul(val, NULL, 0);
                else if (strcmp(key, "focus") == 0) color_focus = strtoul(val, NULL, 0);
            }
        } else if (strcmp(section, "monitors") == 0) {
            if (is_quoted) {
                if (strcmp(key, "hdmi_pos") == 0) snprintf(hdmi_pos, sizeof(hdmi_pos), "%s", val);
                else if (strcmp(key, "dp_pos") == 0) snprintf(dp_pos, sizeof(dp_pos), "%s", val);
            }
        } else if (strcmp(section, "autostart") == 0) {
            if (is_quoted && strcmp(key, "cmd") == 0 && autostart_count < MAX_AUTOSTART) {
                autostart_cmds[autostart_count++] = strdup(val);
            }
        } else if (strcmp(section, "keys") == 0) {
            if (is_quoted) {
                parse_key_str(key, val);
            }
        }
    }
    fclose(fp);
}

double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
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

void setup_monitors(void) {
    FILE *fp = popen("xrandr --query", "r");
    if (!fp) return;

    char line[256];
    char hdmi_name[64] = {0};
    char dp_name[64] = {0};

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "HDMI", 4) == 0 && strstr(line, " connected")) {
            sscanf(line, "%63s", hdmi_name);
        }
        if (strncmp(line, "DP", 2) == 0 && strstr(line, " connected")) {
            sscanf(line, "%63s", dp_name);
        }
    }
    pclose(fp);

    char cmd[1024] = "xrandr";
    int run = 0;
    
    if (hdmi_name[0] != '\0') {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), " --output %s --pos %s --auto", hdmi_name, hdmi_pos);
        strcat(cmd, tmp);
        run = 1;
    }
    if (dp_name[0] != '\0') {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), " --output %s --pos %s --auto", dp_name, dp_pos);
        strcat(cmd, tmp);
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
        if (autostart_cmds[i]) {
            spawn(autostart_cmds[i]);
        }
    }
}

void send_event(Display *dpy, Window w, Atom proto) {
    int n;
    Atom *protocols = NULL;
    int exists = 0;

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
        ev.xclient.data.l[0] = wm_delete_window;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, w, False, NoEventMask, &ev);
    } else {
        XKillClient(dpy, w);
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

void apply_zoom(Display *dpy, Window root) {
    int screen_w = MAIN_MONITOR_WIDTH;
    int screen_h = MAIN_MONITOR_HEIGHT;
    int center_x = screen_w / 2;
    int center_y = screen_h / 2;

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

                    int orig_center_x = st->x + (st->width / 2);
                    int orig_center_y = st->y + (st->height / 2);

                    int new_center_x = center_x + (int)((orig_center_x - center_x) * zoom_factor);
                    int new_center_y = center_y + (int)((orig_center_y - center_y) * zoom_factor);

                    int new_x = new_center_x - (new_w / 2);
                    int new_y = new_center_y - (new_h / 2);

                    XMoveResizeWindow(dpy, children[i], new_x, new_y, new_w, new_h);
                }
            }
        }
        if (children) XFree(children);
    }
}

void grab_keys(Display *dpy, Window root) {
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    
    unsigned int ign[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask, 0x2000, 0x2000 | LockMask, 0x2000 | Mod2Mask, 0x2000 | LockMask | Mod2Mask};
    
    for (int i = 0; i < keys_count; i++) {
        KeyCode code = XKeysymToKeycode(dpy, keys[i].keysym);
        if (code) {
            for (int j = 0; j < 8; j++) {
                XGrabKey(dpy, code, keys[i].mod | ign[j], root, True, GrabModeAsync, GrabModeAsync);
            }
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

int x_error_handler(Display *dpy, XErrorEvent *ee) {
    (void)dpy;
    (void)ee;
    return 0;
}

void pan_viewport(Display *dpy, Window root, int dx, int dy) {
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren;
    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, children[i], &wa) && wa.map_state == IsViewable) {
                XMoveWindow(dpy, children[i], wa.x - dx, wa.y - dy);
                WinState *st = get_window_state(children[i]);
                if (st) {
                    st->x -= dx;
                    st->y -= dy;
                }
            }
        }
        if (children) XFree(children);
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

    if (!(dpy = XOpenDisplay(NULL))) {
        exit(1);
    }

    XSetErrorHandler(x_error_handler);

    root = DefaultRootWindow(dpy);
    wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    cursor = XCreateFontCursor(dpy, XC_dot);
    XDefineCursor(dpy, root, cursor);

    XWarpPointer(dpy, None, root, 0, 0, 0, 0, MAIN_MONITOR_WIDTH / 2, MAIN_MONITOR_HEIGHT / 2);

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

        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            switch (ev.type) {
                case MapRequest: {
                    XSelectInput(dpy, ev.xmap.window, EnterWindowMask | FocusChangeMask | StructureNotifyMask);
                    
                    int rx, ry, wx, wy;
                    unsigned int mask;
                    Window r_ret, c_ret;
                    int spawn_x = 0, spawn_y = 0;

                    if (XQueryPointer(dpy, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask)) {
                        spawn_x = rx - (default_width / 2);
                        spawn_y = ry - (default_height / 2);
                    } else {
                        spawn_x = (MAIN_MONITOR_WIDTH - default_width) / 2;
                        spawn_y = (MAIN_MONITOR_HEIGHT - default_height) / 2;
                    }

                    XMoveResizeWindow(dpy, ev.xmap.window, spawn_x, spawn_y, default_width, default_height);
                    save_window_state(ev.xmap.window, spawn_x, spawn_y, default_width, default_height);
                    XMapWindow(dpy, ev.xmap.window);
                    XSetWindowBorderWidth(dpy, ev.xmap.window, border_width);
                    set_focus(dpy, ev.xmap.window);
                    break;
                }
                case UnmapNotify:
                case DestroyNotify: {
                    Window w = (ev.type == UnmapNotify) ? ev.xunmap.window : ev.xdestroywindow.window;
                    remove_window_state(w);
                    if (w == focused_window) {
                        set_focus(dpy, None);
                    }
                    break;
                }
                case EnterNotify: {
                    set_focus(dpy, ev.xcrossing.window);
                    break;
                }
                case ButtonPress: {
                    vel_x = 0.0;
                    vel_y = 0.0;

                    if ((ev.xbutton.state & Mod4Mask)) {
                        if (ev.xbutton.button == Button1) {
                            is_panning = 1;
                            gettimeofday(&last_motion_time, NULL);
                            last_dx = 0;
                            last_dy = 0;
                        } else if (ev.xbutton.button == Button5) {
                            zoom_factor -= 0.1;
                            if (zoom_factor < 0.2) zoom_factor = 0.2;
                            apply_zoom(dpy, root);
                            break;
                        } else if (ev.xbutton.button == Button4) {
                            zoom_factor += 0.1;
                            if (zoom_factor > 2.5) zoom_factor = 2.5;
                            apply_zoom(dpy, root);
                            break;
                        } else if (ev.xbutton.button == Button2) {
                            zoom_factor = 1.0;
                            apply_zoom(dpy, root);
                            break;
                        }
                    }

                    if (ev.xbutton.subwindow != None) {
                        set_focus(dpy, ev.xbutton.subwindow);
                        XGetWindowAttributes(dpy, focused_window, &start_attr);
                        det_cursor = ev.xbutton;
                        XRaiseWindow(dpy, focused_window);
                    } else {
                        det_cursor = ev.xbutton;
                    }
                    break;
                }
                case ButtonRelease: {
                    if (ev.xbutton.button == Button1 && is_panning) {
                        is_panning = 0;
                        
                        double now = get_time_ms();
                        double last_time = (double)last_motion_time.tv_sec * 1000.0 + (double)last_motion_time.tv_usec / 1000.0;
                        double dt = now - last_time;

                        if (dt < 50.0 && dt > 0.0) {
                            vel_x = (double)last_dx / (dt / 1000.0);
                            vel_y = (double)last_dy / (dt / 1000.0);

                            if (vel_x > MAX_VELOCITY) vel_x = MAX_VELOCITY;
                            if (vel_x < -MAX_VELOCITY) vel_x = -MAX_VELOCITY;
                            if (vel_y > MAX_VELOCITY) vel_y = MAX_VELOCITY;
                            if (vel_y < -MAX_VELOCITY) vel_y = -MAX_VELOCITY;
                        } else {
                            vel_x = 0.0;
                            vel_y = 0.0;
                        }
                    }
                    break;
                }
                case MotionNotify: {
                    while (XCheckTypedEvent(dpy, MotionNotify, &ev));

                    int xdiff = ev.xmotion.x_root - det_cursor.x_root;
                    int ydiff = ev.xmotion.y_root - det_cursor.y_root;

                    if ((ev.xmotion.state & Mod4Mask) && det_cursor.button == Button1) {
                        pan_viewport(dpy, root, -xdiff, -ydiff);
                        
                        last_dx = -xdiff;
                        last_dy = -ydiff;
                        gettimeofday(&last_motion_time, NULL);

                        det_cursor.x_root = ev.xmotion.x_root;
                        det_cursor.y_root = ev.xmotion.y_root;
                    } else if ((ev.xmotion.state & Mod1Mask) && focused_window != None) {
                        if (det_cursor.button == Button1) {
                            int nx = start_attr.x + xdiff;
                            int ny = start_attr.y + ydiff;
                            XMoveWindow(dpy, focused_window, nx, ny);
                            
                            WinState *st = get_window_state(focused_window);
                            if (st) {
                                st->x = nx;
                                st->y = ny;
                            }
                        } else if (det_cursor.button == Button3) {
                            int new_w = start_attr.width + xdiff;
                            int new_h = start_attr.height + ydiff;
                            if (new_w < 100) new_w = 100;
                            if (new_h < 100) new_h = 100;
                            XResizeWindow(dpy, focused_window, new_w, new_h);
                            
                            WinState *st = get_window_state(focused_window);
                            if (st) {
                                st->width = new_w;
                                st->height = new_h;
                            }
                        }
                    }
                    break;
                }
                case KeyPress: {
                    KeySym keysym = XLookupKeysym(&ev.xkey, 0);
                    unsigned int mod = ev.xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);

                    for (int i = 0; i < keys_count; i++) {
                        if (keysym == keys[i].keysym && mod == keys[i].mod) {
                            if (strcmp(keys[i].cmd, "close-window") == 0) {
                                if (focused_window != None && focused_window != root) {
                                    send_event(dpy, focused_window, wm_delete_window);
                                }
                            } else if (strcmp(keys[i].cmd, "workspace_1") == 0) {
                                // Тепаємо вікно на DP монітор (2560x1440)
                                if (focused_window != None && focused_window != root) {
                                    int dp_x = 0, dp_y = 0;
                                    sscanf(dp_pos, "%dx%d", &dp_x, &dp_y);
                                    
                                    int new_w = 2560 - (border_width * 2);
                                    int new_h = 1440 - (border_width * 2);
                                    
                                    XMoveResizeWindow(dpy, focused_window, dp_x, dp_y, new_w, new_h);
                                    WinState *st = get_window_state(focused_window);
                                    if (st) { st->x = dp_x; st->y = dp_y; st->width = new_w; st->height = new_h; }
                                }
                            } else if (strcmp(keys[i].cmd, "workspace_2") == 0) {
                                // Тепаємо вікно на HDMI монітор (1920x1080)
                                if (focused_window != None && focused_window != root) {
                                    int hdmi_x = 2560, hdmi_y = 310;
                                    sscanf(hdmi_pos, "%dx%d", &hdmi_x, &hdmi_y);
                                    
                                    int new_w = 1920 - (border_width * 2);
                                    int new_h = 1080 - (border_width * 2);

                                    XMoveResizeWindow(dpy, focused_window, hdmi_x, hdmi_y, new_w, new_h);
                                    WinState *st = get_window_state(focused_window);
                                    if (st) { st->x = hdmi_x; st->y = hdmi_y; st->width = new_w; st->height = new_h; }
                                }
                            } else if (strcmp(keys[i].cmd, "cursor_workspace_1") == 0) {
                                // Тепаємо курсор по центру DP (2560x1440)
                                int dp_x = 0, dp_y = 0;
                                sscanf(dp_pos, "%dx%d", &dp_x, &dp_y);
                                XWarpPointer(dpy, None, root, 0, 0, 0, 0, dp_x + (2560 / 2), dp_y + (1440 / 2));
                            } else if (strcmp(keys[i].cmd, "cursor_workspace_2") == 0) {
                                // Тепаємо курсор по центру HDMI (1920x1080)
                                int hdmi_x = 2560, hdmi_y = 310;
                                sscanf(hdmi_pos, "%dx%d", &hdmi_x, &hdmi_y);
                                XWarpPointer(dpy, None, root, 0, 0, 0, 0, hdmi_x + (1920 / 2), hdmi_y + (1080 / 2));
                            } else {
                                spawn(keys[i].cmd);
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (!is_panning && (fabs(vel_x) > 1.0 || fabs(vel_y) > 1.0)) {
            double dt = 0.016;
            int step_x = (int)(vel_x * dt);
            int step_y = (int)(vel_y * dt);

            if (step_x != 0 || step_y != 0) {
                pan_viewport(dpy, root, step_x, step_y);
            }

            vel_x *= 0.95;
            vel_y *= 0.95;
            
            XFlush(dpy);
        }

        usleep(16000);
    }

    XCloseDisplay(dpy);
    return 0;
}
