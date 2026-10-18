#!/bin/sh
# Stage the newest 86Box-Next build in Latest/ (repo root, git-ignored).
# Latest holds one rig per guest OS ("Windows 98 SE", "MS-DOS", ...), each a
# folder with its own 86box.cfg, nvr/, disk images and roms/: the exe goes
# into every rig folder that has an 86box.cfg or an 86Box-Next.exe (folders
# with neither are left alone).  Every rig gets whatever files of the global
# ROM set ($NEXT_ROMS, default ../Roms = F:\Claude\Roms: the 86Box ROM set
# plus ours -- NVIDIA, Megatouch, ...) its roms/ lacks.
#
# The rigs are where the machines being tested live, so this only ever
# replaces the exe (and, for a dynamic build, its DLLs and Qt plugins) and
# never deletes anything: 86box.cfg, nvr/, disk images and whatever else is
# there are left alone.  roms/ only gains missing files, unless
# --update-roms asks for every file of the global set to be refreshed.
#
# The default is the static build (build-static: one self-contained exe).  A
# dynamic build directory also works; then every DLL and Qt plugin it needs
# from MSYS2 UCRT64 is copied next to it.
#
# Usage: tools/stage-rig.sh [--update-roms] [build-dir]
#        (run from the repo root, Git Bash)
set -e
UPDATE_ROMS=0
if [ "$1" = "--update-roms" ]; then
    UPDATE_ROMS=1
    shift
fi
BUILD=${1:-build-static}
OUT=Latest  # the rigs: Latest/ in the repo root, git-ignored
ROMS=${NEXT_ROMS:-../Roms}  # the global ROM set, outside the repo
UCRT=/c/msys64/ucrt64
EXE="$BUILD/src/86Box-Next.exe"
export PATH="$UCRT/bin:/c/msys64/usr/bin:$PATH"

[ -f "$EXE" ] || { echo "no $EXE - build first" >&2; exit 1; }
[ -d "$ROMS/machines" ] || { echo "no ROM set in $ROMS" >&2; exit 1; }
# Only an 86Box-Next running from a rig blocks the copy; test VMs elsewhere
# (work-* folders) run their own exe copies.
RIGS_WIN=$(cygpath -w "$(cd "$OUT" && pwd)")
if ! powershell.exe -NoProfile -Command "\$r = '$RIGS_WIN\'; if (Get-CimInstance Win32_Process -Filter \"Name='86Box-Next.exe'\" | Where-Object { \$_.ExecutablePath -and \$_.ExecutablePath.StartsWith(\$r, 'OrdinalIgnoreCase') }) { exit 1 }" >/dev/null 2>&1; then
    echo "86Box-Next is running from a rig in $OUT; close it and stage again" >&2
    exit 2
fi
# Every rig: a subfolder of Latest with a machine or an exe in it.  A rig
# holding a .stage-branch file belongs to that git branch: it gets builds of
# that branch only, and a build of that branch goes to such rigs only.
BRANCH=$(git branch --show-current)
OWNED=0
for d in "$OUT"/*/; do
    [ "$(cat "${d}.stage-branch" 2>/dev/null | tr -d '
')" = "$BRANCH" ] && OWNED=1
done
RIGS=""
for d in "$OUT"/*/; do
    d=${d%/}
    if [ -f "$d/86box.cfg" ] || [ -f "$d/86Box-Next.exe" ]; then
        OWNER=$(cat "$d/.stage-branch" 2>/dev/null | tr -d '
')
        if [ -n "$OWNER" ] && [ "$OWNER" != "$BRANCH" ]; then
            echo "Skipping $d (branch $OWNER's rig)"
            continue
        fi
        if [ -z "$OWNER" ] && [ "$OWNED" = 1 ]; then
            continue
        fi
        RIGS="$RIGS
$d"
    fi
done
[ -n "$RIGS" ] || { echo "no rig folders in $OUT" >&2; exit 1; }

stage_rig() {
    RIG=$1
    # The global ROM set ($ROMS, F:\Claude\Roms): the 86Box set plus ours
    # (NVIDIA, Megatouch, ...).  A rig without roms/ gets a copy; otherwise
    # missing files are added, and existing ones replaced only when asked.
    mkdir -p "$RIG/roms"
    if [ "$UPDATE_ROMS" = 1 ]; then
        (cd "$ROMS" && tar cf - --exclude=.git .) | (cd "$RIG/roms" && tar xf -)
    else
        (cd "$ROMS" && tar cf - --exclude=.git .) | (cd "$RIG/roms" && tar xf - --skip-old-files)
    fi

    strip -o "$RIG/86Box-Next.exe" "$EXE"
    if ! ldd "$EXE" | grep -qi '/ucrt64/'; then
        echo "Staged $(git describe --always --dirty) in $RIG (static)"
        return
    fi

    # A dynamic build: its Qt plugins and every UCRT64 DLL it pulls in.
    windeployqt6 --no-translations --no-compiler-runtime --no-system-d3d-compiler         --no-opengl-sw --dir "$RIG" "$RIG/86Box-Next.exe" >/dev/null
    for i in 1 2 3; do
        find "$RIG" -maxdepth 2 \( -name '*.exe' -o -name '*.dll' \) |
            xargs ldd 2>/dev/null | awk '{print $3}' | grep -i '^/ucrt64/' | sort -u |
            while read -r dll; do
                cp -u "$UCRT/bin/$(basename "$dll")" "$RIG/"
            done
    done
    echo "Staged $(git describe --always --dirty) in $RIG (dynamic)"
}

echo "$RIGS" | while read -r rig; do
    [ -n "$rig" ] && stage_rig "$rig"
done

# isp-server (the virtual ISP and telephone exchange the rigs' modems dial)
# sits in Latest itself, shared by every rig.
# A branch build that owns a rig stages only that rig, not the shared isp-server.
ISP="$BUILD/isp-server/isp-server.exe"
if [ "$OWNED" != 1 ] && [ -f "$ISP" ]; then
    if tasklist //FI "IMAGENAME eq isp-server.exe" 2>/dev/null | grep -qi "isp-server.exe"; then
        echo "isp-server is running; not replaced (close it and stage again)" >&2
    else
        strip -o "$OUT/isp-server.exe" "$ISP"
        echo "Staged isp-server in $OUT"
    fi
fi
