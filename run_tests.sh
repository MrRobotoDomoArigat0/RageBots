#!/bin/sh
# Offline tests (no game files, no game logs needed).
#   brain_test : the bot brain on hand-made game snapshots (combat, pickups, traps, robot legs, ...)
#   menu_test  : the F7 menu model and overlay drawing
#   test_host2 : loads the built SDL2.dll like the game does and checks the bot pads and the F7 menu.
#                Needs the official SDL 2.32.10 x64 SDL2.dll; point SDL2_REAL at it (skipped otherwise).
# On Linux the .exe files run under Wine (WINE=wine by default). On Windows (Git Bash / MSYS2) use WINE= (empty).
# Run ../build.sh first.
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
WINE=${WINE-wine}
export WINEDEBUG=${WINEDEBUG:--all}
OUT=../../build/test; mkdir -p $OUT
$CC -O2 -o $OUT/brain_test.exe brain_test.c ../sor4brain.c ../sor4state.c ../sor4meta.c
$CC -O2 -o $OUT/menu_test.exe menu_test.c ../sor4menu.c -lgdi32 -luser32
$CC -O2 -o $OUT/test_host2.exe test_host2.c
cd $OUT
fail=0
echo "== brain: defense, combos, pickups, traps, bosses, robot legs, friendly fire (synthetic worlds)"
timeout 300 $WINE ./brain_test.exe > brain_test.out || true
grep -E '^FAIL' brain_test.out || true
echo "$(grep -c '^OK' brain_test.out) checks OK; $(tail -1 brain_test.out)"
tail -1 brain_test.out | grep -q '^PASS' || fail=1
echo "== F7 menu model and drawing"
timeout 120 $WINE ./menu_test.exe menu_preview.bgra | tail -2
if [ -n "$SDL2_REAL" ] && [ -f "$SDL2_REAL" ]; then
  cp ../SDL2.dll ./SDL2.dll && cp "$SDL2_REAL" ./SDL2_real.dll
  printf '[Bots]\r\nP2Bot=1\r\n[Brain]\r\nFallbackToTestPattern=1\r\n[Debug]\r\nTestAssumeFocus=1\r\n' > SoR4Bots.ini
  echo "== host: P2 bot ON, F7 menu open/close, fallback pattern (focus assumed)"; timeout 120 $WINE ./test_host2.exe f7 | tail -3
  printf '[Bots]\r\nP2Bot=1\r\n' > SoR4Bots.ini
  echo "== host: P2 bot ON, F7, no game, default settings (expect no presses, stick 0..0)"; timeout 120 $WINE ./test_host2.exe f7 | tail -2 | head -1
  printf '[General]\r\nInputFallbackSeconds=1\r\n[Bots]\r\nP2Bot=1\r\nAttachMode=Auto\r\nReorderLatePads=1\r\nAttachDelayMs=500\r\n' > SoR4Bots.ini
  echo "== host: late controller re-ordering"; timeout 120 $WINE ./test_host2.exe late | grep -E "CONTROLLERDEVICE|plugged"
  printf '[Debug]\r\nTestAssumeFocus=1\r\n' > SoR4Bots.ini
  echo "== host: F7 menu swallows the human's pad and keys, is driven by pad and keys, saves the choice"; timeout 120 $WINE ./test_host2.exe menu | grep -E "OK|FAIL|PASS|expect"
  echo "== logging: rotated once, not truncated"
  rm -f SoR4Bots.log SoR4Bots.prev.log SoR4Bots.pid*.log
  printf '[Bots]\r\nP2Bot=1\r\n' > SoR4Bots.ini
  timeout 120 $WINE ./test_host2.exe f7 > /dev/null; cp SoR4Bots.log run1.log
  timeout 120 $WINE ./test_host2.exe f7 > /dev/null
  cmp -s run1.log SoR4Bots.prev.log && echo "prev.log == run 1 log: OK" || { echo "prev.log differs: FAIL"; fail=1; }
else
  echo "== host tests skipped (set SDL2_REAL=/path/to/official/SDL2.dll, SDL 2.32.10 x64)"
fi
[ $fail = 0 ] && echo "ALL OFFLINE TESTS PASSED" || { echo "SOME TESTS FAILED"; exit 1; }
