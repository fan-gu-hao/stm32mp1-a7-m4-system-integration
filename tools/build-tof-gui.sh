#!/bin/bash
set -e

MP1_REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MP1_SDK="${MP1_SDK:-$HOME/STM32MPU_workspace/STM32MPU-Ecosystem-v6.2.1/Developer-Package/SDK/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi}"

MP1_SOURCE="$MP1_REPO/a7-gui/mp1_tof_monitor.c"
MP1_OUTPUT="$MP1_REPO/build/tof-gui/mp1_tof_monitor"

test -s "$MP1_SOURCE"

if [ ! -f "$MP1_SDK" ]; then
    printf '找不到 SDK 環境設定：%s\n' "$MP1_SDK" >&2
    exit 1
fi

source "$MP1_SDK"

case "$($CC -dumpmachine)" in
    arm-*) ;;
    *)
        printf '%s\n' '目前不是 A7 ARM 交叉編譯器。' >&2
        exit 1
        ;;
esac

pkg-config --exists gtk+-3.0
mkdir -p "$(dirname "$MP1_OUTPUT")"

$CC $CPPFLAGS $CFLAGS -std=gnu11 -O2 -Wall -Wextra \
    $(pkg-config --cflags gtk+-3.0) \
    "$MP1_SOURCE" -o "$MP1_OUTPUT.new" \
    $LDFLAGS $(pkg-config --libs gtk+-3.0)

mv "$MP1_OUTPUT.new" "$MP1_OUTPUT"
file "$MP1_OUTPUT"
