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
#include "config.h"

typedef struct WinState {
    Window w;
    int x, y;
    unsigned int width, height;
    struct WinState *next;
} WinState;

static WinState *saved_states = NULL;
static XWindowAttributes start_attr;
static Atom wm_delete_window;
static Atom wm_protocols;
static Window focused_window = None;

static double zoom_factor = 1.0;

static void save_window_state(Window w, int x, int y, unsigned int width, unsigned int height) {
    WinState *curr = saved_states;
    while (curr) {
        if (curr->w == w) return;
        curr = curr->next;
    }
    WinState *node = malloc(sizeof(WinState));
    node->w = w;
    node->x = x;
    node->y = y;
    node->width = width;
    node->height = height;
    node->next = saved_states;
    saved_states = node;
}

static WinState* get_window_state(Window w) {
    WinState *curr = saved_states;
    while (curr) {
        if (curr->w == w) return curr;
        curr = curr->next;
    }
    return NULL;
}

static void remove_window_state(Window w) {
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

static void setup_hdmi_position(void) {
    FILE *fp = popen("xrandr --query", "r");
    if (!fp) return;

    char line[256];
    char hdmi_name[64] = {0};

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "HDMI-", 5) == 0 && strstr(line, " connected")) {
            sscanf(line, "%63s", hdmi_name);
            break;
        }
    }
    pclose(fp);

    if (hdmi_name[0] != '\0') {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "xrandr --output %s --pos 2560x310", hdmi_name);
        system(cmd);
    }
}

static void spawn(const char *cmd) {
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        exit(0);
    }
}

static void send_event(Display *dpy, Window w, Atom proto) {
    XEvent ev;
    int n;
    Atom *protocols;
    
    if (XGetWMProtocols(dpy, w, &protocols, &n)) {
        while (--n >= 0)
            if (protocols[n] == proto) {
                ev.type = ClientMessage;
                ev.xclient.window = w;
                ev.xclient.message_type = wm_protocols;
                ev.xclient.format = 32;
                ev.xclient.data.l[0] = wm_delete_window;
                ev.xclient.data.l[1] = CurrentTime;
                XSendEvent(dpy, w, False, NoEventMask, &ev);
                XFree(protocols);
                return;
            }
        XFree(protocols);
    }
    XKillClient(dpy, w);
}

static void set_focus(Display *dpy, Window w) {
    if (focused_window != None && focused_window != DefaultRootWindow(dpy)) {
        XSetWindowBorder(dpy, focused_window, COLOR_BORDER);
    }
    focused_window = w;
    if (focused_window != None && focused_window != DefaultRootWindow(dpy)) {
        XSetInputFocus(dpy, focused_window, RevertToParent, CurrentTime);
        XSetWindowBorder(dpy, focused_window, COLOR_FOCUS);
    }
}

static void apply_zoom(Display *dpy, Window root) {
    int screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
    int screen_h = DisplayHeight(dpy, DefaultScreen(dpy));
    int center_x = screen_w / 2;
    int center_y = screen_h / 2;

    Window root_ret, parent_ret, *children;
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

static void grab_keys(Display *dpy, Window root) {
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    
    KeyCode tab_code = XKeysymToKeycode(dpy, XK_Tab);
    if (tab_code) {
        XGrabKey(dpy, tab_code, Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, tab_code, Mod1Mask | LockMask, root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, tab_code, Mod1Mask | Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, tab_code, Mod1Mask | LockMask | Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
    }

    for (unsigned long i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        KeySym keysym = keys[i].keysym;
        KeyCode code = XKeysymToKeycode(dpy, keysym);
        if (code) {
            XGrabKey(dpy, code, keys[i].mod, root, True, GrabModeAsync, GrabModeAsync);
            XGrabKey(dpy, code, keys[i].mod | LockMask, root, True, GrabModeAsync, GrabModeAsync);
            XGrabKey(dpy, code, keys[i].mod | Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
            XGrabKey(dpy, code, keys[i].mod | LockMask | Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
        }
    }
}

static void grab_buttons(Display *dpy, Window root) {
    XGrabButton(dpy, Button1, Mod1Mask, root, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, Mod1Mask, root, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button1, Mod4Mask, root, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button2, Mod4Mask, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button4, Mod4Mask, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button5, Mod4Mask, root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
}

static int x_error_handler(Display *dpy, XErrorEvent *ee) {
    (void)dpy;
    (void)ee;
    return 0;
}

static void pan_viewport(Display *dpy, Window root, int dx, int dy) {
    Window root_ret, parent_ret, *children;
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

static void cycle_windows(Display *dpy, Window root) {
    Window root_ret, parent_ret, *children;
    unsigned int nchildren;
    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren) && nchildren > 0) {
        unsigned int valid_count = 0;
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, children[i], &wa) && wa.map_state == IsViewable && !wa.override_redirect) {
                valid_count++;
            }
        }

        if (valid_count > 0) {
            Window target = children[0];
            XRaiseWindow(dpy, target);
            set_focus(dpy, target);
        }
        XFree(children);
    }
}

int main(void) {
    Display *dpy;
    Window root;
    XEvent ev;
    Cursor cursor;

    setup_hdmi_position();

    if (!(dpy = XOpenDisplay(NULL))) {
        exit(1);
    }

    XSetErrorHandler(x_error_handler);

    root = DefaultRootWindow(dpy);
    wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    cursor = XCreateFontCursor(dpy, XC_dot);
    XDefineCursor(dpy, root, cursor);

    grab_keys(dpy, root);
    grab_buttons(dpy, root);

    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | ExposureMask);
    XSync(dpy, False);

    XButtonEvent det_cursor = {0};

    while (1) {
        XNextEvent(dpy, &ev);
        switch (ev.type) {
            case MapRequest: {
                XSelectInput(dpy, ev.xmap.window, EnterWindowMask | FocusChangeMask | StructureNotifyMask);
                
                int rx, ry, wx, wy;
                unsigned int mask;
                Window r_ret, c_ret;
                int spawn_x = 0, spawn_y = 0;

                if (XQueryPointer(dpy, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask)) {
                    spawn_x = rx - (DEFAULT_WIDTH / 2);
                    spawn_y = ry - (DEFAULT_HEIGHT / 2);
                }

                XMoveResizeWindow(dpy, ev.xmap.window, spawn_x, spawn_y, DEFAULT_WIDTH, DEFAULT_HEIGHT);
                save_window_state(ev.xmap.window, spawn_x, spawn_y, DEFAULT_WIDTH, DEFAULT_HEIGHT);
                XMapWindow(dpy, ev.xmap.window);
                XSetWindowBorderWidth(dpy, ev.xmap.window, BORDER_WIDTH);
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
                if ((ev.xbutton.state & Mod4Mask)) {
                    if (ev.xbutton.button == Button5) { // Колесо вниз (від'їзд)
                        zoom_factor -= 0.1;
                        if (zoom_factor < 0.2) zoom_factor = 0.2;
                        apply_zoom(dpy, root);
                        break;
                    } else if (ev.xbutton.button == Button4) { // Колесо вгору (приближення)
                        zoom_factor += 0.1;
                        if (zoom_factor > 2.5) zoom_factor = 2.5;
                        apply_zoom(dpy, root);
                        break;
                    } else if (ev.xbutton.button == Button2) { // СКМ (скидання)
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
            case MotionNotify: {
                int xdiff = ev.xmotion.x_root - det_cursor.x_root;
                int ydiff = ev.xmotion.y_root - det_cursor.y_root;

                if ((ev.xmotion.state & Mod4Mask) && det_cursor.button == Button1) {
                    pan_viewport(dpy, root, -xdiff, -ydiff);
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
                unsigned int mod = ev.xkey.state & ~LockMask & ~Mod2Mask;

                if (mod == Mod1Mask && keysym == XK_Tab) {
                    cycle_windows(dpy, root);
                    break;
                }

                if (mod == MODKEY && keysym == XK_c && focused_window != None && focused_window != root) {
                    send_event(dpy, focused_window, wm_delete_window);
                    break;
                }

                for (unsigned long i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                    if (keysym == keys[i].keysym && mod == keys[i].mod) {
                        spawn(keys[i].cmd);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    XCloseDisplay(dpy);
    return 0;
}
