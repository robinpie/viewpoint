# Test helpers for viewpoint. Deliberately independent of the main build: these
# link nothing from the project and nothing the project links, so a harness run
# can never be confused by a stale object file. The same goes for the Python
# harness (vpdrive.py), which drives the built binary from outside as a user
# would. The one corner that must reach inside the project is unit/, which keeps
# the promise a different way - see its Makefile.

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

# The black-box scripts drive ../viewpoint, so it has to be built first.
check: all
	@$(MAKE) -C unit check
	@echo "=== ui_shots ==="   && python3 ui_shots.py --quiet
	@echo "=== fade_probe ===" && python3 fade_probe.py
	@echo "=== select_probe ===" && python3 select_probe.py --quiet

clean:
	$(RM) $(BINS)
	@$(MAKE) -C unit clean

.PHONY: all check clean
