#!/usr/bin/env bash
# Headless HD smoke test of an arm64 Linux WebGPU release tarball, run in the
# damnationx-linux-builder image: Xvfb + Mesa lavapipe (software Vulkan), audio muted.
# The HD pack is mounted read-only as `hd` next to the binary, where the game picks it up
# by itself (Graphics > HD Art, on by default); it creates a Warrior through the menus with
# xdotool and screenshots the main menu, hero creation and town. In town it captures the
# skinned 3D warrior (Graphics > 3D Warrior Model, on by default) standing and walking, plus a
# zoomed crop around the hero, toggles first person with G and back, then quits through the
# in-game menu (Esc, Up = Quit Game, Return) and fails unless the game exits cleanly (status 0)
# within QUIT_TIMEOUT seconds. It also fails on "3D warrior"/render3d/glTF errors in the log.
#
# Usage: tools/linux-docker/hd-smoke-test.sh TARBALL DIABDAT_MPQ HD_DIR OUT_DIR
#   TARBALL      devilutionx-aarch64-linux-gnu-webgpu.tar.xz (from tools/build-linux-docker.sh), or the
#                x86_64 one: that runs as linux/amd64 (emulated on an arm64 host, much slower) in the
#                runtime-only image built from tools/linux-docker/smoke-runtime
#   DIABDAT_MPQ  your diabdat.mpq; it is copied into a private temp dir, never into the repo
#   HD_DIR       HD pack folder (mounted read-only as hd/ next to the binary)
#   OUT_DIR      receives *.png screenshots, devilutionx.log and vulkaninfo.txt
#   env RES=1280x960   Xvfb screen size
#   env GAME_ARGS=...  extra devilutionx arguments, e.g. --hd for builds without HD auto-enable
#   env IMAGE=...      container image (default damnationx-linux-builder:24.04); with
#                      damnationx-linux-gdb:24.04 the game runs under gdb and a crash leaves
#                      a backtrace in devilutionx.log
#   env QUIT_TIMEOUT=180  seconds to wait for the game to exit after Quit Game
set -euo pipefail

TARBALL="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
DIABDAT="$2"
HD_DIR="$(cd "$3" && pwd)"
OUT_DIR="$4"
RES="${RES:-1280x960}"
PLATFORM=linux/arm64
DEFAULT_IMAGE=damnationx-linux-builder:24.04
if [[ $(basename "$TARBALL") == *x86_64* ]]; then
	PLATFORM=linux/amd64
	DEFAULT_IMAGE=damnationx-linux-smoke-amd64:24.04
	[[ -n ${IMAGE:-} ]] || docker build --platform "$PLATFORM" -t "$DEFAULT_IMAGE" "$(dirname "$0")/smoke-runtime"
fi
IMAGE="${IMAGE:-$DEFAULT_IMAGE}"
# Seconds to wait for the main menu, and a multiplier for the menu and level-load waits;
# emulated runs are much slower.
START_WAIT="${START_WAIT:-$([[ $PLATFORM == linux/amd64 ]] && echo 60 || echo 20)}"
SLOW="${SLOW:-$([[ $PLATFORM == linux/amd64 ]] && echo 3 || echo 1)}"
QUIT_TIMEOUT="${QUIT_TIMEOUT:-180}"

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/dvx-hd-smoke.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/game" "$WORK/data" "$WORK/save"
tar -xJf "$TARBALL" -C "$WORK/game"
cp "$DIABDAT" "$WORK/data/diabdat.mpq"
cat > "$WORK/save/diablo.ini" <<'INI'
[StartUp]
Diablo Intro=0
Splash=0

[Graphics]
Fullscreen=0
INI

docker run --rm --platform "$PLATFORM" \
	-v "$WORK/game:/game" -v "$WORK/data:/data" -v "$HD_DIR:/game/hd:ro" -v "$WORK/save:/save" -v "$OUT_DIR:/out" \
	-e SDL_AUDIODRIVER=dummy -e RES="$RES" -e GAME_ARGS="${GAME_ARGS:-}" -e QUIT_TIMEOUT="$QUIT_TIMEOUT" -e START_WAIT="$START_WAIT" -e SLOW="$SLOW" \
	"$IMAGE" bash -euo pipefail -c '
		export DISPLAY=:99 SDL_VIDEODRIVER=x11
		Xvfb :99 -screen 0 ${RES}x24 -nolisten tcp >/out/xvfb.log 2>&1 &
		for _ in $(seq 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.2; done
		vulkaninfo --summary >/out/vulkaninfo.txt 2>&1 || true
		shot() { xwd -root -silent | xwdtopnm 2>/dev/null | pnmtopng >/out/$1.png; echo "screenshot $1.png"; }
		# Screenshot plus a 2x crop around the hero; the game window is centred on the Xvfb screen and
		# keeps the player centred above the panel (HX, HY are set once the game window is known).
		shot_hero() {
			shot "$1"
			xwd -root -silent | xwdtopnm 2>/dev/null | pnmcut -left $((HX - 120)) -top $((HY - 150)) -width 240 -height 240 \
				| pamscale 2 | pnmtopng >/out/$1-hero.png
		}
		key() { xdotool search --sync --onlyvisible --class devilutionx windowactivate --sync >/dev/null 2>&1 || true; xdotool key --delay 150 "$@"; }
		click() { xdotool mousemove --sync "$1" "$2" click 1; }
		cd /game
		run=(./devilutionx --data-dir /data --save-dir /save --config-dir /save --diablo --verbose $GAME_ARGS)
		if command -v gdb >/dev/null; then
			run=(gdb -q -batch -return-child-result -ex run -ex "thread apply all bt" --args "${run[@]}")
		fi
		"${run[@]}" >/out/devilutionx.log 2>&1 &
		pid=$!
		alive() { kill -0 $pid 2>/dev/null || { echo "devilutionx exited early"; tail -n 60 /out/devilutionx.log; exit 1; }; }
		sleep "$START_WAIT"; alive; shot 01-main-menu
		key Return; sleep $((4 * SLOW)); alive; shot 02-hero-select       # Single Player
		key Return; sleep $((4 * SLOW)); alive; shot 03-choose-class      # New Hero
		key Return; sleep $((4 * SLOW)); alive; shot 04-hero-name         # Warrior
		xdotool type --delay 120 HDTest; key Return; sleep $((4 * SLOW)); alive; shot 05-after-name
		key Return; sleep $((45 * SLOW)); alive; shot 06-town               # difficulty (if asked) and level load
		sleep $((15 * SLOW)); alive; shot 07-town-later
		eval "$(xdotool search --onlyvisible --class devilutionx getwindowgeometry --shell | tail -n 5)"
		HX=$((X + WIDTH / 2)); HY=$((Y + HEIGHT * 33 / 100))
		echo "game window ${WIDTH}x${HEIGHT}+${X}+${Y}, hero at ${HX},${HY}"
		# 3D warrior idle: two frames a beat apart.
		shot_hero 08-warrior-idle-a; sleep 1.3; alive; shot_hero 09-warrior-idle-b
		# Walk by holding the left button toward a point to the east, then back west. Each walk starts
		# with a small move and a click so the game sees fresh motion under the new cursor position.
		walk_to() { xdotool mousemove --sync "$1" "$2"; sleep 0.3; xdotool mousemove_relative 2 0 click 1 mousedown 1; }
		walk_to $((HX + WIDTH * 4 / 10)) $((HY + HEIGHT / 8))
		sleep 0.6; alive; shot_hero 10-warrior-walk-a
		sleep 0.3; shot_hero 11-warrior-walk-b
		sleep 0.3; shot_hero 12-warrior-walk-c
		xdotool mouseup 1; sleep 3
		walk_to $((HX - WIDTH * 4 / 10)) $((HY - HEIGHT / 10))
		sleep 0.8; alive; shot_hero 13-warrior-walk-back-a
		sleep 0.3; shot_hero 13-warrior-walk-back-b
		xdotool mouseup 1; sleep 5; alive; shot_hero 14-warrior-idle-after-walk
		xdotool mousemove --sync $HX $((HY - HEIGHT / 6))
		# First person (G) and back.
		key g; sleep 8; alive; shot 15-first-person
		sleep 4; alive; shot 16-first-person-later
		key g; sleep 6; alive; shot_hero 17-back-to-iso
		# Quit through the in-game menu: Esc opens it on Options; Up wraps to Quit Game.
		key Escape; sleep 3; alive; shot 18-game-menu
		key Up; sleep 2; alive; shot 19-quit-selected
		key Return
		status=timeout
		for _ in $(seq "$QUIT_TIMEOUT"); do
			if ! kill -0 $pid 2>/dev/null; then
				status=0; wait $pid || status=$?
				break
			fi
			sleep 1
		done
		if [[ $status == timeout ]]; then
			shot 20-quit-hung
			echo "FAIL: devilutionx still running ${QUIT_TIMEOUT}s after Quit Game"
			kill -9 $pid 2>/dev/null || true
			exit 1
		fi
		echo "devilutionx exit status: $status"
		errors=$(grep -E "3D warrior: .*(fail|could not|missing|max|more than)|render3d: .*failed|[Gg]ltf.*(error|fail)|ERROR.*(3D|glTF|render3d)" /out/devilutionx.log || true)
		grep -E "3D warrior|render3d|FPV" /out/devilutionx.log | sort | uniq -c | sort -rn | head -n 40 >/out/render3d-log-summary.txt || true
		if [[ -n $errors ]]; then echo "FAIL: 3D model errors in the log:"; echo "$errors"; exit 1; fi
		grep -q "3D warrior: GPU resources ready" /out/devilutionx.log || { echo "FAIL: the 3D warrior never reached GPU resources ready"; exit 1; }
		[[ $status == 0 ]] || { echo "FAIL: devilutionx exited with status $status"; exit 1; }
		echo "PASS: 3D warrior loaded, first person toggled, clean exit"
	'
ls -la "$OUT_DIR"
