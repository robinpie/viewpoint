# viewpoint

A terminal multiplexer with a **desktop-window-manager metaphor**, written in C
for Linux.

`viewpoint` is a single-process, single-threaded program that presents floating,
overlapping *windows* on your terminal. Each window runs its own shell (or any
program) in a dedicated PTY, complete with window chrome — borders, a title bar
with minimize/maximize/close buttons, a desktop background, and a taskbar. You
move, resize, raise, snap, minimize and maximize windows with the keyboard or the
mouse, exactly as you would on a graphical desktop, except it's all drawn with
box-drawing characters in your terminal.

It is built on three libraries that each own one hard problem:

- **[notcurses](https://github.com/dankamongmen/notcurses)** — rendering and
  compositing. The stack of `ncplane`s *is* the window stack; z-order is the
  source of truth for which window is on top.
- **[libvterm](https://www.leonerd.org.uk/code/libvterm/)** — terminal
  emulation. Every window has its own `VTerm`; viewpoint contains no VT parser
  of its own.
- **[GPM](https://www.nico.schottelius.org/software/gpm/)** — mouse support on
  the bare Linux console (`gpm`). On a graphical terminal emulator, notcurses'
  own mouse decoding is used instead.

The whole thing runs on one `poll(2)` loop. There are no threads.

---

## Dependencies

You need the development headers and libraries for notcurses, libvterm and GPM,
plus a C toolchain with `forkpty` (from libutil, part of glibc).

On Arch Linux:

```sh
sudo pacman -S notcurses libvterm gpm base-devel
```

(`base-devel` provides `gcc`, `make` and `pkg-config`; `libutil`/`forkpty` ship
with glibc.)

notcurses and libvterm are located via `pkg-config`. GPM has no `.pc` file, so it
is linked directly with `-lgpm`.

## Build

```sh
make
```

This compiles with `-Wall -Wextra` and is warning-clean. `make clean` removes the
binary and object files.

## Run

```sh
./viewpoint
```

It starts with two overlapping shell windows. Closing the last window exits
cleanly; you can also just close every window.

> **Note:** viewpoint expects to own the terminal directly. Running it **inside
> `tmux` or `screen` is not supported** — those multiplexers intercept the very
> escape sequences and mouse reports viewpoint relies on. Run it in a real
> terminal emulator (or on a bare Linux VT with `gpm` running).

### Debug log

Set `VP_DEBUG` to a file path to capture an internal event trace (focus changes,
geometry, mode toggles, spawns/closes):

```sh
VP_DEBUG=/tmp/viewpoint.log ./viewpoint
```

---

## Modes: INTERPRET ↔ PASSTHROUGH

viewpoint has one global toggle that flips between two modes:

- **INTERPRET** (default): window-manager chords (below) are handled by
  viewpoint; everything else is forwarded to the focused window's program.
- **PASSTHROUGH**: *every* keystroke is forwarded to the focused program — useful
  when an inner application needs the same keys viewpoint would otherwise grab.

The toggle key — **F12** — works in **both** modes and is **never** forwarded to a
program. It is a single `#define VP_TOGGLE_KEY` in `viewpoint.h`. The current mode
is shown both on the taskbar (`INTERPT [K]` / `PASSTHRU [P]`) and on each window's
title bar (`[K]` / `[P]`); clicking either indicator toggles the mode too.

> Modifier-chord reliability is best in terminals that support the
> **Kitty keyboard protocol**. On a bare VT, `Alt`-combinations may be delivered
> inconsistently by the terminal — this is a terminal limitation, not a crash.

---

## Keymap

All chords below are active in **INTERPRET** mode (the toggle itself works in both
modes). The keymap lives in a single table (`g_keymap[]` in `input.c`).

| Key                       | Action                                   |
|---------------------------|------------------------------------------|
| `F12`                     | Toggle INTERPRET ↔ PASSTHROUGH mode      |
| `Alt`+`Tab`               | Focus next window                        |
| `Alt`+`Shift`+`Tab`       | Focus previous window                    |
| `Alt`+`n`                 | New window                               |
| `Alt`+`F4`                | Close focused window                     |
| `Alt`+`m`                 | Minimize focused window                  |
| `Alt`+`x`                 | Toggle maximize of focused window        |
| `Alt`+`1` … `Alt`+`9`     | Focus / restore window in taskbar slot N |
| `Alt`+`←`/`→`/`↑`/`↓`     | Move focused window                      |
| `Alt`+`Shift`+`←`/`→`/`↑`/`↓` | Resize focused window               |

### Mouse

| Action                                        | Effect                                   |
|-----------------------------------------------|------------------------------------------|
| Click a window                                | Focus / raise it                         |
| Click the title-bar `[K]`/`[P]` indicator     | Toggle global mode                       |
| Click `[_]` / `[▢]` / `[x]` title buttons     | Minimize / maximize / close              |
| Drag the title bar                            | Move the window                          |
| Drag a window border or corner                | Resize the window                        |
| Drag the title bar to a screen edge / corner  | Snap to half / quarter (outline preview shown; applied on release) |
| Click / scroll inside a window's content      | Forwarded to the program (if it enables mouse reporting) |
| Click a taskbar slot                          | Focus, or restore if minimized           |
| Click the taskbar mode region                 | Toggle global mode                       |

On the bare Linux console, mouse input comes from `gpm`; in a graphical terminal,
notcurses decodes the mouse. Only one source is used at a time.

---

## Architecture

| File          | Responsibility                                                        |
|---------------|-----------------------------------------------------------------------|
| `main.c`      | Init + the single `poll(2)` event loop; window teardown/reaping        |
| `pty.c`       | `forkpty()` a child shell; window-size ioctls                          |
| `vt_bridge.c` | libvterm ↔ notcurses bridge: feed PTY bytes, render the grid, forward keys/mouse |
| `window.c`    | Per-window lifecycle and frame (border + title bar) drawing            |
| `wm.c`        | Window list, focus, z-order, layout (move/resize/min/max), render pass |
| `input.c`     | Keyboard chords vs. forwarding; the unified mouse model; snapping; GPM |
| `taskbar.c`   | Bottom taskbar: per-window slots + mode indicator                      |
| `viewpoint.h` | Shared declarations and tunables                                       |

### Notes / deferred work

The abstractions are kept clean for, but the following are intentionally **not**
implemented yet: inner-application sixel passthrough, custom pixel/bitmap widgets,
and `NCBLIT_PIXEL` graphics. Scrollback is not retained.
