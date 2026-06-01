viewpoint is a terminal multiplexer with a desktop-window-manager metaphor for Linux.

viewpoint presents floating windows in a WIMPy interface on your terminal. Each window runs its own shell (or any program) in a dedicated PTY, but with the amazing 1990s innovations in window chrome: draggable borders, a title bar with minimize/maximize/close buttons, a desktop background, and a taskbar. You move, resize, raise, snap, minimize and maximize windows with the mouse or keyboard, similarly as on a graphical desktop.

---

## Build and dependencies

```sh
make
```
You need the development headers and libraries for notcurses, libvterm and GPM, plus a C toolchain with `forkpty` (from libutil, part of glibc).

On Arch Linux: `sudo pacman -S notcurses libvterm gpm base-devel`

notcurses and libvterm are located via `pkg-config`. GPM has no `.pc` file, so it is linked directly with `-lgpm`. For a mouse on the bare Linux console you also need the `gpm` daemon running.

## Run

```sh
./viewpoint
```
viewpoint expects to fully own the terminal. Running viewpoint inside another multiplexer will not work and is not supported. Running another multiplexer inside viewpoint *may* work but is not supported.

## Usage

### Modes

viewpoint has one global toggle that flips between two modes:

- INTERPRET (default): window-manager chords (below) are handled by viewpoint; everything else is forwarded to the focused window's program.
- PASSTHROUGH: every keystroke (except the toggle key) is forwarded to the focused program

The toggle key works in both modes and is never forwarded to a program (except the settings menu). The current mode is shown on on the taskbar (`INTERPRET` / `PASSTHROUGH`).

We try to support all reasonably behaved terminals, but some deliver certain key combinations oddly (or never get them due to the DE capturing them, e.g. `Alt`+`Tab`).

### Keymap

These are the default chords active in INTERPRET mode.

| Key                           | Action                                   |
|-------------------------------|------------------------------------------|
| `F12`                         | Toggle INTERPRET ↔ PASSTHROUGH mode      |
| `Alt`+`Tab`                   | Focus next window                        |
| `Alt`+`Shift`+`Tab`           | Focus previous window                    |
| `Alt`+`n`                     | New window                               |
| `Alt`+`F4`                    | Close focused window                     |
| `Alt`+`m`                     | Minimize focused window                  |
| `Alt`+`x`                     | Toggle maximize of focused window        |
| `Alt`+`1` … `Alt`+`9`         | Focus / restore window in taskbar slot N |
| `Alt`+`←`/`→`/`↑`/`↓`         | Move focused window                      |
| `Alt`+`Shift`+`←`/`→`/`↑`/`↓` | Resize focused window                    |
| `Alt`+`,` / `Alt`+`.`         | Scroll the taskbar left / right (when more windows are open than fit) |

### Mouse

| Action                                        | Effect                                   |
|-----------------------------------------------|------------------------------------------|
| Click an unfocused window                     | Focus / raise it                         |
| Click `[_]` / `[▢]` / `[x]` title buttons     | Minimize / maximize / close              |
| Drag the title bar                            | Move the window                          |
| Drag a window border or corner                | Resize the window                        |
| Drag the title bar to a screen edge / corner  | Snap to half / quarter (outline preview shown; applied on release) |
| Click a taskbar slot                          | Focus, or restore if minimized           |
| Horizontal scroll / click the `◄` `►` arrows  | Scroll the taskbar when more windows are open than fit |
| Click the taskbar mode region                 | Toggle global mode                       |
| Click the `⚙ Settings` icon (desktop top-left)| Open the settings menu                   |
| Click the `⏻ Exit` icon (desktop bottom-right)| Quit viewpoint                           |
| Drag a desktop icon (`⚙` / `⏻`)               | Move it; its position is remembered across restarts |

## Configuration

viewpoint reads an optional config file from `$XDG_CONFIG_HOME/viewpoint/viewpoint.conf`.

### In-app keybinding editor

The quickest way to rebind keys is the `⚙ Settings` icon at the top-left of the
desktop. Clicking it opens a modal editor listing every action with its current
chord (and the mode-toggle key):

- `↑`/`↓` (or click a row) to select an action
- `Enter` (or click the row again) then press the chord you want. Changes take effect instantly.
- `D` / `Delete` to unbind the selected action
- `S` to save now, `Esc` (or click outside the panel) to close

If a change you make in the editor is governed by your manual config section, the status line warns you, but the manual section wins, so that change won't survive a restart.

### Configuration file

The config file has two parts:

```conf
# viewpoint config

# in-app/automatically set configuration:
# ...the editor manages this section, don't edit it by hand...
# manual configuration:
# ...your hand-written settings live here and are preserved verbatim...
```

### Editing the file by hand

It is a line-oriented `key = value` file; `#` starts a comment and blank lines are ignored. 

```conf
# Add or change a chord:  bind = <chord> <action>
bind = alt+enter new
bind = alt+q     close

# Drop a default chord:
unbind = alt+f4

# Change the always-on mode-toggle key (default f12):
toggle = f12

# Pin a desktop icon's top-left corner:  icon = <name> <x> <y>
# (names: settings, exit. Normally written automatically when you drag an icon.)
icon = settings 1 1
icon = exit 60 20
```

A chord is an optional `alt+`/`shift+`/`ctrl+` modifier prefix followed by a key: a single character (`n`, `x`), a function key (`f1`…`f60`), or a named key (`tab`, `enter`, `esc`, `space`, `left`/`right`/`up`/`down`, `home`, `end`, `pgup`, `pgdn`, `delete`, `insert`, `backspace`).

The available actions are: `focus_next`, `focus_prev`, `new`, `close`, `minimize`, `maximize`, `move_left`/`move_right`/`move_up`/`move_down`, `resize_left`/`resize_right`/`resize_up`/`resize_down`, `slot1`…`slot9` (focus/restore the window in taskbar slot N), and `taskbar_left`/`taskbar_right` (scroll the taskbar's window slots).

## Acknowledgements

This project uses the following libraries:

- [notcurses](https://github.com/dankamongmen/notcurses)
- [libvterm](https://www.leonerd.org.uk/code/libvterm/)
- [GPM](https://www.nico.schottelius.org/software/gpm/)

---

This project is licensed under the GNU General Public License v3.0. See the LICENSE file for details.
