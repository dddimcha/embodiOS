#!/usr/bin/env python3
"""
EMBODIOS ISO Creator
Creates bootable ISO images with AI models
"""

import os
import sys
import shutil
import subprocess
import tempfile
from pathlib import Path
import argparse

class ISOCreator:
    def __init__(self):
        self.work_dir = None
        
    def create_iso(self, model_path, output_path, arch='x86_64', features=None):
        """Create bootable EMBODIOS ISO"""
        features = features or ['gpio', 'uart']
        
        with tempfile.TemporaryDirectory() as work_dir:
            self.work_dir = Path(work_dir)
            
            print(f"Creating EMBODIOS ISO for {arch}")
            
            # Create directory structure
            self._create_structure()
            
            # Copy bootloader
            self._setup_bootloader(arch)
            
            # Copy kernel and model
            self._copy_embodi_files(model_path, arch)
            
            # Create boot configuration
            self._create_boot_config(arch, features)
            
            # Build ISO
            self._build_iso(output_path)
            
            print(f"ISO created: {output_path}")
            
    def _create_structure(self):
        """Create ISO directory structure"""
        dirs = [
            'boot/grub',
            'EFI/BOOT',
            'embodi',
            'isolinux'
        ]
        
        for d in dirs:
            (self.work_dir / d).mkdir(parents=True, exist_ok=True)
            
    def _setup_bootloader(self, arch):
        """Setup bootloader files"""
        print("Setting up bootloader...")
        
        # GRUB for UEFI
        grub_cfg = """set default=0
set timeout=5

menuentry "EMBODIOS - Natural Language OS" {
    set gfxpayload=keep
    linux /embodi/kernel model=/embodi/model.aios quiet
    initrd /embodi/initrd.img
}

menuentry "EMBODIOS - Safe Mode" {
    set gfxpayload=text
    linux /embodi/kernel model=/embodi/model.aios debug nosplash
    initrd /embodi/initrd.img
}
"""
        (self.work_dir / 'boot/grub/grub.cfg').write_text(grub_cfg)
        
        # ISOLINUX for legacy boot
        isolinux_cfg = """DEFAULT embodi
PROMPT 1
TIMEOUT 50

LABEL embodi
    KERNEL /embodi/kernel
    APPEND model=/embodi/model.aios quiet
    
LABEL embodi-debug
    KERNEL /embodi/kernel  
    APPEND model=/embodi/model.aios debug nosplash
"""
        (self.work_dir / 'isolinux/isolinux.cfg').write_text(isolinux_cfg)
        
    def _copy_embodi_files(self, model_path, arch):
        """Copy EMBODIOS kernel and model"""
        print("Copying EMBODIOS files...")
        
        # Copy kernel
        kernel_src = Path(f"build/{arch}/embodi-kernel")
        if kernel_src.exists():
            shutil.copy2(kernel_src, self.work_dir / 'embodi/kernel')
        else:
            # Create dummy kernel for testing
            (self.work_dir / 'embodi/kernel').write_bytes(b'EMBODIOS_KERNEL' * 1000)
            
        # Copy model
        if Path(model_path).exists():
            shutil.copy2(model_path, self.work_dir / 'embodi/model.aios')
        else:
            print(f"Warning: Model not found: {model_path}")
            (self.work_dir / 'embodi/model.aios').write_bytes(b'MODEL' * 1000)
            
        # Create minimal initrd
        self._create_initrd()
        
    def _create_initrd(self):
        """Create minimal initrd"""
        initrd_dir = self.work_dir / 'initrd'
        initrd_dir.mkdir(exist_ok=True)
        
        # Create init script
        init_script = """#!/bin/sh
echo "EMBODIOS Early Boot"
mount -t proc none /proc
mount -t sysfs none /sys
exec /embodi/kernel
"""
        (initrd_dir / 'init').write_text(init_script)
        (initrd_dir / 'init').chmod(0o755)
        
        # Create cpio archive
        cmd = f"cd {initrd_dir} && find . | cpio -o -H newc | gzip > {self.work_dir}/embodi/initrd.img"
        subprocess.run(cmd, shell=True, check=True)
        
    def _create_boot_config(self, arch, features):
        """Create boot configuration"""
        config = {
            'arch': arch,
            'features': features,
            'version': '0.1.0'
        }
        
        import json
        (self.work_dir / 'embodi/config.json').write_text(json.dumps(config, indent=2))
        
    def _build_iso(self, output_path):
        """Build the ISO image"""
        print("Building ISO...")
        
        # Check for tools
        if shutil.which('mkisofs'):
            iso_tool = 'mkisofs'
        elif shutil.which('genisoimage'):
            iso_tool = 'genisoimage'
        else:
            print("Error: mkisofs or genisoimage not found")
            return False
            
        # Build ISO with hybrid boot
        cmd = [
            iso_tool,
            '-o', output_path,
            '-b', 'isolinux/isolinux.bin',
            '-c', 'isolinux/boot.cat',
            '-no-emul-boot',
            '-boot-load-size', '4',
            '-boot-info-table',
            '-eltorito-alt-boot',
            '-e', 'EFI/BOOT/bootx64.efi',
            '-no-emul-boot',
            '-R', '-J', '-v',
            '-V', 'EMBODIOS_OS',
            str(self.work_dir)
        ]
        
        subprocess.run(cmd, check=True)
        
        # Make hybrid for USB boot
        if shutil.which('isohybrid'):
            subprocess.run(['isohybrid', output_path], check=True)

def main():
    parser = argparse.ArgumentParser(description='Create EMBODIOS bootable ISO')
    parser.add_argument('model', help='Path to AI model (.aios)')
    parser.add_argument('output', help='Output ISO file')
    parser.add_argument('--arch', default='x86_64', help='Architecture')
    parser.add_argument('--features', nargs='+', help='Hardware features')
    
    args = parser.parse_args()
    
    creator = ISOCreator()
    creator.create_iso(args.model, args.output, args.arch, args.features)

if __name__ == '__main__':
    main()