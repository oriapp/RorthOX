#!/usr/bin/env bash
# Bounded public diagnostics for the advisory KVM check.
set -euo pipefail
cd "$(dirname "$0")/.."
notice() {
    local title=$1 text=${2:0:1800}
    text=${text//'%'/'%25'}
    text=${text//$'\r'/'%0D'}
    text=${text//$'\n'/'%0A'}
    printf '::notice title=%s::%s\n' "$title" "$text"
}
case "${1:-}" in
    host)
        details="kernel=$(uname -r); cpu=$(grep -m 2 -E '^(vendor_id|model name)[[:space:]]*:' /proc/cpuinfo || true); "
        for parameter in /sys/module/kvm_{intel,amd}/parameters/{nested,ept,npt,unrestricted_guest}; do
            if [ -r "$parameter" ]; then
                details+="$parameter=$(head -c 100 "$parameter"); "
            fi
        done
        details+="/dev/kvm before=${2:-absent} after=${3:-absent} (mode owner group)"
        notice 'KVM host' "$details"
        ;;
    container)
        . scripts/qemu-accel.sh
        device=absent
        if [ -e /dev/kvm ]; then
            device="$(stat -c '%F %a %U %G' /dev/kvm); readable=$([ -r /dev/kvm ] && echo yes || echo no); writable=$([ -w /dev/kvm ] && echo yes || echo no)"
        fi
        version=$("${QEMU:-qemu-system-i386}" --version | head -n 1)
        if accel_probe; then status=working; else status=unavailable; fi
        notice 'KVM container' "id=$(id); /dev/kvm=$device; QEMU=$version; probe=$status: $accel_why; output=$accel_probe_details"
        ;;
    *) echo 'usage: kvm-diagnostics.sh host before after | container' >&2; exit 2 ;;
esac
