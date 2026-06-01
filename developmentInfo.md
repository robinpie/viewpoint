## Development notes

Written in C for Linux.

Set the `VP_DEBUG` environment variable to a file path to see parse errors and the resolved config path.

It is built on three libraries:

- [notcurses](https://github.com/dankamongmen/notcurses) for rendering and compositing. The stack of `ncplane`s is the window stack; z-order is the source of truth for which window is on top.
- [libvterm](https://www.leonerd.org.uk/code/libvterm/) for terminal emulation. Every window has its own `VTerm`. We stay out of the weeds of manual VT parsing.
- [GPM](https://www.nico.schottelius.org/software/gpm/) for mouse support on the bare Linux console. On a graphical terminal emulator, notcurses' own mouse decoding is used instead. The two are mutually exclusive (see below).

## Architecture

| File          | Responsibility                                                        |
|---------------|-----------------------------------------------------------------------|
| `main.c`      | Init + the single `poll(2)` event loop; window teardown/reaping        |
| `pty.c`       | `forkpty()` a child shell; window-size ioctls                          |
| `vt_bridge.c` | libvterm ↔ notcurses bridge: feed PTY bytes, render the grid, forward keys/mouse |
| `window.c`    | Per-window lifecycle and frame (border + title bar) drawing            |
| `wm.c`        | Window list, focus, z-order, layout (move/resize/min/max), render pass |
| `input.c`     | Keyboard chords vs. forwarding; the unified mouse model; snapping; GPM; keymap defaults + parsing |
| `taskbar.c`   | Bottom taskbar: per-window slots + mode indicator                      |
| `config.c`    | Load/save the config file (`viewpoint.conf`); the customizable keymap   |
| `settings.c`  | Desktop launcher icon + the modal in-app keybinding editor             |
| `viewpoint.h` | Shared declarations and tunables                                       |

### Debug log

Set `VP_DEBUG` to a file path to capture an internal event trace (focus changes, geometry, mode toggles, spawns/closes):

```sh
VP_DEBUG=/tmp/viewpoint.log ./viewpoint
```

### Miscellaneous

The *default* toggle key is `#define VP_TOGGLE_KEY` in `viewpoint.h`, and the default keymap is a single table (`g_default_keymap[]` in `input.c`). At startup these seed `WM.config`, which the config file and the in-app editor then edit — so the live keymap is data in `WM.config`, not the table itself.

On the bare Linux console (`TERM=linux`, tracked as `WM.console`), mouse input comes from our own GPM connection (`gpm_setup`/`gpm_pump` in `input.c`), opened with the full event mask so even bare hover motion is delivered. In a graphical terminal, notcurses decodes the mouse and we leave GPM alone. `main()` enables exactly one of the two.

These two sources must never both be active: notcurses also links libgpm, so enabling its mice on the console would open a second libgpm client inside our process, and the two would collide over GPM's shared global connection state. (That collision was a real bug — mouse events would stop reaching the WM entirely.)

Because notcurses can't deliver bare hover-motion over GPM (it only forwards button and drag events on the console), the console mouse must be ours: that's what lets the software pointer track an empty-handed hover. The pointer itself is a one-cell `ncplane` (`WM.cursor`) kept on top and moved to the latest mouse cell each render; a GUI terminal draws its own hardware cursor, so we only draw ours when `WM.console`.

### Todo

- Fix numerous bugs and UI polish issues
- Far in the future, but keep abstractions clean for: inner-application sixel passthrough, viewpoint-native sixel widgets, and `NCBLIT_PIXEL` graphics.
