#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "config.h"

static int next_x = 50;
static int next_y = 50;
static XWindowAttributes start_attr;
static Atom wm_delete_window;
static Atom wm_protocols;
static Window focused_window = None;

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

static void grab_keys(Display *dpy, Window root) {
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
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
}

static int x_error_handler(Display *dpy, XErrorEvent *ee) {
    (void)dpy;
    (void)ee;
    return 0;
}

int main(void) {
    Display *dpy;
    Window root;
    XEvent ev;
    Cursor cursor;

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
                
                XMoveResizeWindow(dpy, ev.xmap.window, next_x, next_y, DEFAULT_WIDTH, DEFAULT_HEIGHT);
                next_x += 30;
                next_y += 30;
                if (next_x > 400) next_x = 50;
                if (next_y > 400) next_y = 50;

                XMapWindow(dpy, ev.xmap.window);
                XSetWindowBorderWidth(dpy, ev.xmap.window, BORDER_WIDTH);
                XSetWindowBorder(dpy, ev.xmap.window, COLOR_BORDER);
                break;
            }
            case UnmapNotify:
            case DestroyNotify: {
                Window w = (ev.type == UnmapNotify) ? ev.xunmap.window : ev.xdestroywindow.window;
                if (w == focused_window) {
                    focused_window = None;
                }
                break;
            }
            case EnterNotify: {
                focused_window = ev.xcrossing.window;
                XSetInputFocus(dpy, focused_window, RevertToParent, CurrentTime);
                XSetWindowBorder(dpy, focused_window, COLOR_FOCUS);
                break;
            }
            case LeaveNotify: {
                if (ev.xcrossing.window != root) {
                    XSetWindowBorder(dpy, ev.xcrossing.window, COLOR_BORDER);
                }
                break;
            }
            case ButtonPress: {
                if (ev.xbutton.subwindow != None) {
                    focused_window = ev.xbutton.subwindow;
                    XGetWindowAttributes(dpy, focused_window, &start_attr);
                    det_cursor = ev.xbutton;
                    XRaiseWindow(dpy, focused_window);
                }
                break;
            }
            case MotionNotify: {
                if (ev.xmotion.state & Mod1Mask) {
                    int xdiff = ev.xmotion.x_root - det_cursor.x_root;
                    int ydiff = ev.xmotion.y_root - det_cursor.y_root;

                    if (det_cursor.button == Button1) {
                        XMoveWindow(dpy, focused_window, start_attr.x + xdiff, start_attr.y + ydiff);
                    } else if (det_cursor.button == Button3) {
                        int new_w = start_attr.width + xdiff;
                        int new_h = start_attr.height + ydiff;
                        if (new_w < 100) new_w = 100;
                        if (new_h < 100) new_h = 100;
                        XResizeWindow(dpy, focused_window, new_w, new_h);
                    }
                }
                break;
            }
            case KeyPress: {
                KeySym keysym = XLookupKeysym(&ev.xkey, 0);
                unsigned int mod = ev.xkey.state & ~LockMask & ~Mod2Mask;

                if (mod == MODKEY && keysym == XK_c && focused_window != None && focused_window != root) {
                    send_event(dpy, focused_window, wm_delete_window);
                    break;
                }

                if (mod == MODKEY && keysym == XK_Tab) {
                    XCirculateSubwindowsDown(dpy, root);
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
