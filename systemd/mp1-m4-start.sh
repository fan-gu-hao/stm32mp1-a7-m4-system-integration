#!/bin/sh
set -eu

MP1_RPROC=/sys/class/remoteproc/remoteproc0
MP1_FW=OpenAMP_TTY_echo_CM4.elf

while [ ! -r "$MP1_RPROC/state" ]; do
    sleep 1
done

case "$(cat "$MP1_RPROC/state")" in
    offline)
        printf '%s\n' "$MP1_FW" > "$MP1_RPROC/firmware"
        printf '%s\n' start > "$MP1_RPROC/state"
        ;;
    running)
        if [ "$(cat "$MP1_RPROC/firmware")" != "$MP1_FW" ]; then
            echo "M4 is running a different firmware." >&2
            exit 1
        fi
        ;;
    *)
        echo "M4 state does not allow startup." >&2
        exit 1
        ;;
esac

while [ ! -c /dev/ttyRPMSG0 ]; do
    sleep 1
done
