#ifndef CONFIG_H
#define CONFIG_H

#define BORDER_WIDTH 1
#define COLOR_BG     0x101010
#define COLOR_FG     0xffffff
#define COLOR_BORDER 0x303030
#define COLOR_FOCUS  0x0000ff

#define DEFAULT_WIDTH  2560
#define DEFAULT_HEIGHT 1440

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
    { MODKEY, XK_d, "rofi -show drun" },
    { MODKEY, XK_p, "playerctl play-pause" },
    { MODKEY | ShiftMask, XK_w, "~/git/polybar-toggle/polybar-toggle" },
    { MODKEY | ShiftMask, XK_p, "poweroff" },
    { MODKEY | ShiftMask, XK_r, "reboot" },
    { MODKEY | ShiftMask, XK_g, "openrgb --profile ghost" },
    { MODKEY, XK_Tab, "alt-tab" },
    { MODKEY, XK_x, "center" },
    { MODKEY, XK_s, "steam" },
    { MODKEY, XK_space, "setxkbmap -layout us,ua -option grp:win_space_toggle" },
};

#endif
