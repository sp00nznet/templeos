#!/bin/sh
# Setup for Linux and macOS: lift TempleOS from your ISO, build, capture.
#   ./setup.sh /path/to/TempleOS.ISO
# Needs: python3 with capstone (pip install capstone), cmake, a C compiler,
# SDL2 (Debian/Ubuntu: apt install libsdl2-dev; macOS: brew install sdl2),
# and pcrecomp's 'temple' branch at ../pcrecomp-temple (or PCRECOMP=...).
# TempleOS was written by Terry A. Davis (1969-2018) and is in the public domain.
set -e
cd "$(dirname "$0")"
ISO=${1:-work/TempleOS.ISO}
PCR=${PCRECOMP:-../pcrecomp-temple}
[ -d "$PCR" ] || git clone -b temple https://github.com/sp00nznet/pcrecomp.git "$PCR" || true
[ -f "$PCR/tools/lift/lift64_sys.py" ] || { echo "pcrecomp 'temple' branch not found at $PCR (set PCRECOMP)"; exit 1; }
python3 -c 'import capstone' 2>/dev/null || { echo "python3 -m pip install capstone"; exit 1; }
mkdir -p work
[ -f work/TempleOS.ISO ] || cp "$ISO" work/TempleOS.ISO
python3 tools/gen.py work/TempleOS.ISO "$PCR"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPCRECOMP_DIR="$PCR"
python3 tools/capture.py work/TempleOS.ISO
printf '#!/bin/sh\ncd "$(dirname "$0")"\nexec build/templeos work/TempleOS.ISO "$@"\n' > templeos.sh
chmod +x templeos.sh
echo "Done. Start TempleOS with ./templeos.sh"
