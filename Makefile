# displayOff - monitors off after a while, a starfield on request

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wayland-client

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
IDLE_NOTIFY = $(WAYLAND_PROTOCOLS)/staging/ext-idle-notify/ext-idle-notify-v1.xml
IDLE_INHIBIT = $(WAYLAND_PROTOCOLS)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
LAYER_SHELL = protocols/wlr-layer-shell-unstable-v1.xml
OUTPUT_POWER = protocols/wlr-output-power-management-unstable-v1.xml

PROTO_H = build/ext-idle-notify-v1-client-protocol.h \
	build/wlr-layer-shell-unstable-v1-client-protocol.h \
	build/wlr-output-power-management-unstable-v1-client-protocol.h
# The layer shell refers to xdg_popup, so the code of xdg-shell has to be there.
PROTO_C = build/ext-idle-notify-v1-protocol.c build/wlr-layer-shell-unstable-v1-protocol.c \
	build/wlr-output-power-management-unstable-v1-protocol.c build/xdg-shell-protocol.c

# CFLAGS and LDLIBS stay the user's, also on the command line: `make CFLAGS=-O0`
# must not lose the include paths.
CFLAGS ?= -O2 -g
DO_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
DO_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: displayOff

displayOff: displayOff.c $(PROTO_H) $(PROTO_C)
	$(CC) $(DO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ displayOff.c $(PROTO_C) $(DO_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/ext-idle-notify-v1-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(IDLE_NOTIFY) $@
build/ext-idle-notify-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(IDLE_NOTIFY) $@
build/wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYER_SHELL) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/wlr-layer-shell-unstable-v1-protocol.c: $(LAYER_SHELL) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/wlr-output-power-management-unstable-v1-client-protocol.h: $(OUTPUT_POWER) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/wlr-output-power-management-unstable-v1-protocol.c: $(OUTPUT_POWER) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/xdg-shell-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@
build/idle-inhibit-unstable-v1-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(IDLE_INHIBIT) $@
build/idle-inhibit-unstable-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(IDLE_INHIBIT) $@

# A client that asks not to be idle, as a video player does.
build/inhibit: tests/inhibit.c build/idle-inhibit-unstable-v1-client-protocol.h \
		build/idle-inhibit-unstable-v1-protocol.c
	$(CC) $(DO_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/inhibit.c \
		build/idle-inhibit-unstable-v1-protocol.c $(DO_LIBS) $(LDLIBS)

install: displayOff
	install -Dm755 displayOff $(DESTDIR)$(PREFIX)/bin/displayOff

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/displayOff

# Needs a built mwc next to this directory, or in MWC_DIR, plus grim and foot.
test: displayOff build/inhibit
	tests/ui.sh

clean:
	rm -rf displayOff build

.PHONY: all install uninstall test clean
