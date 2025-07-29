#!/usr/bin/env python3
"""
NOVA Bundler - Create deployable NOVA bundles
"""

import os
import sys
import json
import shutil
import hashlib
import tempfile
import subprocess
from pathlib import Path
from datetime import datetime

class NOVABundler:
    def __init__(self):
        self.bundle_dir = Path.home() / '.nova' / 'bundles'
        self.bundle_dir.mkdir(parents=True, exist_ok=True)
        
    def create_bundle(self, model, output, target='bare-metal', arch='x86_64', 
                     memory='2G', features=None, compress=False):
        """Create a NOVA bundle"""
        features = features or ['gpio', 'uart', 'network']
        
        print(f"Creating NOVA bundle for {model}")
        print(f"Target: {target}, Architecture: {arch}")
        
        with tempfile.TemporaryDirectory() as work_dir:
            work_path = Path(work_dir)
            
            # Find model
            model_path = self._find_model(model)
            if not model_path:
                print(f"Error: Model not found: {model}")
                return False
                
            # Create bundle structure
            self._create_bundle_structure(work_path)
            
            # Copy and configure kernel
            self._prepare_kernel(work_path, arch, features)
            
            # Copy model
            shutil.copy2(model_path, work_path / 'nova/model.aios')
            
            # Create configuration
            self._create_config(work_path, model, arch, memory, features)
            
            # Create bootloader
            self._create_bootloader(work_path, target, arch)
            
            # Package bundle
            if target == 'bare-metal':
                self._create_iso(work_path, output)
            elif target == 'qemu':
                self._create_qemu_image(work_path, output)
            elif target == 'docker':
                self._create_docker_bundle(work_path, output)
            else:
                print(f"Unknown target: {target}")
                return False
                
            if compress and output.exists():
                self._compress_bundle(output)
                
        print(f"Bundle created: {output}")
        return True
        
    def write_bundle(self, bundle_path, device, verify=False):
        """Write bundle to device (USB drive)"""
        if not Path(bundle_path).exists():
            print(f"Error: Bundle not found: {bundle_path}")
            return False
            
        if not device.startswith('/dev/'):
            print(f"Error: Invalid device: {device}")
            return False
            
        print(f"Writing {bundle_path} to {device}")
        print("WARNING: This will erase all data on the device!")
        
        # Confirmation
        response = input("Continue? (yes/no): ")
        if response.lower() != 'yes':
            print("Aborted")
            return False
            
        # Write with dd
        cmd = ['sudo', 'dd', f'if={bundle_path}', f'of={device}', 
               'bs=4M', 'status=progress', 'conv=fsync']
        
        try:
            subprocess.run(cmd, check=True)
            
            if verify:
                print("Verifying...")
                self._verify_write(bundle_path, device)
                
            print("Write complete!")
            return True
            
        except subprocess.CalledProcessError as e:
            print(f"Error writing to device: {e}")
            return False
            
    def _find_model(self, model_name):
        """Find model file"""
        # Check in models directory
        models_dir = Path.home() / '.nova' / 'models'
        
        # Try exact path first
        if Path(model_name).exists():
            return Path(model_name)
            
        # Try in models directory
        possible_names = [
            f"{model_name}.aios",
            f"{model_name.replace('/', '-')}.aios",
            f"huggingface_{model_name.replace('/', '-')}.aios"
        ]
        
        for name in possible_names:
            path = models_dir / name
            if path.exists():
                return path
                
        return None
        
    def _create_bundle_structure(self, work_path):
        """Create bundle directory structure"""
        dirs = [
            'nova',
            'boot',
            'boot/grub',
            'EFI/BOOT',
            'config'
        ]
        
        for d in dirs:
            (work_path / d).mkdir(parents=True, exist_ok=True)
            
    def _prepare_kernel(self, work_path, arch, features):
        """Prepare NOVA kernel"""
        # In production, this would compile kernel with specific features
        # For now, copy pre-built or create dummy
        
        kernel_path = Path('build') / arch / 'nova-kernel'
        
        if kernel_path.exists():
            shutil.copy2(kernel_path, work_path / 'nova/kernel')
        else:
            # Create dummy kernel
            dummy_kernel = b'NOVA_KERNEL_' + arch.encode() + b'\n'
            dummy_kernel += b'FEATURES: ' + ','.join(features).encode() + b'\n'
            dummy_kernel += b'\x00' * 1024 * 100  # 100KB dummy
            
            (work_path / 'nova/kernel').write_bytes(dummy_kernel)
            
        # Make executable
        (work_path / 'nova/kernel').chmod(0o755)
        
    def _create_config(self, work_path, model, arch, memory, features):
        """Create NOVA configuration"""
        config = {
            'version': '0.1.0',
            'model': model,
            'arch': arch,
            'memory': memory,
            'features': features,
            'created': datetime.now().isoformat(),
            'checksum': self._calculate_checksum(work_path / 'nova/model.aios')
        }
        
        (work_path / 'config/nova.json').write_text(json.dumps(config, indent=2))
        
    def _create_bootloader(self, work_path, target, arch):
        """Create bootloader configuration"""
        if target == 'bare-metal':
            # GRUB configuration
            grub_cfg = f"""set default=0
set timeout=5
set gfxmode=1024x768

menuentry "NOVA - {arch}" {{
    set gfxpayload=keep
    linux /nova/kernel model=/nova/model.aios config=/config/nova.json
    boot
}}

menuentry "NOVA - Recovery Mode" {{
    set gfxpayload=text  
    linux /nova/kernel model=/nova/model.aios config=/config/nova.json recovery
    boot
}}
"""
            (work_path / 'boot/grub/grub.cfg').write_text(grub_cfg)
            
            # UEFI startup script
            startup_nsh = """@echo -off
echo "Starting NOVA..."
\nova\kernel model=\nova\model.aios config=\config\nova.json
"""
            (work_path / 'EFI/BOOT/startup.nsh').write_text(startup_nsh)
            
    def _create_iso(self, work_path, output):
        """Create bootable ISO"""
        from nova.nova_installer.iso.create_iso import ISOCreator
        
        creator = ISOCreator()
        # Use the work_path directly since we already set it up
        creator.work_dir = work_path
        creator._build_iso(output)
        
    def _create_qemu_image(self, work_path, output):
        """Create QEMU disk image"""
        # Create raw disk image
        img_size = '2G'
        subprocess.run(['qemu-img', 'create', '-f', 'raw', output, img_size], check=True)
        
        # Create partition and filesystem
        # This would need proper implementation with loop devices
        print("QEMU image creation not fully implemented yet")
        
    def _create_docker_bundle(self, work_path, output):
        """Create Docker bundle"""
        # Create Dockerfile
        dockerfile = """FROM nova-base:latest
COPY nova /nova
COPY config /config
CMD ["/nova/kernel"]
"""
        (work_path / 'Dockerfile').write_text(dockerfile)
        
        # Build Docker image
        tag = output.stem
        subprocess.run(['docker', 'build', '-t', tag, str(work_path)], check=True)
        
    def _calculate_checksum(self, file_path):
        """Calculate file checksum"""
        sha256 = hashlib.sha256()
        with open(file_path, 'rb') as f:
            for chunk in iter(lambda: f.read(8192), b''):
                sha256.update(chunk)
        return sha256.hexdigest()
        
    def _compress_bundle(self, bundle_path):
        """Compress bundle"""
        print("Compressing bundle...")
        subprocess.run(['gzip', '-9', str(bundle_path)], check=True)
        
    def _verify_write(self, bundle_path, device):
        """Verify written data"""
        bundle_size = Path(bundle_path).stat().st_size
        bundle_hash = self._calculate_checksum(bundle_path)
        
        # Read back and verify
        print("Reading back from device...")
        with open(device, 'rb') as f:
            device_data = f.read(bundle_size)
            
        device_hash = hashlib.sha256(device_data).hexdigest()
        
        if bundle_hash == device_hash:
            print("Verification successful!")
        else:
            print("Verification FAILED!")
            print(f"Expected: {bundle_hash}")
            print(f"Got:      {device_hash}")

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='NOVA Bundle Manager')
    subparsers = parser.add_subparsers(dest='command', help='Commands')
    
    # Create command
    create_parser = subparsers.add_parser('create', help='Create bundle')
    create_parser.add_argument('--model', required=True, help='Model name or path')
    create_parser.add_argument('--output', required=True, help='Output file')
    create_parser.add_argument('--target', default='bare-metal', 
                              choices=['bare-metal', 'qemu', 'docker'])
    create_parser.add_argument('--arch', default='x86_64')
    create_parser.add_argument('--memory', default='2G')
    create_parser.add_argument('--features', nargs='+')
    create_parser.add_argument('--compress', action='store_true')
    
    # Write command
    write_parser = subparsers.add_parser('write', help='Write to device')
    write_parser.add_argument('bundle', help='Bundle file')
    write_parser.add_argument('device', help='Target device')
    write_parser.add_argument('--verify', action='store_true')
    
    args = parser.parse_args()
    
    bundler = NOVABundler()
    
    if args.command == 'create':
        bundler.create_bundle(
            model=args.model,
            output=Path(args.output),
            target=args.target,
            arch=args.arch,
            memory=args.memory,
            features=args.features,
            compress=args.compress
        )
    elif args.command == 'write':
        bundler.write_bundle(args.bundle, args.device, args.verify)
    else:
        parser.print_help()

if __name__ == '__main__':
    main()