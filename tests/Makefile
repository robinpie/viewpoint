# Test helpers for viewpoint. Deliberately independent of the main build: these
# link nothing from the project and nothing the project links, so a harness run
# can never be confused by a stale object file.

CC     ?= cc
CFLAGS += -std=gnu11 -Wall -Wextra -O1 -g

BINS = winsize_probe ttywinsz ptycontrol

all: $(BINS)

winsize_probe: winsize_probe.c
	$(CC) $(CFLAGS) -o $@ $<

ttywinsz: ttywinsz.c
	$(CC) $(CFLAGS) -o $@ $<

# forkpty lives in libutil, same as the main build.
ptycontrol: ptycontrol.c
	$(CC) $(CFLAGS) -o $@ $< -lutil

clean:
	$(RM) $(BINS)

.PHONY: all clean
