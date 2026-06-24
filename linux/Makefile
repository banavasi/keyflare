CFLAGS ?= -O2 -Wall -Wextra
PKGS := gtk+-3.0 gtk-layer-shell-0
PREFIX ?= $(HOME)/.local

keyflare: keyflare.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(PKGS)) -o $@ $< $(shell pkg-config --libs $(PKGS))

install: keyflare
	mkdir -p $(PREFIX)/bin
	ln -sf $(CURDIR)/keyflare $(PREFIX)/bin/keyflare

clean:
	rm -f keyflare

.PHONY: install clean
