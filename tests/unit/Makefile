# Unit checks over viewpoint's own functions.
#
# Unlike the black-box helpers one directory up, these have to reach inside the
# project - that is the point of them. They keep the sibling Makefile's promise
# about stale objects a different way: sources are compiled straight into each
# test binary and the root build's .o files are never touched, so a run can
# neither be confused by a stale object nor leave one behind.

CC      ?= cc
PKGS     = notcurses vterm libsixel

CFLAGS  += -std=gnu11 -Wall -Wextra -O1 -g
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lgpm -lutil

ROOT     = ../..

# Everything but main.c, which owns main() and vp_log(); check.h supplies a
# quiet vp_log in its place.
SRCS = $(ROOT)/pty.c $(ROOT)/session.c $(ROOT)/vt_bridge.c \
       $(ROOT)/compositor.c $(ROOT)/window.c $(ROOT)/wm.c $(ROOT)/input.c \
       $(ROOT)/taskbar.c $(ROOT)/config.c $(ROOT)/settings.c $(ROOT)/sixel.c \
       $(ROOT)/theme.c $(ROOT)/sizeosd.c

# fade_test.c #includes sizeosd.c to reach its file-static helpers, so it must
# not also be handed the standalone copy.
FADE_SRCS = $(filter-out $(ROOT)/sizeosd.c,$(SRCS))

TESTS = fade_test config_test

all: $(TESTS)

fade_test: fade_test.c check.h $(FADE_SRCS)
	$(CC) $(CFLAGS) -o $@ fade_test.c $(FADE_SRCS) $(LDLIBS)

config_test: config_test.c check.h $(SRCS)
	$(CC) $(CFLAGS) -o $@ config_test.c $(SRCS) $(LDLIBS)

check: $(TESTS)
	@rc=0; \
	for t in $(TESTS); do \
		echo "=== $$t ==="; \
		./$$t || rc=1; \
		echo; \
	done; \
	exit $$rc

clean:
	$(RM) $(TESTS)

.PHONY: all check clean
