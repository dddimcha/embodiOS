#!/usr/bin/env bash
# test_uefi.sh - build and verify direct UEFI boot (no GRUB) under OVMF.
#
# Usage:
#   scripts/test_uefi.sh           # build esp.img + boot-to-shell check
#   CHAT=1 scripts/test_uefi.sh    # additionally run the chat smoke
#                                  # (needs the GGUF model baked into
#                                  # embodios.elf; patient: TCG is slow)
#
# Env: QEMU_BIN (default $HOME/sysroot/usr/bin/qemu-system-x86_64),
#      OVMF_FD (default $HOME/sysroot/usr/share/qemu/OVMF.fd),
#      QEMU_MEM (default 3072), QEMU_SMP (default 1), TIMEOUT (default 240).
#
# The kernel must already be built with the model embedded, e.g.:
#   make -C kernel GGUF_MODEL=/tmp/models/smollm-135m-instruct-q4_k_m.gguf
set -euo pipefail

cd "$(dirname "$0")/.."
QEMU_BIN=${QEMU_BIN:-$HOME/sysroot/usr/bin/qemu-system-x86_64}
OVMF_FD=${OVMF_FD:-$HOME/sysroot/usr/share/qemu/OVMF.fd}
QEMU_MEM=${QEMU_MEM:-3072}
QEMU_SMP=${QEMU_SMP:-1}
TIMEOUT=${TIMEOUT:-240}

echo "==> building esp.img (BOOTX64.EFI + embodios.elf)"
make -C kernel uefi

OVMF_COPY=$(mktemp /tmp/ovmf-uefi-XXXXXX.fd)
cp -f "$OVMF_FD" "$OVMF_COPY"   # pflash is written by the firmware
trap 'rm -f "$OVMF_COPY" /tmp/uefi-boot.log' EXIT

echo "==> booting under OVMF (serial log: /tmp/uefi-boot.log)"
timeout "$TIMEOUT" "$QEMU_BIN" \
    -drive if=pflash,format=raw,file="$OVMF_COPY" \
    -drive format=raw,file=kernel/esp.img \
    -m "$QEMU_MEM" -smp "$QEMU_SMP" -nographic -no-reboot \
    > /tmp/uefi-boot.log 2>&1 || true

fail=0
check() {
    if grep -qa "$1" /tmp/uefi-boot.log; then
        echo "  PASS: $2"
    else
        echo "  FAIL: $2 ('$1' not found)"
        fail=1
    fi
}

check "embodios UEFI loader"        "BOOTX64.EFI ran (serial banner)"
check "found \\\\embodios.elf"      "kernel located on ESP"
check "boot services exited"        "ExitBootServices succeeded"
check "Memmap: boot magic=0x36D76289" "multiboot2 magic delivered"
check "\[multiboot2 mmap\]"         "RAM source is multiboot2 mmap"
check "Kernel cmdline: uefi"        "cmdline tag delivered"
check "embodios>"                   "shell prompt reached"

if [ "$fail" -ne 0 ]; then
    echo "==> tail of serial log:"
    tail -c 2000 /tmp/uefi-boot.log | cat -v
    exit 1
fi

if [ "${CHAT:-0}" = "1" ]; then
    echo "==> chat smoke (patient, TCG)"
    python3 /mnt/agents/work/smoke_uefi.py kernel/esp.img "${CHAT_TIMEOUT:-900}" \
        "What is the capital of France?"
fi

echo "UEFI boot test: PASS"
