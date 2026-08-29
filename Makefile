# viewpoint — terminal multiplexer with a desktop-window-manager metaphor.
# Plain hand-written Makefile, pkg-config driven.

CC      ?= cc
PKGS     = notcurses vterm libsixel

CFLAGS  += -std=gnu11 -Wall -Wextra -O2 -g
CFLAGS  += $(shell pkg-config --cflags $(PKGS))

# notcurses/vterm via pkg-config; gpm has no .pc file, so it's linked directly
# with -lgpm (we drive the bare-console mouse ourselves); forkpty needs libutil.
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lgpm -lutil

BIN      = viewpoint
OBJS     = main.o pty.o session.o vt_bridge.o compositor.o window.o wm.o input.o taskbar.o config.o settings.o sixel.o theme.o sizeosd.o selection.o

# Phase 1 builds with a subset; the full target needs every unit. To bring up
# an earlier phase, override OBJS on the command line, e.g.:
#   make OBJS="main.o pty.o vt_bridge.o"

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c viewpoint.h compositor.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) $(BIN) $(OBJS)

.PHONY: all clean
