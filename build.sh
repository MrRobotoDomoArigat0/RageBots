#!/bin/sh
# Builds ../build/SDL2.dll (the SoR4Bots proxy). Needs an x86_64 mingw-w64 compiler on PATH.
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
mkdir -p ../build
$CC -O2 -Wall -Wextra -Wno-unused-parameter -shared -s \
    -o ../build/SDL2.dll sor4bots.c sor4state.c sor4brain.c sor4meta.c sor4menu.c SDL2.def -lkernel32 -luser32 -lgdi32
echo "built ../build/SDL2.dll"
