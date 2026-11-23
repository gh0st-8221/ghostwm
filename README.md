# ghostwm

A lightweight X11 tiling window manager written in C from scratch.

Configuration: `~/.config/ghostwm/config.toml`

## Background fullscreen

ghostwm can keep fullscreen applications mapped when you switch focus or
change workspaces. This is useful for browsers and media apps that should keep
their fullscreen state instead of receiving an unfullscreen request whenever
another window gets focus.

When enabled, ghostwm lowers the fullscreen window behind the active workspace
instead of removing `_NET_WM_STATE_FULLSCREEN`. A background fullscreen window
stays mapped while another non-minimized window is visible on the same monitor.
If the destination workspace has no visible client, the background fullscreen
window is unmapped until there is another visible client.

This preserves the X11 mapped/viewable state; it cannot guarantee that every
application or website ignores occlusion, throttling, or its own background
visibility policy.

Set this in the `[general]` section:

```toml
fullscreen_keepalive = true
```

Set it to `false` to restore the traditional behavior where moving focus away
from a fullscreen client exits fullscreen.

## Example configuration

```toml
[general]
border_width = 1
default_width = 1200
default_height = 800
workspaces = 4
sleep_timeout = 0
fullscreen_keepalive = true

[colors]
bg = "0x000000"
fg = "0xffffff"
border = "0x303030"
focus = "0x0000ff"

[keys]
"Mod4+f" = "fullscreen"
"Mod4+Tab" = "alt-tab"
"Mod4+1" = "workspace 1"
"Mod4+2" = "workspace 2"
"Mod4+3" = "workspace 3"
"Mod4+4" = "workspace 4"
```

## Build

```sh
make
sudo make install
```
