#!/usr/bin/env python3
"""mkesp.py - pure-Python FAT16 ESP image writer for embodiOS UEFI boot.

mtools/mkfs.vfat are unavailable in the sandbox, so this writes a
partition-table-less ("superfloppy") FAT16 image directly. OVMF binds its
FAT driver to such whole-disk volumes and runs the fallback boot path
\\EFI\\BOOT\\BOOTX64.EFI from them.

All file/directory names must be 8.3-safe (they are uppercased); no LFN
entries are emitted.

Usage: mkesp.py <output.img> <src:dst> [<src:dst> ...]
  <src> is a host path, <dst> is the in-image path with '/' separators,
  e.g.  mkesp.py esp.img BOOTX64.EFI:EFI/BOOT/BOOTX64.EFI \\
                    embodios.elf:embodios.elf
"""
import os
import struct
import sys

SECTOR = 512
RESERVED_SECTORS = 1
NUM_FATS = 2
ROOT_ENTRIES = 512          # 32 sectors of root directory
MEDIA = 0xF8


def align_up(v, a):
    return (v + a - 1) // a * a


def fat_name(name):
    """Convert an 8.3 name to the 11-byte directory format."""
    name = name.upper()
    if '.' in name:
        base, ext = name.split('.', 1)
    else:
        base, ext = name, ''
    if len(base) > 8 or len(ext) > 3 or not base:
        raise ValueError('not an 8.3 name: %r' % name)
    for ch in base + ext:
        if ch in ' *?/\\|<>":+,;=[]' or ord(ch) < 0x21:
            raise ValueError('illegal character in 8.3 name: %r' % name)
    return (base.ljust(8) + ext.ljust(3)).encode('ascii')


class Image:
    def __init__(self, total_bytes):
        self.files = []       # (host_path, [path components])
        self.total_bytes = total_bytes

    def layout(self):
        # Pick sectors-per-cluster so the cluster count is a valid FAT16
        # count (4085 <= clusters < 65525) and the FAT fits.
        data_bytes = self.total_bytes
        for spc in (1, 2, 4, 8, 16, 32, 64, 128):
            root_secs = ROOT_ENTRIES * 32 // SECTOR
            # Iterate: fat size depends on cluster count which depends on
            # fat size; two fixed-point passes are plenty.
            fat_secs = 1
            for _ in range(4):
                data_secs = (data_bytes // SECTOR - RESERVED_SECTORS
                             - NUM_FATS * fat_secs - root_secs)
                clusters = data_secs // spc
                fat_secs = align_up((clusters + 2) * 2, SECTOR) // SECTOR
            if 4085 <= clusters < 65525:
                break
        else:
            raise ValueError('cannot find FAT16 geometry for %d bytes'
                             % data_bytes)
        self.spc = spc
        self.fat_secs = fat_secs
        self.root_secs = root_secs
        self.clusters = clusters
        self.total_secs = RESERVED_SECTORS + NUM_FATS * fat_secs + root_secs \
            + clusters * spc
        self.data_start = RESERVED_SECTORS + NUM_FATS * fat_secs + root_secs
        return self.total_secs * SECTOR

    # -- cluster allocation -------------------------------------------
    def alloc_chain(self, nclusters):
        start = self.next_cluster
        self.next_cluster += nclusters
        return start

    def cluster_offset(self, c):
        return (self.data_start + (c - 2) * self.spc) * SECTOR

    def set_chain(self, fat, start, n):
        """Record a cluster chain of length n starting at cluster start."""
        for i in range(n):
            nxt = start + i + 1 if i + 1 < n else 0xFFFF
            struct.pack_into('<H', fat, (start + i) * 2, nxt)

    def dir_entry(self, name11, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name11
        e[11] = attr
        struct.pack_into('<H', e, 26, cluster)
        struct.pack_into('<I', e, 28, size)
        return e

    def build(self):
        size = self.layout()
        img = bytearray(size)

        # ---- boot sector / BPB ---------------------------------------
        bs = bytearray(SECTOR)
        bs[0:3] = b'\xEB\x3C\x90'
        bs[3:11] = b'MKESP   '
        struct.pack_into('<H', bs, 11, SECTOR)          # bytes/sector
        bs[13] = self.spc                               # sectors/cluster
        struct.pack_into('<H', bs, 14, RESERVED_SECTORS)
        bs[16] = NUM_FATS
        struct.pack_into('<H', bs, 17, ROOT_ENTRIES)
        tot = self.total_secs
        if tot < 0x10000:
            struct.pack_into('<H', bs, 19, tot)
            struct.pack_into('<I', bs, 32, 0)
        else:
            struct.pack_into('<H', bs, 19, 0)
            struct.pack_into('<I', bs, 32, tot)
        bs[21] = MEDIA
        struct.pack_into('<H', bs, 22, self.fat_secs)
        struct.pack_into('<H', bs, 24, 63)             # sectors/track
        struct.pack_into('<H', bs, 26, 255)            # heads
        struct.pack_into('<I', bs, 28, 0)              # hidden sectors
        bs[36] = 0x80                                   # drive number
        bs[38] = 0x29                                   # extended boot sig
        struct.pack_into('<I', bs, 39, 0x454D4249)      # volume id 'EMBI'
        bs[43:54] = b'EMBODIOS   '
        bs[54:62] = b'FAT16   '
        bs[510:512] = b'\x55\xAA'
        img[0:SECTOR] = bs

        # ---- FATs ------------------------------------------------------
        fat = bytearray(self.fat_secs * SECTOR)
        fat[0] = MEDIA
        fat[1] = 0xFF
        fat[2] = 0xFF                                   # cluster 1: EOC
        fat[3] = 0xFF

        # ---- walk the file list, build directory tree -----------------
        self.next_cluster = 2
        root_dir = bytearray(self.root_secs * SECTOR)
        # node: name11, attr, cluster, size, children(dict), data(bytes|None)
        root_node = {'attr': 0x10, 'children': {}}

        for host_path, dst in self.files:
            with open(host_path, 'rb') as f:
                content = f.read()
            parts = [p for p in dst.split('/') if p]
            node = root_node
            for d in parts[:-1]:
                key = d.upper()
                if key not in node['children']:
                    node['children'][key] = {'attr': 0x10, 'children': {}}
                node = node['children'][key]
            node['children'][parts[-1].upper()] = {
                'attr': 0x20, 'data': content,
            }

        def place(node):
            """Allocate clusters for a node; return (first_cluster, size)."""
            if node['attr'] == 0x20:                    # regular file
                data = node['data']
                n = max(1, align_up(len(data), self.spc * SECTOR)
                        // (self.spc * SECTOR))
                start = self.alloc_chain(n)
                self.set_chain(fat, start, n)
                off = self.cluster_offset(start)
                img[off:off + len(data)] = data
                return start, len(data)
            # directory: serialize children first
            entries = bytearray()
            for name in sorted(node['children']):
                child = node['children'][name]
                c, sz = place(child)
                entries += self.dir_entry(fat_name(name), child['attr'], c, sz)
            n = max(1, align_up(len(entries), self.spc * SECTOR)
                    // (self.spc * SECTOR))
            start = self.alloc_chain(n)
            self.set_chain(fat, start, n)
            off = self.cluster_offset(start)
            img[off:off + len(entries)] = entries
            return start, 0

        # root directory contents live in the fixed root area
        root_entries = bytearray()
        for name in sorted(root_node['children']):
            child = root_node['children'][name]
            c, sz = place(child)
            root_entries += self.dir_entry(fat_name(name), child['attr'], c, sz)
        if len(root_entries) > len(root_dir):
            raise ValueError('root directory overflow')
        root_off = (RESERVED_SECTORS + NUM_FATS * self.fat_secs) * SECTOR
        img[root_off:root_off + len(root_entries)] = root_entries

        if self.next_cluster - 2 > self.clusters:
            raise ValueError('image overflow: need %d clusters, have %d'
                             % (self.next_cluster - 2, self.clusters))

        # ---- write both FAT copies --------------------------------------
        for i in range(NUM_FATS):
            off = (RESERVED_SECTORS + i * self.fat_secs) * SECTOR
            img[off:off + len(fat)] = fat

        return bytes(img)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    out = sys.argv[1]
    files = []
    total = 0
    for spec in sys.argv[2:]:
        src, dst = spec.split(':', 1)
        sz = os.path.getsize(src)
        total += sz
        files.append((src, dst))
    # image size: data + generous filesystem overhead, rounded to 1 MiB
    img = Image(total + 2 * 1024 * 1024)
    img.files = files
    data = img.build()
    with open(out, 'wb') as f:
        f.write(data)
    print('mkesp: %s: FAT16, %d sectors/cluster, %d clusters, %d bytes, %d file(s)'
          % (out, img.spc, img.clusters, len(data), len(files)))


if __name__ == '__main__':
    main()
