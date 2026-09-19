#!/bin/bash
# Bootstrap toolchain (QEMU + GRUB + xorriso) into $HOME/sysroot — no root needed.
# Usage: bash /mnt/agents/work/bootstrap_toolchain.sh && . $HOME/env.sh
set -e
mkdir -p $HOME/debs $HOME/sysroot $HOME/aptlists/partial $HOME/aptcache/archives/partial
APT_OPTS="-o Dir::State::Lists=$HOME/aptlists -o Dir::Cache=$HOME/aptcache"
# reuse shared apt lists if local update fails
if [ ! -s $HOME/aptlists/*Packages* ] 2>/dev/null; then
  apt-get $APT_OPTS update >/dev/null 2>&1 || cp /mnt/agents/work/aptlists/* $HOME/aptlists/ 2>/dev/null || true
fi
cd $HOME/debs
PKGS="qemu-system-x86 qemu-system-common qemu-system-data seabios ipxe-qemu xorriso mtools grub-pc-bin grub-common grub-efi-amd64-bin ovmf libisoburn1 libburn4 libisofs6 libjte2 libcapstone4 libfdt1 libpmem1 librdmacm1 libibverbs1 libslirp0 libvdeplug2 libbpf1 liburing2 libfuse3-3 libaio1 libndctl6 libdaxctl1 libefivar1 libefiboot1"
for p in $PKGS; do [ -f "$(ls $p\_*.deb 2>/dev/null | head -1)" ] || apt-get $APT_OPTS download "$p" >/dev/null 2>&1; done
for f in $HOME/debs/*.deb; do dpkg -x "$f" $HOME/sysroot; done
cp -n $HOME/debs/*.deb /mnt/agents/work/debs/ 2>/dev/null || true  # cache for future sessions
# Patch hardcoded grub paths (equal-length!). Basename MUST stay "grub" (grub-mkrescue derives ISO path from it).
python3 - <<'PYEOF'
import glob, os
for f in glob.glob(os.path.expanduser("~/sysroot/usr/bin/grub*")):
    d = open(f, "rb").read()
    if b"/usr/lib/grub" in d or b"/usr/share/grub" in d:
        d = d.replace(b"/usr/lib/grub", b"/tmp/lib/grub").replace(b"/usr/share/grub", b"/tmp/share/grub")
        assert len(d) > 0
        open(f, "wb").write(d)
PYEOF
mkdir -p /tmp/lib /tmp/share
ln -sfn $HOME/sysroot/usr/lib/grub /tmp/lib/grub
ln -sfn $HOME/sysroot/usr/share/grub /tmp/share/grub
cat > $HOME/env.sh <<'EOF'
export SYSROOT=$HOME/sysroot
export PATH=$SYSROOT/usr/bin:$PATH
export LD_LIBRARY_PATH=$SYSROOT/usr/lib/x86_64-linux-gnu:$SYSROOT/lib/x86_64-linux-gnu
export QEMU_BIN=$SYSROOT/usr/bin/qemu-system-x86_64
export QEMU_MODULE_DIR=$SYSROOT/usr/lib/x86_64-linux-gnu/qemu
EOF
. $HOME/env.sh
qemu-system-x86_64 --version | head -1
grub-mkrescue --version
xorriso -version 2>&1 | head -1
echo "BOOTSTRAP OK — run: . \$HOME/env.sh"
