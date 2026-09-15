#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "config.h"

static void spawn(const char *cmd) {
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        exit(0);
    }
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

int main(void) {
    Display *dpy;
    Window root;
    XEvent ev;
    Cursor cursor;

    if (!(dpy = XOpenDisplay(NULL))) {
        exit(1);
    }

    root = DefaultRootWindow(dpy);

    cursor = XCreateFontCursor(dpy, XC_dot);
    XDefineCursor(dpy, root, cursor);

    grab_keys(dpy, root);

    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | ExposureMask);
    XSync(dpy, False);

    while (1) {
        XNextEvent(dpy, &ev);
        switch (ev.type) {
            case MapRequest:
                XSelectInput(dpy, ev.xmap.window, EnterWindowMask | FocusChangeMask);
                XMapWindow(dpy, ev.xmap.window);
                XSetWindowBorderWidth(dpy, ev.xmap.window, BORDER_WIDTH);
                XSetWindowBorder(dpy, ev.xmap.window, COLOR_BORDER);
                break;
            case EnterNotify:
                XSetInputFocus(dpy, ev.xcrossing.window, RevertToParent, CurrentTime);
                XSetWindowBorder(dpy, ev.xcrossing.window, COLOR_FOCUS);
                break;
            case LeaveNotify:
                XSetWindowBorder(dpy, ev.xcrossing.window, COLOR_BORDER);
                break;
            case KeyPress: {
                KeySym keysym = XLookupKeysym(&ev.xkey, 0);
                for (unsigned long i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                    if (keysym == keys[i].keysym && (ev.xkey.state & ~LockMask & ~Mod2Mask) == keys[i].mod) {
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
