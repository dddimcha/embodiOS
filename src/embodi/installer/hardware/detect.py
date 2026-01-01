#!/usr/bin/env python3
"""
EMBODIOS Hardware Detection
Detects hardware and suggests optimal configuration
"""

import platform
import subprocess
import psutil
import json
from pathlib import Path

class HardwareDetector:
    """Detect system hardware and capabilities"""
    
    def __init__(self):
        self.info = {}
        
    def detect_all(self):
        """Run all hardware detection"""
        self.info = {
            'system': self._detect_system(),
            'cpu': self._detect_cpu(),
            'memory': self._detect_memory(),
            'storage': self._detect_storage(),
            'network': self._detect_network(),
            'gpu': self._detect_gpu(),
            'peripherals': self._detect_peripherals()
        }
        return self.info
        
    def _detect_system(self):
        """Detect system information"""
        return {
            'platform': platform.system(),
            'release': platform.release(),
            'version': platform.version(),
            'architecture': platform.machine(),
            'processor': platform.processor(),
            'hostname': platform.node()
        }
        
    def _detect_cpu(self):
        """Detect CPU information"""
        info = {
            'count': psutil.cpu_count(logical=False),
            'threads': psutil.cpu_count(logical=True),
            'frequency': psutil.cpu_freq().current if psutil.cpu_freq() else 0,
            'model': platform.processor()
        }
        
        # Try to get more detailed CPU info
        try:
            if platform.system() == 'Linux':
                with open('/proc/cpuinfo') as f:
                    for line in f:
                        if 'model name' in line:
                            info['model'] = line.split(':')[1].strip()
                            break
        except:
            pass
            
        return info
        
    def _detect_memory(self):
        """Detect memory information"""
        mem = psutil.virtual_memory()
        return {
            'total': mem.total,
            'total_gb': round(mem.total / (1024**3), 1),
            'available': mem.available,
            'available_gb': round(mem.available / (1024**3), 1)
        }
        
    def _detect_storage(self):
        """Detect storage devices"""
        devices = []
        
        for partition in psutil.disk_partitions():
            try:
                usage = psutil.disk_usage(partition.mountpoint)
                devices.append({
                    'device': partition.device,
                    'mountpoint': partition.mountpoint,
                    'fstype': partition.fstype,
                    'total': usage.total,
                    'total_gb': round(usage.total / (1024**3), 1),
                    'free_gb': round(usage.free / (1024**3), 1)
                })
            except:
                continue
                
        return devices
        
    def _detect_network(self):
        """Detect network interfaces"""
        interfaces = []
        
        for name, addrs in psutil.net_if_addrs().items():
            interface = {'name': name, 'addresses': []}
            
            for addr in addrs:
                if addr.family == 2:  # AF_INET (IPv4)
                    interface['addresses'].append({
                        'type': 'ipv4',
                        'address': addr.address,
                        'netmask': addr.netmask
                    })
                    
            if interface['addresses']:
                interfaces.append(interface)
                
        return interfaces
        
    def _detect_gpu(self):
        """Detect GPU devices"""
        gpus = []
        
        # Try nvidia-smi
        try:
            result = subprocess.run(['nvidia-smi', '--query-gpu=name,memory.total',
                                   '--format=csv,noheader,nounits'],
                                  capture_output=True, text=True)
            if result.returncode == 0:
                for line in result.stdout.strip().split('\n'):
                    name, memory = line.split(', ')
                    gpus.append({
                        'vendor': 'nvidia',
                        'name': name,
                        'memory_mb': int(memory)
                    })
        except:
            pass
            
        # Try lspci for other GPUs
        try:
            result = subprocess.run(['lspci'], capture_output=True, text=True)
            if result.returncode == 0:
                for line in result.stdout.split('\n'):
                    if 'VGA' in line or 'Display' in line:
                        if 'NVIDIA' not in line:  # Skip if already detected
                            gpus.append({
                                'vendor': 'unknown',
                                'name': line.split(': ')[-1],
                                'memory_mb': 0
                            })
        except:
            pass
            
        return gpus
        
    def _detect_peripherals(self):
        """Detect peripheral devices"""
        peripherals = {
            'usb': self._detect_usb(),
            'serial': self._detect_serial_ports()
        }
        return peripherals
        
    def _detect_usb(self):
        """Detect USB devices"""
        devices = []
        
        try:
            result = subprocess.run(['lsusb'], capture_output=True, text=True)
            if result.returncode == 0:
                for line in result.stdout.strip().split('\n'):
                    if line:
                        devices.append(line)
        except:
            pass
            
        return devices
        
    def _detect_serial_ports(self):
        """Detect serial ports"""
        ports = []
        
        # Common serial port patterns
        patterns = ['/dev/ttyS*', '/dev/ttyUSB*', '/dev/ttyACM*']
        
        for pattern in patterns:
            for port in Path('/dev').glob(pattern.split('/')[-1]):
                ports.append(str(port))
                
        return ports
        
    def suggest_profile(self):
        """Suggest hardware profile based on detection"""
        if not self.info:
            self.detect_all()
            
        mem_gb = self.info['memory']['total_gb']
        cpu_count = self.info['cpu']['count']
        has_gpu = len(self.info['gpu']) > 0
        
        if mem_gb < 2:
            return 'embedded'
        elif mem_gb < 8:
            return 'desktop'
        elif mem_gb >= 16 and cpu_count >= 8:
            return 'server'
        else:
            return 'desktop'
            
    def suggest_model(self):
        """Suggest AI model based on hardware"""
        if not self.info:
            self.detect_all()
            
        mem_gb = self.info['memory']['total_gb']
        
        if mem_gb < 1:
            return 'gpt2'
        elif mem_gb < 2:
            return 'TinyLlama/TinyLlama-1.1B-Chat-v1.0'
        elif mem_gb < 4:
            return 'microsoft/phi-2'
        elif mem_gb < 8:
            return 'mistralai/Mistral-7B-Instruct-v0.2'
        else:
            return 'meta-llama/Llama-2-13b-chat-hf'

def main():
    """Run hardware detection"""
    detector = HardwareDetector()
    info = detector.detect_all()
    
    print("EMBODIOS Hardware Detection")
    print("======================")
    print(f"\nSystem: {info['system']['platform']} {info['system']['architecture']}")
    print(f"CPU: {info['cpu']['model']} ({info['cpu']['count']} cores)")
    print(f"Memory: {info['memory']['total_gb']}GB")
    print(f"GPUs: {len(info['gpu'])}")
    
    print(f"\nSuggested profile: {detector.suggest_profile()}")
    print(f"Suggested model: {detector.suggest_model()}")
    
    # Save detection results
    output_file = Path.home() / '.embodi' / 'hardware-info.json'
    output_file.parent.mkdir(parents=True, exist_ok=True)
    
    with open(output_file, 'w') as f:
        json.dump(info, f, indent=2)
        
    print(f"\nFull report saved to: {output_file}")

if __name__ == '__main__':
    main()