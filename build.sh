#!/usr/bin/env bash
# Builds the native helper, the replacement uWindowCapture.dll and the test tools.
# Needs: gcc, libX11/Xcomposite/Xext/Xfixes/Xrandr/Xtst headers, wine (winegcc) + clang + lld.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"
mkdir -p build
CFLAGS="-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -I src/common"
echo "[1/4] xso-bridge (native helper)"
gcc $CFLAGS -o build/xso-bridge src/helper/xso-bridge.c -lX11 -lXcomposite -lXext -lXfixes -lXrandr -lXtst -lXdamage -lXrender
echo "[2/4] uWindowCapture.dll (PE, winegcc/clang)"
winegcc -b x86_64-w64-mingw32 -shared -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-but-set-variable -I src/common \
    -o build/uWindowCapture.dll src/dll/uwc_linux.c -luser32 -lkernel32
echo "[3/4] shmtest (native test client)"
gcc $CFLAGS -o build/shmtest src/test/shmtest.c
echo "[4/4] harness.exe (PE test program)"
winegcc -b x86_64-w64-mingw32 -mconsole -O2 -Wall -I src/common -o build/harness.exe src/test/harness.c -ld3d11 -luser32
echo "done:"; ls -la build/xso-bridge build/uWindowCapture.dll
