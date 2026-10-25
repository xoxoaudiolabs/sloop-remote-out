#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build sloop-remote-out, SLOOP for the FM-1 Companion Studio, from this repo's SLOOP (the one the FM-1 runs, with
# REMOTE OUT), on the Pi itself.
#   ./build.sh [SLOOP_DIR]      -> ./sloop-remote-out       (SLOOP_DIR: the repo, one folder up, unless given)
# Needs gcc and python3 with Pillow (SLOOP's generated headers: fonts, icons, tables, samples, kits, patches, logo).
set -e
cd "$(dirname "$0")"
SLOOP="$(cd "${1:-..}" && pwd)"
GEN="$SLOOP/build/gen"
mkdir -p "$GEN"
for t in font:felucca_font icons:felucca_icons tables:felucca_tables samples:felucca_samples \
         drumkits:felucca_drumkits fm6_patches:felucca_fm6 logo:sloop_logo; do
    [ -f "$GEN/${t#*:}.h" ] || python3 "$SLOOP/tools/gen_${t%%:*}.py" "$GEN/${t#*:}.h"
done
gcc -O2 -w -I"$GEN" -I"$SLOOP/firmware/src" -I"$SLOOP/firmware/hal" -I"$SLOOP/tests" \
    -o sloop-remote-out sloop_remote_out.c -lm -lpthread
echo "built ./sloop-remote-out from $SLOOP"
