# viewpoint

A terminal multiplexer with a desktop-window-manager metaphor for Linux.

`viewpoint` presents floating, overlapping, WIMPy windows on your terminal. Each window runs its own shell (or any program) in a dedicated PTY, complete with the amazing 1990s innovations in window chrome: draggable borders, a title bar with minimize/maximize/close buttons, a desktop background, and a taskbar. You move, resize, raise, snap, minimize and maximize windows with the mouse or keyboard, similarly as on a graphical desktop.

---


## Build and dependencies

```sh
make
```
You need the development headers and libraries for notcurses, libvterm and GPM, plus a C toolchain with `forkpty` (from libutil, part of glibc).

On Arch Linux:

```sh
sudo pacman -S notcurses libvterm gpm base-devel
```

(`base-devel` provides `gcc`, `make` and `pkg-config`; `libutil`/`forkpty` ship with glibc.)

notcurses and libvterm are located via `pkg-config`. GPM has no `.pc` file, so it is linked directly with `-lgpm`.

## Run

```sh
./viewpoint
```

It starts with two overlapping shell windows. Closing the last window exits cleanly; you can also just close every window.

viewpoint expects to fully own the terminal. Running viewpoint inside another multiplexer will not work and is not supported. Running another multiplexer inside viewpoint *may* work but is not supported.

## Usage

### Modes: INTERPRET / PASSTHROUGH

viewpoint has one global toggle that flips between two modes:

- INTERPRET (default): window-manager chords (below) are handled by viewpoint; everything else is forwarded to the focused window's program.
- PASSTHROUGH: every keystroke (except the toggle key) is forwarded to the focused program

The toggle key, `F12`, works in both modes and is never forwarded to a program. The current mode is shown both on the taskbar (`INTERPT [K]` / `PASSTHRU [P]`) and on each window's title bar (`[K]` / `[P]`); clicking either indicator toggles the mode too.

We try to support all reasonably behaved terminals, but some deliver certain key combinations oddly (or never get them due to the DE capturing them, e.g. `Alt`+`Tab`).

### Keymap

All chords below are active in INTERPRET mode (the toggle itself works in both modes). 

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
| Click an unfocused window                     | Focus / raise it                         |
| Click the title-bar `[K]`/`[P]` indicator     | Toggle global mode                       |
| Click `[_]` / `[▢]` / `[x]` title buttons     | Minimize / maximize / close              |
| Drag the title bar                            | Move the window                          |
| Drag a window border or corner                | Resize the window                        |
| Drag the title bar to a screen edge / corner  | Snap to half / quarter (outline preview shown; applied on release) |
| Click a taskbar slot                          | Focus, or restore if minimized           |
| Click the taskbar mode region                 | Toggle global mode                       |

## Development notes

Written in C for Linux.

It is built on three libraries:

- [notcurses](https://github.com/dankamongmen/notcurses) for rendering and compositing. The stack of `ncplane`s is the window stack; z-order is the source of truth for which window is on top.
- [libvterm](https://www.leonerd.org.uk/code/libvterm/) for terminal emulation. Every window has its own `VTerm`. We stay out of the weeds of manual VT parsing.
- [GPM](https://www.nico.schottelius.org/software/gpm/) for mouse support on the bare Linux console. On a graphical terminal emulator, notcurses'own mouse decoding is used instead.

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

### Debug log

Set `VP_DEBUG` to a file path to capture an internal event trace (focus changes, geometry, mode toggles, spawns/closes):

```sh
VP_DEBUG=/tmp/viewpoint.log ./viewpoint
```

### Miscellaneous

The toggle key is a single `#define VP_TOGGLE_KEY` in `viewpoint.h`. The rest of the keymap lives in a single table (`g_keymap[]` in `input.c`).

On the bare Linux console, mouse input comes from `gpm`. In a graphical terminal, notcurses decodes the mouse. Only one source is used at a time.

### Todo

- Fix numerous bugs and UI polish issues
- Far in the future, but keep abstractions clean for: inner-application sixel passthrough, viewpoint-native sixel widgets, and `NCBLIT_PIXEL` graphics.

---

This project is licensed under the GNU General Public License v3.0. See the LICENSE file for details.

Although I (robinpie) personally don't really like the GPLv3, this project links to both Apache 2.0 code (notcurses) and GPLv2+ code (GPM), so GPLv3 is our only option.
