#ifndef CONFIG_H
#define CONFIG_H

#define BORDER_WIDTH 1
#define COLOR_BG     0x101010
#define COLOR_FG     0xffffff
#define COLOR_BORDER 0x303030
#define COLOR_FOCUS  0x0000ff

#define MODKEY Mod4Mask

struct KeyBinding {
    unsigned int mod;
    KeySym keysym;
    const char *cmd;
};

static struct KeyBinding keys[] = {
    { MODKEY, XK_q, "alacritty" },
    { MODKEY, XK_w, "firefox" },
    { MODKEY, XK_c, "close-window" },
    { MODKEY, XK_d, "fuzzel" },
    { MODKEY, XK_p, "playerctl play-pause" },
    { MODKEY | ShiftMask, XK_w, "~/.config/driftwm/ironbar-toggle" },
    { MODKEY | ShiftMask, XK_p, "poweroff" },
    { MODKEY | ShiftMask, XK_r, "reboot" },
    { MODKEY | ShiftMask, XK_g, "openrgb --profile ghost" },
    { MODKEY, XK_Tab, "~/.config/driftwm/window-search.sh" },
    { MODKEY, XK_s, "steam" },
    { Mod1Mask, XK_Print, "sh -c 'grim -g \"$(slurp)\" - | wl-copy'" },
    { 0, XK_Print, "sh -c 'grim ~/Screenshots/screenshot_$(date +%Y%m%d_%H%M%S).png'" },
    { ControlMask, XK_Print, "sh -c 'grim -g \"$(slurp)\" ~/Screenshot_$(date +%Y%m%d_%H%M%S).png'" },
    { MODKEY, XK_h, "center-nearest left" },
    { MODKEY, XK_j, "center-nearest down" },
    { MODKEY, XK_k, "center-nearest up" },
    { MODKEY, XK_l, "center-nearest right" },
    { MODKEY | ControlMask, XK_h, "pan-viewport left" },
    { MODKEY | ControlMask, XK_j, "pan-viewport down" },
    { MODKEY | ControlMask, XK_k, "pan-viewport up" },
    { MODKEY | ControlMask, XK_l, "pan-viewport right" },
    { MODKEY | ShiftMask, XK_h, "nudge-window left" },
    { MODKEY | ShiftMask, XK_j, "nudge-window down" },
    { MODKEY | ShiftMask, XK_k, "nudge-window up" },
    { MODKEY | ShiftMask, XK_l, "nudge-window right" },
    { MODKEY, XK_equal, "zoom-in" },
    { MODKEY, XK_minus, "zoom-out" },
    { MODKEY, XK_0, "zoom-reset" },
    { MODKEY, XK_a, "zoom-to-fit" },
    { MODKEY, XK_x, "center-window" },
};

#endif
