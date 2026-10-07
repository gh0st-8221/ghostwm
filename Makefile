CC      ?= cc
PREFIX  ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin
SHAREDIR = $(DESTDIR)$(PREFIX)/share
XSESS   = $(DESTDIR)/usr/share/xsessions
CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra $(shell pkg-config --cflags x11 xrandr xcomposite xdamage xrender)
LDLIBS  = $(shell pkg-config --libs x11 xrandr xcomposite xdamage xrender) -lm

all: ghostwm

ghostwm: ghostwm.c
	$(CC) $(CFLAGS) -o $@ ghostwm.c $(LDFLAGS) $(LDLIBS)

install: ghostwm
	install -Dm755 ghostwm $(BINDIR)/ghostwm
	@if [ -f config.toml ]; then install -Dm644 config.toml $(SHAREDIR)/ghostwm/config.toml; fi
	printf '[Desktop Entry]\nName=ghostwm\nComment=Minimal zoomable WM\nExec=ghostwm\nType=Application\n' > ghostwm.desktop
	install -Dm644 ghostwm.desktop $(XSESS)/ghostwm.desktop
	rm -f ghostwm.desktop

install-config:
	@if [ ! -f config.toml ]; then echo "no config.toml in this dir, skipping"; \
	elif [ -e $(HOME)/.config/ghostwm/config.toml ]; then echo "config exists, skipping"; \
	else install -Dm644 config.toml $(HOME)/.config/ghostwm/config.toml; fi

uninstall:
	rm -f $(BINDIR)/ghostwm $(XSESS)/ghostwm.desktop
	rm -rf $(SHAREDIR)/ghostwm

clean:
	rm -f ghostwm ghostwm.desktop

.PHONY: all install install-config uninstall clean
