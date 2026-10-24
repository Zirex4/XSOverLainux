#!/usr/bin/env bash
# XSOverlay on Linux launch wrapper.
#
# Steam > XSOverlay > Properties > Launch options:
#   /path/to/XSOverLainux/xso-linux.sh %command%
#
# What it does:
#   1. starts the native X11 capture helper (xso-bridge) if it is not running
#   2. installs the Linux-aware uWindowCapture.dll into the XSOverlay install
#      (the original is kept as uWindowCapture.dll.windows)
#   3. runs XSOverlay through Proton exactly as Steam asked
#   4. stops the helper when XSOverlay exits (only if this script started it)
#
# Env knobs:
#   XSO_BRIDGE_SHM   shared memory file (default /dev/shm/xso-bridge)
#   XSO_BRIDGE_ARGS  extra args for xso-bridge (default "-v")
#   XSO_DIRECT=0     keep the "XSOverlay Process Manager.exe" launcher (default: launch
#                    XSOverlay.exe -batchmode directly, the launcher crashes under Wine)
#   XSO_INPUT=0      do not forward mouse/keyboard from XSOverlay to the X11 desktop
#   XSO_UWC_DEBUG=1  verbose DLL log in /dev/shm/xso-uwc.log
#   XSO_NO_HELPER=1  do not start/stop the helper (you run it yourself)
#   XSO_RELAUNCH=N   relaunch XSOverlay up to N times (default 3) when it exits because
#                    SteamVR's compositor died (exit code 130, "VR - QUIT - EVENT") while
#                    SteamVR itself is still running (SteamVR-for-Linux#1006 crashes)

here="$(cd "$(dirname "$(realpath "${BASH_SOURCE[0]}")")" && pwd)"
log="$here/xso-linux.log"
exec > >(tee -a "$log") 2>&1
echo "=== $(date '+%F %T') xso-linux.sh: $*"

export XSO_BRIDGE_SHM="${XSO_BRIDGE_SHM:-/dev/shm/xso-bridge}"
started_helper=
helper_pid=

if [ -z "${XSO_NO_HELPER:-}" ]; then
    if ! pgrep -x xso-bridge >/dev/null 2>&1; then
        "$here/build/xso-bridge" ${XSO_BRIDGE_ARGS:--v} >> "$here/xso-bridge.log" 2>&1 &
        helper_pid=$!
        started_helper=1
        echo "started xso-bridge (pid $helper_pid)"
        sleep 0.5
    else
        echo "xso-bridge already running"
    fi
fi

# locate the XSOverlay install dir from the command line
gamedir=
args=("$@")
for i in "${!args[@]}"; do
    a="${args[$i]}"
    case "$a" in
        *"XSOverlay Process Manager.exe"|*/XSOverlay.exe)
            d="$(dirname "$a")"
            [ "$(basename "$d")" = "Launcher" ] && d="$(dirname "$d")"
            gamedir="$d"
            if [ "${XSO_DIRECT:-1}" != "0" ] && [ "$(basename "$a")" = "XSOverlay Process Manager.exe" ]; then
                args[$i]="$gamedir/XSOverlay.exe"
                args+=("-batchmode")
                echo "launching $gamedir/XSOverlay.exe -batchmode directly (XSO_DIRECT=0 to keep the launcher)"
            fi
            ;;
    esac
done

if [ -n "$gamedir" ] && [ -d "$gamedir/XSOverlay_Data/Plugins/x86_64" ]; then
    dll="$gamedir/XSOverlay_Data/Plugins/x86_64/uWindowCapture.dll"
    if ! cmp -s "$here/build/uWindowCapture.dll" "$dll"; then
        if [ ! -f "$dll.windows" ] && ! grep -q "xso_shm\|linux bridge build" "$dll" 2>/dev/null; then
            cp -p "$dll" "$dll.windows" && echo "backed up original DLL to $dll.windows"
        fi
        cp "$here/build/uWindowCapture.dll" "$dll.new" && mv -f "$dll.new" "$dll" && echo "installed Linux uWindowCapture.dll into $gamedir"
    else
        echo "Linux uWindowCapture.dll already installed"
    fi
else
    echo "WARNING: could not locate the XSOverlay install dir from the command line; DLL not installed"
fi

attempt=0
while :; do
    "${args[@]}"
    rc=$?
    echo "XSOverlay exited with $rc"
    max="${XSO_RELAUNCH:-3}"
    if [ "$rc" = "130" ] && [ "$attempt" -lt "$max" ] && pgrep -x vrmonitor >/dev/null 2>&1; then
        attempt=$((attempt+1))
        echo "SteamVR still running: XSOverlay probably lost the compositor (SteamVR bug #1006); relaunching ($attempt/$max) in 8 s"
        for i in 1 2 3 4 5 6 7 8; do sleep 1; pgrep -x vrmonitor >/dev/null 2>&1 || break; done
        pgrep -x vrmonitor >/dev/null 2>&1 || { echo "SteamVR exited; not relaunching"; break; }
        continue
    fi
    break
done

if [ -n "$started_helper" ] && [ -n "$helper_pid" ]; then
    kill "$helper_pid" 2>/dev/null && echo "stopped xso-bridge"
fi
exit $rc
