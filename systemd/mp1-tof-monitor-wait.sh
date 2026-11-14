#!/bin/sh
set -eu

printf '%s\n' 'Waiting for M4, RPMsg and Wayland...'

MP1_COUNT=0

while [ "$MP1_COUNT" -lt 30 ]; do
    if [ -S /run/user/1000/wayland-0 ] &&
       [ -c /dev/ttyRPMSG0 ] &&
       [ -r /dev/ttyRPMSG0 ] &&
       [ -w /dev/ttyRPMSG0 ] &&
       [ -c /dev/ttyRPMSG1 ] &&
       [ -r /dev/ttyRPMSG1 ] &&
       [ -w /dev/ttyRPMSG1 ] &&
       [ -r /sys/class/remoteproc/remoteproc0/state ] &&
       [ "$(cat /sys/class/remoteproc/remoteproc0/state)" = "running" ]; then
        printf '%s\n' 'Ready: M4, RPMsg and Wayland.'
        exit 0
    fi

    MP1_COUNT=$((MP1_COUNT + 1))
    sleep 1
done

printf '%s\n' 'Startup timed out: check M4, RPMsg permissions and Weston.' >&2
exit 1
