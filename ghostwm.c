#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <stdlib.h>
#include "config.h"

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
            default:
                break;
        }
    }

    XCloseDisplay(dpy);
    return 0;
}
