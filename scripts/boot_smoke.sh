#!/usr/bin/env bash
#
# boot_smoke.sh - Boot EMBODIOS in QEMU and verify the shell responds.
#
# Usage:
#   scripts/boot_smoke.sh [--iso ISO_PATH] [--uefi] [--timeout SEC]
#
# Exit code 0 = kernel booted and shell answered 'version'.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

BOOT_TARGET="$ROOT_DIR/kernel/embodios.elf"
USE_ISO=0
USE_UEFI=0
TIMEOUT=60

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iso|-i) USE_ISO=1; BOOT_TARGET="${2:-$ROOT_DIR/dist/embodios.iso}"; shift; [ -n "${2:-}" ] && shift ;;
        --uefi|-u) USE_UEFI=1; shift ;;
        --timeout|-t) TIMEOUT="$2"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

QEMU="${QEMU_BIN:-$(command -v qemu-system-x86_64 || true)}"
[ -n "$QEMU" ] || { echo "ERROR: qemu-system-x86_64 not found"; exit 1; }
[ -f "$BOOT_TARGET" ] || { echo "ERROR: boot target not found: $BOOT_TARGET"; exit 1; }

QEMU_ARGS=(-m 1024M -serial stdio -display none -no-reboot)
if [ $USE_ISO -eq 1 ]; then
    QEMU_ARGS=(-cdrom "$BOOT_TARGET" -boot d "${QEMU_ARGS[@]}")
else
    QEMU_ARGS=(-kernel "$BOOT_TARGET" "${QEMU_ARGS[@]}")
fi
if [ $USE_UEFI -eq 1 ]; then
    OVMF=""
    # OVMF_CODE_4M.fd needs a separate VARS flash - skip it for -bios use
    for f in "${SYSROOT:-$HOME/sysroot}/usr/share/OVMF/OVMF_CODE.fd" \
             "${SYSROOT:-$HOME/sysroot}/usr/share/ovmf/OVMF.fd" \
             "${SYSROOT:-$HOME/sysroot}/usr/share/qemu/OVMF.fd" \
             /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd; do
        [ -f "$f" ] && OVMF="$f" && break
    done
    [ -n "$OVMF" ] || { echo "ERROR: OVMF firmware not found"; exit 1; }
    QEMU_ARGS=(-bios "$OVMF" "${QEMU_ARGS[@]}")
    echo "[smoke] UEFI firmware: $OVMF"
fi

LOG="$(mktemp /tmp/embodios-boot-XXXXXX.log)"
echo "[smoke] Booting $BOOT_TARGET (timeout ${TIMEOUT}s, log: $LOG)"

# Feed 'version' to the shell once it should be up; capture serial output.
( sleep 15; printf 'version\n'; sleep 10; printf 'help\n'; sleep 5 ) \
    | timeout "$TIMEOUT" "$QEMU" "${QEMU_ARGS[@]}" > "$LOG" 2>&1 || true

if grep -q "EMBODIOS" "$LOG" && grep -qiE "embodios>|\\$|version" "$LOG"; then
    echo "[smoke] PASS: kernel booted, banner detected"
    grep -m1 "EMBODIOS" "$LOG" || true
    exit 0
else
    echo "[smoke] FAIL: boot verification failed, see $LOG"
    tail -20 "$LOG" || true
    exit 1
fi
