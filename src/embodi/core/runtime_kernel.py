"""
EMBODIOS Runtime Kernel - The actual OS kernel powered by AI
"""

import os
import sys
import time
import threading
import queue
import signal
from typing import Dict, List, Optional, Any, Callable
from dataclasses import dataclass
from enum import Enum

# Import EMBODIOS components
from embodi.core.hal import HardwareAbstractionLayer, NaturalLanguageHAL
from embodi.core.inference import EMBODIOSInferenceEngine
from embodi.core.nl_processor import EMBODIOSCommandProcessor

class SystemState(Enum):
    """System states"""
    BOOTING = "booting"
    RUNNING = "running"
    SUSPENDED = "suspended"
    HALTING = "halting"
    HALTED = "halted"

@dataclass
class SystemEvent:
    """System event"""
    event_type: str
    source: str
    data: Any
    timestamp: float

class EMBODIOSKernel:
    """Main EMBODIOS kernel - AI-powered operating system"""
    
    def __init__(self, model_path: str, hardware_config: Optional[Dict] = None):
        self.model_path = model_path
        self.hardware_config = hardware_config or {}
        self.state = SystemState.BOOTING
        
        # Core components
        self.hal = None
        self.inference_engine = None
        self.command_processor = None
        self.nl_hal = None
        
        # System management
        self.event_queue: queue.Queue[SystemEvent] = queue.Queue()
        self.threads: Dict[str, threading.Thread] = {}
        self.interrupt_handlers: Dict[int, Callable] = {}
        self.system_memory: Dict[str, Dict[str, int]] = {}
        
        # Statistics
        self.boot_time = time.time()
        self.command_count = 0
        self.inference_cycles = 0
        
    def boot(self):
        """Boot the EMBODIOS system"""
        print("EMBODIOS v0.1.0 - AI Operating System")
        print("=====================================")
        
        try:
            self._boot_sequence()
            self.state = SystemState.RUNNING
            print("\nSystem ready. Type commands in natural language.\n")
            self._main_loop()
            
        except KeyboardInterrupt:
            print("\nShutdown requested...")
            self.shutdown()
        except Exception as e:
            print(f"\nKernel panic: {e}")
            self._kernel_panic(str(e))
    
    def _boot_sequence(self):
        """Execute boot sequence"""
        
        # Phase 1: Initialize hardware
        print("[BOOT] Initializing hardware abstraction layer...")
        self.hal = HardwareAbstractionLayer()
        self.hal.initialize()
        
        # Phase 2: Load AI model
        print(f"[BOOT] Loading AI model: {self.model_path}")
        self.inference_engine = EMBODIOSInferenceEngine()
        
        if os.path.exists(self.model_path):
            self.inference_engine.load_model(self.model_path)
        else:
            print("[BOOT] Warning: Model file not found, using minimal mode")
        
        # Phase 3: Initialize subsystems
        print("[BOOT] Initializing subsystems...")
        self._init_memory_management()
        self._init_interrupt_system()
        self._init_device_drivers()
        
        # Phase 4: Initialize command processor
        print("[BOOT] Initializing natural language processor...")
        self.command_processor = EMBODIOSCommandProcessor(
            self.hal, 
            self.inference_engine
        )
        self.nl_hal = NaturalLanguageHAL(self.hal)
        
        # Phase 5: Start system services
        print("[BOOT] Starting system services...")
        self._start_system_services()
        
        # Phase 6: Run hardware detection
        print("[BOOT] Detecting hardware...")
        self._detect_hardware()
        
        print("[BOOT] Boot sequence complete")
        print(f"[BOOT] Boot time: {time.time() - self.boot_time:.2f} seconds")
    
    def _init_memory_management(self):
        """Initialize memory management"""
        # Simple memory regions for AI OS
        self.system_memory = {
            'kernel': {'start': 0x0, 'size': 16 * 1024 * 1024},  # 16MB
            'model': {'start': 0x1000000, 'size': 256 * 1024 * 1024},  # 256MB
            'heap': {'start': 0x11000000, 'size': 128 * 1024 * 1024},  # 128MB
            'stack': {'start': 0x19000000, 'size': 8 * 1024 * 1024},  # 8MB
            'mmio': {'start': 0x20000000, 'size': 256 * 1024 * 1024},  # 256MB
        }
    
    def _init_interrupt_system(self):
        """Initialize interrupt handling"""
        # Register interrupt handlers
        self.interrupt_handlers = {
            0: self._handle_timer_interrupt,
            1: self._handle_keyboard_interrupt,
            16: self._handle_gpio_interrupt,
            32: self._handle_uart_interrupt,
            48: self._handle_i2c_interrupt,
        }
        
        # Start interrupt monitoring thread
        self.threads['interrupt_monitor'] = threading.Thread(
            target=self._interrupt_monitor_thread,
            daemon=True
        )
        self.threads['interrupt_monitor'].start()
    
    def _init_device_drivers(self):
        """Initialize device drivers"""
        # Initialize configured hardware
        if 'gpio' in self.hardware_config.get('enabled', []):
            gpio = self.hal.get_device('gpio')
            if hasattr(gpio, 'init'):
                gpio.init()
        
        if 'i2c' in self.hardware_config.get('enabled', []):
            i2c = self.hal.get_device('i2c')
            if hasattr(i2c, 'init'):
                i2c.init()
        
        if 'uart' in self.hardware_config.get('enabled', []):
            uart = self.hal.get_device('uart')
            if hasattr(uart, 'init'):
                uart.init()
    
    def _start_system_services(self):
        """Start background system services"""
        
        # Hardware monitoring service
        self.threads['hw_monitor'] = threading.Thread(
            target=self._hardware_monitor_thread,
            daemon=True
        )
        self.threads['hw_monitor'].start()
        
        # AI inference service
        self.threads['ai_service'] = threading.Thread(
            target=self._ai_service_thread,
            daemon=True
        )
        self.threads['ai_service'].start()
    
    def _detect_hardware(self):
        """Detect and enumerate hardware"""
        devices = []
        
        # Check GPIO
        try:
            gpio = self.hal.get_device('gpio')
            if gpio:
                devices.append("GPIO controller")
        except:
            pass
        
        # Check I2C devices
        try:
            i2c = self.hal.get_device('i2c')
            if i2c and hasattr(i2c, 'init'):
                i2c.init()
                devices.append("I2C bus")
                # Scan for devices
                if hasattr(i2c, 'read'):
                    for addr in range(0x08, 0x78):
                        try:
                            i2c.read(addr, 0, 1)
                            devices.append(f"  - I2C device at 0x{addr:02X}")
                        except:
                            pass
        except:
            pass
        
        # Check UART
        try:
            uart = self.hal.get_device('uart')
            if uart:
                devices.append("UART controller")
        except:
            pass
        
        print(f"[BOOT] Detected {len(devices)} devices:")
        for device in devices:
            print(f"  {device}")
    
    def _main_loop(self):
        """Main kernel loop"""
        
        # Set up signal handlers
        signal.signal(signal.SIGINT, lambda s, f: self.shutdown())
        
        while self.state == SystemState.RUNNING:
            try:
                # Get user input
                user_input = input("> ")
                
                if not user_input.strip():
                    continue
                
                # Process through AI
                self._process_command(user_input)
                
            except EOFError:
                # Handle Ctrl+D
                break
            except Exception as e:
                print(f"Error: {e}")
    
    def _process_command(self, command: str):
        """Process user command through AI"""
        
        self.command_count += 1
        
        # Special system commands
        if command.lower() in ['exit', 'quit', 'shutdown']:
            self.shutdown()
            return
        
        if command.lower() == 'status':
            self._show_status()
            return
        
        # Process through natural language processor
        try:
            if self.command_processor:
                response = self.command_processor.process_input(command)
            else:
                response = "Command processor not initialized"
            print(f"AI: {response}")
            
        except Exception as e:
            print(f"AI: Error processing command - {e}")
    
    def _show_status(self):
        """Show system status"""
        uptime = time.time() - self.boot_time
        
        print("\nSystem Status")
        print("=============")
        print(f"State: {self.state.value}")
        print(f"Uptime: {uptime:.0f} seconds")
        print(f"Commands processed: {self.command_count}")
        print(f"AI inference cycles: {self.inference_cycles}")
        print(f"Model: {os.path.basename(self.model_path)}")
        
        # Memory usage
        print("\nMemory Regions:")
        for name, region in self.system_memory.items():
            size_mb = region['size'] / (1024 * 1024)
            print(f"  {name}: {size_mb:.1f} MB at 0x{region['start']:08X}")
        
        # Active threads
        print(f"\nActive threads: {len(self.threads)}")
        for name, thread in self.threads.items():
            print(f"  {name}: {'alive' if thread.is_alive() else 'dead'}")
        
        print()
    
    def _interrupt_monitor_thread(self):
        """Monitor for hardware interrupts"""
        
        while self.state == SystemState.RUNNING:
            try:
                # Check for interrupts (simulated)
                # In real implementation, would check interrupt controller
                
                # Check GPIO interrupts
                if self.hal:
                    gpio = self.hal.get_device('gpio')
                    if gpio and hasattr(gpio, 'check_interrupts'):
                        irqs = gpio.check_interrupts()
                        for irq in irqs:
                            self._handle_interrupt(16, {'pin': irq})
                
                time.sleep(0.01)  # 10ms polling
                
            except Exception as e:
                print(f"Interrupt monitor error: {e}")
    
    def _hardware_monitor_thread(self):
        """Monitor hardware health"""
        
        while self.state == SystemState.RUNNING:
            try:
                # Collect hardware metrics
                event = SystemEvent(
                    event_type='hardware_status',
                    source='hw_monitor',
                    data={
                        'timestamp': time.time(),
                        'devices': len(self.hal.devices)
                    },
                    timestamp=time.time()
                )
                
                self.event_queue.put(event)
                
                time.sleep(5)  # Check every 5 seconds
                
            except Exception as e:
                print(f"Hardware monitor error: {e}")
    
    def _ai_service_thread(self):
        """Background AI inference service"""
        
        while self.state == SystemState.RUNNING:
            try:
                # Process system events through AI
                if not self.event_queue.empty():
                    event = self.event_queue.get(timeout=1)
                    self._process_system_event(event)
                
                # Run periodic inference
                self.inference_cycles += 1
                
                time.sleep(0.1)  # 100ms cycle
                
            except queue.Empty:
                pass
            except Exception as e:
                print(f"AI service error: {e}")
    
    def _process_system_event(self, event: SystemEvent):
        """Process system event through AI"""
        
        # Convert event to natural language
        event_text = f"System event: {event.event_type} from {event.source}"
        
        # Let AI decide if action needed
        if self.inference_engine and self.inference_engine.model_loaded:
            # Simple processing for now
            pass
    
    def _handle_interrupt(self, irq: int, data: Optional[Dict] = None):
        """Handle hardware interrupt"""
        
        if irq in self.interrupt_handlers:
            handler = self.interrupt_handlers[irq]
            handler(data)
        else:
            print(f"Unhandled interrupt: IRQ {irq}")
    
    def _handle_timer_interrupt(self, data: Optional[Dict]):
        """Handle timer interrupt"""
        pass
    
    def _handle_keyboard_interrupt(self, data: Optional[Dict]):
        """Handle keyboard interrupt"""
        pass
    
    def _handle_gpio_interrupt(self, data: Optional[Dict]):
        """Handle GPIO interrupt"""
        pin = data.get('pin', 0) if data else 0
        value = data.get('value', 0) if data else 0
        
        # Let AI handle the interrupt
        event_text = f"GPIO interrupt on pin {pin}, value {value}"
        if self.command_processor:
            response = self.command_processor.process_input(event_text)
        else:
            response = "Command processor not initialized"
        
        print(f"\n[INTERRUPT] {event_text}")
        print(f"AI: {response}")
        print("> ", end='', flush=True)
    
    def _handle_uart_interrupt(self, data: Optional[Dict]):
        """Handle UART interrupt"""
        if self.hal:
            uart = self.hal.get_device('uart')
            if uart and hasattr(uart, 'available') and uart.available():
                uart_data = uart.read(1) if hasattr(uart, 'read') else None
                if uart_data:
                    print(f"\n[UART RX] {uart_data}")
                    print("> ", end='', flush=True)
    
    def _handle_i2c_interrupt(self, data: Optional[Dict]):
        """Handle I2C interrupt"""
        pass
    
    def shutdown(self):
        """Shutdown the system"""
        
        if self.state == SystemState.HALTING:
            return
        
        self.state = SystemState.HALTING
        print("\nShutting down EMBODIOS...")
        
        # Stop services
        print("Stopping services...")
        for name, thread in self.threads.items():
            print(f"  Stopping {name}...")
        
        # Save state
        print("Saving system state...")
        
        # Cleanup hardware
        print("Releasing hardware...")
        if self.hal:
            for device_name in self.hal.devices:
                print(f"  Releasing {device_name}...")
        
        self.state = SystemState.HALTED
        print("System halted.")
        sys.exit(0)
    
    def _kernel_panic(self, message: str):
        """Handle kernel panic"""
        
        print("\n" + "="*60)
        print("KERNEL PANIC")
        print("="*60)
        print(f"Error: {message}")
        print(f"State: {self.state.value}")
        print(f"Uptime: {time.time() - self.boot_time:.2f}s")
        print("\nSystem halted. Please restart.")
        
        self.state = SystemState.HALTED
        
        # Infinite loop
        while True:
            time.sleep(1)


class EMBODIOSRunner:
    """Runner for EMBODIOS in different modes"""
    
    def __init__(self):
        self.kernel = None
    
    def run_interactive(self, model_path: str, hardware_config: Optional[Dict] = None):
        """Run EMBODIOS in interactive mode"""
        
        hardware_config = hardware_config or {
            'enabled': ['gpio', 'i2c', 'uart'],
            'platform': 'generic'
        }
        
        self.kernel = EMBODIOSKernel(model_path, hardware_config)
        self.kernel.boot()
    
    def run_bare_metal(self, model_path: str):
        """Run EMBODIOS in bare metal mode (simulated)"""
        
        print("EMBODIOS Bare Metal Mode")
        print("========================")
        print("Note: This is a simulation. Real bare metal requires bootloader.")
        print()
        
        hardware_config = {
            'enabled': ['gpio', 'i2c', 'uart', 'spi'],
            'platform': 'bare_metal',
            'memory_limit': 512 * 1024 * 1024  # 512MB
        }
        
        self.kernel = EMBODIOSKernel(model_path, hardware_config)
        self.kernel.boot()
    
    def run_container(self, image: str, **kwargs):
        """Run EMBODIOS container"""
        
        print(f"Starting EMBODIOS container: {image}")
        
        # Parse image name
        if ':' in image:
            name, tag = image.split(':', 1)
        else:
            name, tag = image, 'latest'
        
        # Find model file
        models_dir = os.path.expanduser('~/.embodi/models')
        model_path = os.path.join(models_dir, f"{name}-{tag}.aios")
        
        if not os.path.exists(model_path):
            # Try without tag
            model_path = os.path.join(models_dir, f"{name}.aios")
        
        if not os.path.exists(model_path):
            print(f"Error: Model not found: {image}")
            return
        
        # Run with container config
        hardware_config = {
            'enabled': kwargs.get('hardware', ['gpio']),
            'platform': 'container',
            'memory_limit': kwargs.get('memory', '2G')
        }
        
        self.run_interactive(model_path, hardware_config)


def main():
    """Main entry point for EMBODIOS runtime"""
    
    import argparse
    
    parser = argparse.ArgumentParser(description='EMBODIOS Runtime')
    parser.add_argument('command', choices=['run', 'bare-metal', 'test'])
    parser.add_argument('model', help='Model file or image name')
    parser.add_argument('--hardware', nargs='+', default=['gpio'],
                       help='Hardware to enable')
    parser.add_argument('--memory', default='2G', help='Memory limit')
    parser.add_argument('--debug', action='store_true', help='Debug mode')
    
    args = parser.parse_args()
    
    runner = EMBODIOSRunner()
    
    if args.command == 'run':
        # Interactive mode
        runner.run_interactive(args.model, {
            'enabled': args.hardware,
            'debug': args.debug
        })
    
    elif args.command == 'bare-metal':
        # Bare metal simulation
        runner.run_bare_metal(args.model)
    
    elif args.command == 'test':
        # Test mode
        print("EMBODIOS Test Mode")
        print("==================")
        
        # Create test model if needed
        test_model = 'test-model.aios'
        if not os.path.exists(test_model):
            print("Creating test model...")
            # Would create minimal test model
        
        runner.run_interactive(test_model, {
            'enabled': ['gpio'],
            'platform': 'test'
        })


if __name__ == '__main__':
    main()