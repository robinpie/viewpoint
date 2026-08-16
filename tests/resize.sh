# #!/usr/bin/env bash
# # SPDX-License-Identifier: GPL-3.0-only
# # Copyright (C) 2026  robinpie <robin413@protonmail.com>
# #
# #   viewpoint  the "geom" lines in its own debug log - what the WM intended
# #   kernel     TIOCGWINSZ on the app's pty, read from outside via /proc
# #   app        the probe's log of every size it was told about
# #
# # Usage: tests/resize.sh [options]
# #   --app probe|probe-alt|htop   what to run in the window (default probe-alt)
# #   --resize max|keys|drag|all   how to resize it (default all)
# #   --keep                       leave the stack running for poking at
# #   --outdir DIR                 where artifacts land (default a fresh tmpdir)
# set -uo pipefail
# 
# HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# ROOT=$(dirname "$HERE")
# 
# APP=probe-alt
# RESIZE=all
# KEEP=0
# OUTDIR=
# 
# while [ $# -gt 0 ]; do
# 	case "$1" in
# 	--app) APP=$2; shift 2 ;;
# 	--resize) RESIZE=$2; shift 2 ;;
# 	--keep) KEEP=1; shift ;;
# 	--outdir) OUTDIR=$2; shift 2 ;;
# 	-h|--help) sed -n '4,36p' "$0"; exit 0 ;;
# 	*) echo "unknown option: $1" >&2; exit 2 ;;
# 	esac
# done
# 
# for tool in Xvfb xterm xdotool import; do
# 	command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 3; }
# done
# 
# # ---------------------------------------------------------------- workspace --
# 
# if [ -z "$OUTDIR" ]; then
# 	OUTDIR=$(mktemp -d "${TMPDIR:-/tmp}/viewpoint-resize.XXXXXX")
# fi
# mkdir -p "$OUTDIR" "$OUTDIR/home" "$OUTDIR/config"
# 
# # The session socket is $XDG_RUNTIME_DIR/viewpoint.sock and sockaddr_un.sun_path
# # is 108 bytes, so the runtime dir cannot live under an arbitrarily deep
# # --outdir: session_path() would truncate, session_connect() would fail, and
# # viewpoint would exit before it ever drew a window. Keep it short and shallow.
# RUNDIR=$(mktemp -d /tmp/vprz.XXXXXX)
# chmod 700 "$RUNDIR"
# 
# VPLOG="$OUTDIR/viewpoint.log"
# PROBELOG="$OUTDIR/probe.log"
# REPORT="$OUTDIR/report.txt"
# : >"$REPORT"
# 
# say() { printf '%s\n' "$*" | tee -a "$REPORT"; }
# hr()  { say "--------------------------------------------------------------------"; }
# 
# # ------------------------------------------------------------------ display --
# 
# # Terminal geometry. Big enough that "maximised" is a large, unmistakable jump
# # from the two-thirds-size window viewpoint opens by default.
# COLS=200
# ROWS=54
# 
# pick_display() {
# 	for n in $(seq 90 120); do
# 		[ -e "/tmp/.X11-unix/X$n" ] || { echo "$n"; return; }
# 	done
# 	echo "no free display" >&2; exit 3
# }
# DISP=":$(pick_display)"
# 
# XVFB_PID= ; XTERM_PID= ; DAEMON_PIDS=
# 
# cleanup() {
# 	local rc=$?
# 	if [ "$KEEP" = 1 ]; then
# 		say ""
# 		say "--keep given; stack left running on DISPLAY=$DISP"
# 		say "  screenshot: DISPLAY=$DISP import -window root shot.png"
# 		say "  tear down : kill $XTERM_PID $XVFB_PID ${DAEMON_PIDS:-}"
# 		exit $rc
# 	fi
# 	# The session daemon setsid()s away from us, so it is not in our process
# 	# group and will outlive the xterm. Find it by the private
# 	# XDG_RUNTIME_DIR it inherited rather than by pattern-matching a command
# 	# line, which would also match this script.
# 	local d
# 	d=$(daemon_pids)
# 	[ -n "${XTERM_PID:-}" ] && kill "$XTERM_PID" 2>/dev/null
# 	sleep 0.3
# 	for p in $d; do kill "$p" 2>/dev/null; done
# 	sleep 0.2
# 	for p in $d; do kill -9 "$p" 2>/dev/null; done
# 	[ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
# 	wait 2>/dev/null
# 	rm -rf "$RUNDIR"
# 	exit $rc
# }
# 
# # Any process holding our private XDG_RUNTIME_DIR in its environment. That is
# # the daemon and every shell it spawned, and nothing else on the machine.
# #
# # The marker is deliberately never exported into this script's own environment
# # (see VPENV below) - it is handed to xterm and inherited from there. A pattern
# # broad enough to match the harness is a pattern that makes cleanup kill the
# # harness, so self and parent are excluded regardless.
# daemon_pids() {
# 	local p pid out=
# 	for p in /proc/[0-9]*; do
# 		pid=${p#/proc/}
# 		[ "$pid" = "$$" ] && continue
# 		[ "$pid" = "$PPID" ] && continue
# 		# The read has to be guarded as a block: a bare redirect from an
# 		# unreadable /proc entry is the shell's error, not tr's, so
# 		# tr's own 2>/dev/null would not silence it.
# 		if { tr '\0' '\n' <"$p/environ"; } 2>/dev/null |
# 			grep -qxF "XDG_RUNTIME_DIR=$RUNDIR"; then
# 			out="$out $pid"
# 		fi
# 	done
# 	echo "$out"
# }
# trap cleanup EXIT INT TERM
# 
# echo "workspace: $OUTDIR"
# echo "display:   $DISP"
# 
# # 24bpp so sixel and truecolour behave as they do on a real desktop.
# Xvfb "$DISP" -screen 0 2400x1400x24 -nolisten tcp >"$OUTDIR/xvfb.log" 2>&1 &
# XVFB_PID=$!
# for _ in $(seq 50); do
# 	DISPLAY=$DISP xdotool getdisplaygeometry >/dev/null 2>&1 && break
# 	sleep 0.1
# done
# DISPLAY=$DISP xdotool getdisplaygeometry >/dev/null 2>&1 || { echo "Xvfb never came up" >&2; exit 3; }
# 
# # ------------------------------------------------------------------- build ---
# 
# make -C "$ROOT" >"$OUTDIR/build.log" 2>&1 || { echo "build failed, see $OUTDIR/build.log" >&2; exit 3; }
# make -C "$HERE" >>"$OUTDIR/build.log" 2>&1 || { echo "test build failed, see $OUTDIR/build.log" >&2; exit 3; }
# 
# # ---------------------------------------------------------------- launch -----
# 
# # -b 0 removes xterm's internal border so cell size is exactly window/geometry,
# # which the drag path below needs to turn viewpoint cell coordinates into
# # pixels. -ti vt340 is what makes xterm advertise sixel support.
# #
# # The environment is passed to xterm rather than exported, so this script's own
# # process never carries the XDG_RUNTIME_DIR marker that cleanup hunts for.
# VPENV=(
# 	"DISPLAY=$DISP"
# 	"XDG_RUNTIME_DIR=$RUNDIR"
# 	"XDG_CONFIG_HOME=$OUTDIR/config"
# 	"HOME=$OUTDIR/home"
# 	"SHELL=/bin/sh"
# 	"VP_DEBUG=$VPLOG"
# 	"TERM=xterm-256color"
# )
# 
# env "${VPENV[@]}" xterm -ti vt340 -b 0 +sb \
# 	-fa "DejaVu Sans Mono" -fs 10 \
# 	-geometry "${COLS}x${ROWS}+0+0" \
# 	-title vp-harness \
# 	-e /bin/sh -c "exec '$ROOT/viewpoint' 2>'$OUTDIR/viewpoint.stderr'" \
# 	>"$OUTDIR/xterm.log" 2>&1 &
# XTERM_PID=$!
# 
# WID=
# for _ in $(seq 100); do
# 	WID=$(DISPLAY=$DISP xdotool search --name '^vp-harness$' 2>/dev/null | head -1)
# 	[ -n "$WID" ] && break
# 	sleep 0.1
# done
# if [ -z "$WID" ]; then
# 	# viewpoint's stdout is the pty notcurses drives, so anything it has to
# 	# say about why it gave up is on stderr, in its own file.
# 	echo "xterm window never appeared" >&2
# 	[ -s "$OUTDIR/viewpoint.stderr" ] &&
# 		{ echo "viewpoint said:" >&2; sed 's/^/  /' "$OUTDIR/viewpoint.stderr" >&2; }
# 	exit 3
# fi
# DISPLAY=$DISP xdotool windowfocus "$WID" 2>/dev/null
# DISPLAY=$DISP xdotool windowraise "$WID" 2>/dev/null
# 
# for _ in $(seq 100); do
# 	grep -q 'pixel support' "$VPLOG" 2>/dev/null && break
# 	sleep 0.1
# done
# sleep 2
# 
# # ------------------------------------------------------------- helpers -------
# 
# shot() { DISPLAY=$DISP import -window root "$OUTDIR/$1.png" 2>/dev/null; }
# 
# # XTEST rather than "xdotool key --window": synthetic events sent to a specific
# # window arrive with send_event set and xterm drops those. XTEST events are
# # indistinguishable from real ones, so this exercises the same path a keypress
# # from a human does.
# #
# # Do not "fix" the above by setting XTerm*allowSendEvents instead. It makes
# # --window work, and it silently stops xterm reporting the mouse to the
# # application, so every drag in this harness becomes a no-op that looks exactly
# # like the bug under test.
# focus() { DISPLAY=$DISP xdotool windowfocus "$WID" 2>/dev/null; DISPLAY=$DISP xdotool windowraise "$WID" 2>/dev/null; }
# key()   { focus; DISPLAY=$DISP xdotool key --clearmodifiers "$@"; sleep 0.35; }
# typ()   { focus; DISPLAY=$DISP xdotool type --delay 12 "$1"; sleep 0.2; }
# 
# # xterm handles modified clicks itself - shift+click is its own selection
# # override - and swallows them instead of reporting them to the application. A
# # modifier still latched from a preceding alt+... chord therefore makes the
# # whole drag vanish before viewpoint ever sees it.
# clear_mods() {
# 	DISPLAY=$DISP xdotool keyup alt shift ctrl super 2>/dev/null
# 	sleep 0.2
# }
# 
# # The last geometry viewpoint logged for a given window id.
# vp_geom() { grep "^geom id=$1 " "$VPLOG" 2>/dev/null | tail -1; }
# vp_field() { vp_geom "$1" | sed -n "s/.* $2=\([-0-9]*\).*/\1/p"; }
# 
# # Every pid under our private daemon, deepest first, so the app running inside
# # the window is found before the shell that launched it.
# app_pid() {
# 	local want=$1 p comm
# 	for p in $(daemon_pids); do
# 		[ -r "/proc/$p/comm" ] || continue
# 		comm=$(cat "/proc/$p/comm" 2>/dev/null)
# 		[ "$comm" = "$want" ] && { echo "$p"; return; }
# 	done
# }
# 
# # A blocked signal is inherited across fork *and* exec, so if SIGWINCH is
# # blocked in the app it was blocked by some ancestor and handed down. Walking
# # the whole tree shows exactly which process introduced it.
# SIGWINCH_BIT=$((1 << 27)) # signal 28, and /proc numbers the bits from 0
# 
# sigwinch_blocked() {
# 	local blk
# 	blk=$(sed -n 's/^SigBlk:\s*//p' "/proc/$1/status" 2>/dev/null)
# 	[ -n "$blk" ] || return 2
# 	(( 0x$blk & SIGWINCH_BIT ))
# }
# 
# mask_table() {
# 	say ""
# 	say "[SIGWINCH mask down the process tree]"
# 	say "      pid    ppid  command           SIGWINCH"
# 	local p ppid comm state
# 	for p in $(daemon_pids); do
# 		[ -r "/proc/$p/status" ] || continue
# 		comm=$(cat "/proc/$p/comm" 2>/dev/null)
# 		ppid=$(sed -n 's/^PPid:\s*//p' "/proc/$p/status" 2>/dev/null)
# 		if sigwinch_blocked "$p"; then
# 			state="BLOCKED"
# 		elif [ $? = 2 ]; then
# 			state="?"
# 		else
# 			state="deliverable"
# 		fi
# 		printf '  %7s %7s  %-16s  %s\n' "$p" "$ppid" "$comm" "$state" |
# 			tee -a "$REPORT"
# 	done
# }
# 
# report_sizes() {
# 	local phase=$1 pid=$2
# 	local x y w h k
# 	x=$(vp_field "$WINID" x); y=$(vp_field "$WINID" y)
# 	w=$(vp_field "$WINID" w); h=$(vp_field "$WINID" h)
# 	say ""
# 	say "[$phase]"
# 	if [ -n "$w" ]; then
# 		# The frame is 1 cell of border on every side.
# 		say "  viewpoint window   : ${h}h x ${w}w cells at ($x,$y)"
# 		say "  => content should be: $((h - 2))h x $((w - 2))w"
# 	else
# 		# window_set_geometry is what logs "geom", so a window that has
# 		# not been moved or resized yet has never logged one.
# 		say "  viewpoint window   : <not logged until first geometry change>"
# 	fi
# 	if [ -n "$pid" ]; then
# 		k=$("$HERE/ttywinsz" "$pid" 2>/dev/null)
# 		say "  kernel pty winsize : $(echo "$k" | awk '{print $2"h x "$3"w  ("$4")"}')"
# 	else
# 		say "  kernel pty winsize : <app pid not found>"
# 	fi
# 	if [ -f "$PROBELOG" ]; then
# 		say "  app observed size  : $(grep -v '^#' "$PROBELOG" | tail -1 | awk '{print $3"  "$4"  ("$1"ms, "$2")"}')"
# 	fi
# }
# 
# # ------------------------------------------------------------- start app -----
# 
# shot 01-boot
# 
# case "$APP" in
# probe)      APPCMD="$HERE/winsize_probe --log $PROBELOG --label PROBE" ; APPCOMM=winsize_probe ;;
# probe-alt)  APPCMD="$HERE/winsize_probe --alt --log $PROBELOG --label PROBE-ALT" ; APPCOMM=winsize_probe ;;
# htop)       APPCMD="htop" ; APPCOMM=htop ;;
# *) echo "unknown --app: $APP" >&2; exit 2 ;;
# esac
# 
# typ "$APPCMD"
# key Return
# sleep 2
# 
# # Which window did it land in? The focused one is the newest, and viewpoint
# # names its windows by id starting at 1. Take the highest id it has logged.
# WINID=$(grep '^geom id=' "$VPLOG" | sed -n 's/^geom id=\([0-9]*\) .*/\1/p' | sort -n | tail -1)
# [ -n "$WINID" ] || WINID=2
# 
# APPPID=$(app_pid "$APPCOMM")
# 
# # window_set_geometry is the only thing that logs "geom", so a freshly spawned
# # window has never logged its own position and the drag arm has no corner to
# # aim at. Nudge it one step and back: net zero movement, but it leaves a geom
# # line behind. MOVE_STEP is 2 cells and the window starts well clear of both
# # edges, so the round trip is exact rather than clamped.
# key alt+Right
# key alt+Left
# sleep 0.5
# 
# shot 02-app-started
# say "harness: app=$APP window id=$WINID app pid=${APPPID:-<none>}"
# hr
# mask_table
# report_sizes "before resize" "$APPPID"
# 
# # ------------------------------------------------------------- resizes -------
# 
# do_max() {
# 	hr
# 	say "resize method: alt+x (maximise toggle)"
# 	key alt+x
# 	sleep 1.5
# 	shot 03-maximised
# 	report_sizes "after alt+x maximise" "$APPPID"
# }
# 
# do_keys() {
# 	hr
# 	say "resize method: 12 x alt+shift+Right, 6 x alt+shift+Down"
# 	for _ in $(seq 12); do key alt+shift+Right; done
# 	for _ in $(seq 6); do key alt+shift+Down; done
# 	sleep 1.5
# 	shot 04-key-resized
# 	report_sizes "after keyboard resize" "$APPPID"
# }
# 
# do_drag() {
# 	hr
# 	say "resize method: mouse drag of the bottom-right corner"
# 	clear_mods
# 
# 	# xterm with -b 0: the cell grid exactly tiles the window, so cell size
# 	# is window size over the geometry we asked for.
# 	local geom wx wy ww wh cw ch
# 	geom=$(DISPLAY=$DISP xdotool getwindowgeometry --shell "$WID")
# 	eval "$geom"          # sets X, Y, WIDTH, HEIGHT
# 	wx=$X; wy=$Y; ww=$WIDTH; wh=$HEIGHT
# 	cw=$((ww / COLS)); ch=$((wh / ROWS))
# 	say "  xterm ${ww}x${wh}px at ($wx,$wy); cell ${cw}x${ch}px"
# 
# 	local x y w h px py
# 	x=$(vp_field "$WINID" x); y=$(vp_field "$WINID" y)
# 	w=$(vp_field "$WINID" w); h=$(vp_field "$WINID" h)
# 
# 	# Grab the bottom-right corner cell of the frame.
# 	px=$((wx + (x + w - 1) * cw + cw / 2))
# 	py=$((wy + (y + h - 1) * ch + ch / 2))
# 	say "  grabbing frame corner cell ($((x + w - 1)),$((y + h - 1))) at ${px},${py}px"
# 
# 	DISPLAY=$DISP xdotool mousemove "$px" "$py"; sleep 0.3
# 	DISPLAY=$DISP xdotool mousedown 1; sleep 0.3
# 	# Walk out in steps, the way a hand does - one TIOCSWINSZ per step.
# 	local i
# 	for i in $(seq 1 10); do
# 		DISPLAY=$DISP xdotool mousemove $((px + i * 4 * cw)) $((py + i * ch))
# 		sleep 0.12
# 	done
# 	sleep 0.4
# 	DISPLAY=$DISP xdotool mouseup 1
# 	sleep 1.5
# 	shot 05-drag-resized
# 	report_sizes "after mouse drag resize" "$APPPID"
# }
# 
# case "$RESIZE" in
# max)  do_max ;;
# keys) do_keys ;;
# drag) do_drag ;;
# all)  do_drag; do_keys; do_max ;;
# *) echo "unknown --resize: $RESIZE" >&2; exit 2 ;;
# esac
# 
# # ------------------------------------------------------------- verdict -------
# 
# hr
# say ""
# say "VERDICT"
# 
# X=$(vp_field "$WINID" x); Y=$(vp_field "$WINID" y)
# W=$(vp_field "$WINID" w); H=$(vp_field "$WINID" h)
# WANT_R=$((H - 2)); WANT_C=$((W - 2))
# 
# KROWS=; KCOLS=
# if [ -n "$APPPID" ]; then
# 	read -r _ KROWS KCOLS _ < <("$HERE/ttywinsz" "$APPPID" 2>/dev/null)
# fi
# AROWS=; ACOLS=
# if [ -f "$PROBELOG" ]; then
# 	AROWS=$(grep -v '^#' "$PROBELOG" | tail -1 | sed -n 's/.*rows=\([0-9]*\).*/\1/p')
# 	ACOLS=$(grep -v '^#' "$PROBELOG" | tail -1 | sed -n 's/.*cols=\([0-9]*\).*/\1/p')
# fi
# 
# say "  viewpoint wants : ${WANT_R}h x ${WANT_C}w"
# say "  kernel pty has  : ${KROWS:-?}h x ${KCOLS:-?}w"
# say "  app believes    : ${AROWS:-n/a}h x ${ACOLS:-n/a}w"
# say ""
# 
# # The decisive number is not the final size - the probe polls, so it converges
# # on the right size no matter what. It is whether SIGWINCH ever arrived, because
# # that is all a real curses app has to go on.
# NWINCH=; NPOLL=; BLOCKED=
# if [ -f "$PROBELOG" ]; then
# 	NWINCH=$(grep -v '^#' "$PROBELOG" | tail -1 | sed -n 's/.*sigwinch=\([0-9]*\).*/\1/p')
# 	NPOLL=$(grep -v '^#' "$PROBELOG" | tail -1 | sed -n 's/.*polled=\([0-9]*\).*/\1/p')
# 	BLOCKED=$(sed -n 's/^# SIGWINCH blocked at startup: //p' "$PROBELOG")
# fi
# [ -n "$NWINCH" ] && say "  SIGWINCH count  : ${NWINCH}   (size changes found by polling instead: ${NPOLL:-0})"
# [ -n "$BLOCKED" ] && say "  SIGWINCH blocked: $BLOCKED"
# 
# # Works for any app, probe or not: ask /proc whether the signal could even be
# # delivered to it.
# APP_BLOCKED=unknown
# if [ -n "$APPPID" ]; then
# 	if sigwinch_blocked "$APPPID"; then
# 		APP_BLOCKED=yes
# 	elif [ $? = 2 ]; then
# 		APP_BLOCKED=unknown
# 	else
# 		APP_BLOCKED=no
# 	fi
# 	say "  app signal mask : SIGWINCH blocked = $APP_BLOCKED"
# fi
# say ""
# 
# FAIL=0
# if [ "$APP_BLOCKED" = yes ]; then
# 	say "  REPRODUCED (B): the pty was resized correctly and the app cannot ever"
# 	say "                  hear about it - SIGWINCH is blocked in its signal"
# 	say "                  mask. A blocked signal survives both fork and exec,"
# 	say "                  so this was inherited; see the mask table above for"
# 	say "                  the first process in the tree that has it blocked."
# 	say "                  Apps that only redraw on SIGWINCH never reflow."
# 	FAIL=1
# elif [ -z "$KROWS" ] || [ "$KROWS" = "?" ]; then
# 	say "  INCONCLUSIVE: could not read the app's pty size."
# 	FAIL=1
# elif [ "$KROWS" != "$WANT_R" ] || [ "$KCOLS" != "$WANT_C" ]; then
# 	say "  REPRODUCED (A): viewpoint resized its window but the pty was never"
# 	say "                  told. The TIOCSWINSZ is missing or lost between the"
# 	say "                  client and the session daemon that owns the pty."
# 	FAIL=1
# elif [ -n "$NWINCH" ] && [ "$NWINCH" = 0 ] && [ "${NPOLL:-0}" != 0 ]; then
# 	say "  REPRODUCED (B): the pty was resized correctly and the app was never"
# 	say "                  told. Not one SIGWINCH arrived - the probe only found"
# 	say "                  the new size because it polls TIOCGWINSZ on a timer."
# 	say "                  A real curses app does not poll, so it never reflows."
# 	FAIL=1
# elif [ -n "$AROWS" ] && { [ "$AROWS" != "$WANT_R" ] || [ "$ACOLS" != "$WANT_C" ]; }; then
# 	say "  REPRODUCED (C): the pty has the right size but the app is still on an"
# 	say "                  older one."
# 	FAIL=1
# else
# 	say "  Sizes agree end to end and SIGWINCH was delivered. If the window still"
# 	say "  looks wrong on screen, the app did reflow and the fault is in"
# 	say "  viewpoint's rendering of the result."
# fi
# 
# if [ -f "$PROBELOG" ]; then
# 	say ""
# 	say "probe log ($PROBELOG):"
# 	sed 's/^/    /' "$PROBELOG" | tee -a "$REPORT" >/dev/null
# 	sed 's/^/    /' "$PROBELOG"
# fi
# 
# say ""
# say "artefacts in $OUTDIR"
# say "  *.png            screenshots at each step"
# say "  viewpoint.log    VP_DEBUG trace (grep for '^geom')"
# say "  probe.log        every size the app was told about"
# say "  report.txt       this report"
# 
# exit $FAIL
