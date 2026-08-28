# Test helpers

```sh
make -C tests check      # everything below, after a `make` in the root
```

Three separate things live here.

## `vpdrive.py` — driving the real app

A Python harness that runs the built `viewpoint` on a pty and reads back what it
drew. It never links project code, so what it reports is what a user would see.

```python
from vpdrive import Session, DOWN, RIGHT, ENTER

with Session() as vp:
    vp.open_settings()                       # click the desktop icon
    vp.send(DOWN + RIGHT + ENTER)            # into the Animations tile
    scr = vp.wait_text("Size indicator fade-out")
    print(scr)                               # the whole screen, as text
    print(vp.read_config())                  # what it saved
```

Two things make this harder than piping bytes at the binary, and the harness
exists to handle both:

- **notcurses interrogates the terminal at startup** — cursor position, device
  attributes, palette entries — and blocks until it gets answers. A pipe never
  answers, so the app hangs before drawing anything. `Session.respond()` plays
  the terminal's part.
- **viewpoint keeps a detached session daemon** holding live windows, keyed on
  `$XDG_RUNTIME_DIR`. Sharing yours means inheriting whatever windows a previous
  run left behind, so every `Session` gets a private runtime dir *and* a private
  `$XDG_CONFIG_HOME` — a run can neither see your desktop nor touch your config.
  Because the daemon `setsid()`s out of reach, `stop()` hunts it down by that
  private path.

Useful pieces:

| | |
|---|---|
| `Session(rows, cols, config=…)` | a running app; `config=` seeds `viewpoint.conf` |
| `vp.send(bytes)`, `vp.click(y, x)` | input; `arrow()`, `alt()`, `click()` build the sequences |
| `vp.wait_text(s)`, `vp.wait_for(pred)` | wait for the screen to say something, rather than sleeping |
| `vp.settle()` | wait for it to stop redrawing |
| `vp.screen` | the `Screen`: `.text()`, `.line(r)`, `.find(s)`, `.fg[r][c]`, `.bg[r][c]` |
| `vp.read_config()` | the config file as the app has written it |

The screen model keeps **per-cell colors**, which matters more than it sounds:
terminals have no alpha, so viewpoint's only fade is a color ramp, and a harness
that could read text alone would be blind to it.

Waiting is by predicate, not by sleep — the app redraws when it has something to
say, and a fixed delay is either slower than it needs to be or flaky under load.

## `ui_shots.py`, `fade_probe.py` — worked examples

Both are runnable checks and both double as the documentation for `vpdrive`.

- `ui_shots.py` walks the settings panel: opens it, checks every tile is on the
  grid, flips the size-indicator switch, and confirms the choice reached
  `viewpoint.conf`. `--quiet` prints only the checks, not the screens.
- `fade_probe.py` resizes a window and samples the size indicator's border color
  over its whole life, with the fade both on and off. Run it to *see* the ramp:

  ```
  ----- fade ON ------      ----- fade OFF -----
    t+  21ms rgb(96,128,192)  t+  22ms rgb(96,128,192)
    t+ 674ms rgb(88,117,175)  t+ 615ms (box gone)
    …8 more shades…
    t+1128ms (box gone)
  ```

## `unit/` — checks over viewpoint's own functions

The one corner that reaches inside the project, because that is the point of it.
It keeps the sibling Makefile's promise about stale objects a different way:
sources are compiled straight into each test binary and the root build's `.o`
files are never touched.

- `fade_test.c` — the size indicator's clock. `#include`s `sizeosd.c` to reach
  the file-static helpers, then winds the "when did the box appear" timestamp
  back and asks what it thinks at each point in the timeline.
- `config_test.c` — a setting's round trip through `config.c`: default, parse,
  a write that stays quiet at the default, a reload that agrees, and an honest
  answer about whether the manual section shadows it. A new setting can copy the
  shape.

`check.h` is the whole framework: one macro and a counter.

## `resize.sh` and friends

`winsize_probe.c`, `ttywinsz.c`, `ptycontrol.c` and `resize.sh` are the older
X11-driven resize investigation — they need a bunch of the X11 utilities, and
`resize.sh` is currently commented out. Still largely purpose specific. Not a
good or wide test harness yet lol
