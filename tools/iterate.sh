#!/bin/sh
# Lift, build, run hidden; repeat while the run stops on a kernel address that
# has not been lifted yet (exit 20, recorded in work/seeds-kernel.txt).
ISO=${1:-../triage/TempleOS.ISO}; SECS=${2:-20}
cd "$(dirname "$0")/.."
for i in $(seq 1 ${ROUNDS:-30}); do
  python tools/gen.py "$ISO" || exit 1
  cmake --build build --config Release -j >build.log 2>&1 || { grep -E "error" build.log | head; exit 1; }
  EXE=build/Release/templeos.exe; [ -x "$EXE" ] || EXE=build/templeos
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout $((SECS+30)) "$EXE" --hidden --shot work/shot.png $SECS --quit $((SECS+1)) "$ISO" > work/run.log 2>&1
  rc=$?; tail -3 work/run.log
  [ $rc -eq 20 ] || exit $rc
done
