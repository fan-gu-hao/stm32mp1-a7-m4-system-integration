#!/bin/bash
set -e

MP1_REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MP1_DEV="${MP1_DEV:-$HOME/STM32MPU_workspace/STM32MPU-Ecosystem-v6.2.1/Developer-Package}"
MP1_KPKG="${MP1_KPKG:-$MP1_DEV/stm32mp-openstlinux-6.6-yocto-scarthgap-mpu-v26.06.10/sources/ostl-linux/linux-stm32mp-6.6.129-stm32mp-r3.1-r0}"

MP1_KERNEL="$MP1_KPKG/linux-6.6.129"
MP1_BUILD="$MP1_KPKG/build"
MP1_SDK="${MP1_SDK:-$MP1_DEV/SDK/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi}"

MP1_NAME=stm32mp157f-dk2-tof-m4
MP1_SOURCE="$MP1_REPO/bsp/device-tree/$MP1_NAME.dts"
MP1_DEST="$MP1_KERNEL/arch/arm/boot/dts/st/$MP1_NAME.dts"
MP1_OUTPUT="$MP1_REPO/build/device-tree"

test -s "$MP1_SOURCE"
test -f "$MP1_SDK"
test -s "$MP1_BUILD/.config"
test -s "$MP1_KERNEL/arch/arm/boot/dts/st/stm32mp157f-dk2.dts"

mkdir -p "$MP1_OUTPUT"

if ! cmp -s "$MP1_SOURCE" "$MP1_DEST"; then
    if [ -e "$MP1_DEST" ]; then
        MP1_BACKUP="$(mktemp -d "$MP1_OUTPUT/dts-backup.XXXXXX")"
        cp -p "$MP1_DEST" "$MP1_BACKUP/"
        printf '原裝置樹備份：%s\n' "$MP1_BACKUP"
    fi

    cp "$MP1_SOURCE" "$MP1_DEST"
fi

source "$MP1_SDK"

make -C "$MP1_KERNEL" O="$MP1_BUILD" ARCH=arm \
    CROSS_COMPILE="${TARGET_PREFIX:-arm-ostl-linux-gnueabi-}" \
    "st/$MP1_NAME.dtb"

test -s "$MP1_BUILD/arch/arm/boot/dts/st/$MP1_NAME.dtb"

cp "$MP1_BUILD/arch/arm/boot/dts/st/$MP1_NAME.dtb" \
    "$MP1_OUTPUT/$MP1_NAME.dtb.new"

mv "$MP1_OUTPUT/$MP1_NAME.dtb.new" \
    "$MP1_OUTPUT/$MP1_NAME.dtb"

file "$MP1_OUTPUT/$MP1_NAME.dtb"
