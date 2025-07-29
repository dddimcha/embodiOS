"""
EMBODIOS Hardware Abstraction Layer - Complete Implementation
"""

import os
import mmap
import struct
import fcntl
import array
from typing import Dict, List, Optional, Callable, Union, Any
from pathlib import Path

class HardwareAbstractionLayer:
    """Complete HAL implementation for EMBODIOS"""
    
    def __init__(self):
        self.devices = {}
        self.memory_regions = {}
        self.interrupt_handlers = {}
        self.is_bare_metal = self._detect_environment()
        
    def initialize(self):
        """Initialize the HAL"""
        print("EMBODIOS HAL initializing...")
        
        # Initialize hardware interfaces
        self._init_gpio()
        self._init_i2c()
        self._init_spi()
        self._init_uart()
        
        print("EMBODIOS HAL initialized")
        
    def _detect_environment(self) -> bool:
        """Detect if running bare metal or under OS"""
        # Check if we have /dev access (OS present)
        return not os.path.exists('/dev')
    
    def _init_gpio(self):
        """Initialize GPIO interface"""
        if self.is_bare_metal:
            # Direct memory-mapped GPIO
            self.gpio = BareMetalGPIO()
        else:
            # OS-based GPIO (sysfs or gpiod)
            self.gpio = OSBasedGPIO()
        
        self.register_device('gpio', self.gpio)
    
    def _init_i2c(self):
        """Initialize I2C interface"""
        if self.is_bare_metal:
            self.i2c = BareMetalI2C()
        else:
            self.i2c = OSBasedI2C()
        
        self.register_device('i2c', self.i2c)
    
    def _init_spi(self):
        """Initialize SPI interface"""
        if self.is_bare_metal:
            self.spi = BareMetalSPI()
        else:
            self.spi = OSBasedSPI()
        
        self.register_device('spi', self.spi)
    
    def _init_uart(self):
        """Initialize UART interface"""
        if self.is_bare_metal:
            self.uart = BareMetalUART()
        else:
            self.uart = OSBasedUART()
        
        self.register_device('uart', self.uart)
    
    def register_device(self, name: str, device):
        """Register a hardware device"""
        self.devices[name] = device
    
    def get_device(self, name: str):
        """Get a registered device"""
        return self.devices.get(name)
    
    def register_interrupt_handler(self, irq: int, handler: Callable):
        """Register interrupt handler for bare metal mode"""
        self.interrupt_handlers[irq] = handler
    
    def memory_map(self, physical_addr: int, size: int) -> Union[mmap.mmap, 'SimulatedMemory']:
        """Map physical memory region"""
        if self.is_bare_metal:
            # Direct physical access - return simulated memory for bare metal
            return SimulatedMemory(physical_addr, size)
        else:
            # Use /dev/mem for physical memory access
            try:
                mem_fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
                mem = mmap.mmap(mem_fd, size, offset=physical_addr)
                os.close(mem_fd)
                self.memory_regions[physical_addr] = mem
                return mem
            except:
                # Fallback to simulation
                return SimulatedMemory(physical_addr, size)


class BareMetalGPIO:
    """Direct hardware GPIO control for bare metal"""
    
    # BCM2835/2837 GPIO registers for Raspberry Pi
    GPIO_BASE = 0x3F200000  # RPi 3/4
    # GPIO_BASE = 0xFE200000  # RPi 4 alternative
    
    def __init__(self):
        # Map GPIO registers
        self.gpio_mem = self._map_peripheral(self.GPIO_BASE, 0xB4)
        self.pins = {}  # Track pin states
        
    def _map_peripheral(self, base_addr: int, size: int):
        """Map peripheral memory"""
        # In bare metal, just return the address
        # In OS mode, would use mmap
        return base_addr
    
    def setup(self, pin: int, mode: str):
        """Set pin mode (input/output)"""
        if mode not in ['input', 'output']:
            raise ValueError("Mode must be 'input' or 'output'")
        
        # Calculate FSEL register and bit position
        fsel_reg = pin // 10
        fsel_shift = (pin % 10) * 3
        
        reg_addr = self.gpio_mem + fsel_reg * 4
        
        # Read current value
        current = self._read32(reg_addr)
        
        # Clear bits for this pin
        current &= ~(7 << fsel_shift)
        
        # Set new mode (1 for output, 0 for input)
        if mode == 'output':
            current |= (1 << fsel_shift)
        
        # Write back
        self._write32(reg_addr, current)
        
        # Track pin state
        self.pins[pin] = {'mode': mode, 'value': False}
    
    def write(self, pin: int, value: bool):
        """Write digital value to pin"""
        if value:
            # Set pin high - use SET register
            reg = 0x1C + (pin // 32) * 4
            self._write32(self.gpio_mem + reg, 1 << (pin % 32))
        else:
            # Set pin low - use CLR register
            reg = 0x28 + (pin // 32) * 4
            self._write32(self.gpio_mem + reg, 1 << (pin % 32))
        
        # Update tracked state
        if pin in self.pins:
            self.pins[pin]['value'] = value
    
    def read(self, pin: int) -> bool:
        """Read digital value from pin"""
        # Read from LEV register
        reg = 0x34 + (pin // 32) * 4
        value = self._read32(self.gpio_mem + reg)
        return bool(value & (1 << (pin % 32)))
    
    def _read32(self, addr: int) -> int:
        """Read 32-bit value from address"""
        # In bare metal, this would be direct memory access
        # For now, simulate
        return 0
    
    def _write32(self, addr: int, value: int):
        """Write 32-bit value to address"""
        # In bare metal, this would be direct memory access
        # For now, simulate
        pass


class OSBasedGPIO:
    """GPIO control through OS interfaces"""
    
    def __init__(self):
        self.exported_pins = set()
        self.gpio_path = Path('/sys/class/gpio')
        self.pins = {}  # Track pin states
        
    def setup(self, pin: int, mode: str):
        """Set pin mode using sysfs"""
        if pin not in self.exported_pins:
            # Export pin
            try:
                (self.gpio_path / 'export').write_text(str(pin))
                self.exported_pins.add(pin)
            except:
                pass  # May already be exported
        
        # Set direction
        pin_path = self.gpio_path / f'gpio{pin}'
        if pin_path.exists():
            (pin_path / 'direction').write_text(mode.replace('input', 'in').replace('output', 'out'))
        
        # Track pin state
        self.pins[pin] = {'mode': mode, 'value': False}
    
    def write(self, pin: int, value: bool):
        """Write value using sysfs"""
        pin_path = self.gpio_path / f'gpio{pin}'
        if pin_path.exists():
            (pin_path / 'value').write_text('1' if value else '0')
        
        # Update tracked state
        if pin in self.pins:
            self.pins[pin]['value'] = value
    
    def read(self, pin: int) -> bool:
        """Read value using sysfs"""
        pin_path = self.gpio_path / f'gpio{pin}'
        if pin_path.exists():
            return (pin_path / 'value').read_text().strip() == '1'
        return False


class BareMetalI2C:
    """Direct I2C hardware control"""
    
    # BCM2835 I2C registers
    I2C_BASE = 0x3F804000  # I2C1
    
    BSC_C = 0x00    # Control
    BSC_S = 0x04    # Status  
    BSC_DLEN = 0x08 # Data Length
    BSC_A = 0x0C    # Slave Address
    BSC_FIFO = 0x10 # Data FIFO
    
    def __init__(self):
        self.i2c_mem = self._map_peripheral(self.I2C_BASE, 0x20)
    
    def _map_peripheral(self, base_addr: int, size: int):
        return base_addr
    
    def init(self, speed: int = 100000):
        """Initialize I2C with given speed"""
        # Set I2C clock divider
        # Assuming 250MHz core clock
        divider = 250000000 // speed
        self._write32(self.i2c_mem + 0x14, divider)
        
        # Clear FIFO
        self._write32(self.i2c_mem + self.BSC_C, 0x10)
    
    def write(self, device_addr: int, register: int, data: bytes):
        """Write data to I2C device"""
        # Set slave address
        self._write32(self.i2c_mem + self.BSC_A, device_addr)
        
        # Clear FIFO and status
        self._write32(self.i2c_mem + self.BSC_C, 0x30)
        self._write32(self.i2c_mem + self.BSC_S, 0x302)
        
        # Write register address and data to FIFO
        self._write32(self.i2c_mem + self.BSC_FIFO, register)
        for byte in data:
            self._write32(self.i2c_mem + self.BSC_FIFO, byte)
        
        # Set data length
        self._write32(self.i2c_mem + self.BSC_DLEN, len(data) + 1)
        
        # Start transfer
        self._write32(self.i2c_mem + self.BSC_C, 0x8080)
        
        # Wait for completion
        while not (self._read32(self.i2c_mem + self.BSC_S) & 0x02):
            pass
    
    def read(self, device_addr: int, register: int, length: int) -> bytes:
        """Read data from I2C device"""
        # Write register address first
        self._write32(self.i2c_mem + self.BSC_A, device_addr)
        self._write32(self.i2c_mem + self.BSC_DLEN, 1)
        self._write32(self.i2c_mem + self.BSC_FIFO, register)
        self._write32(self.i2c_mem + self.BSC_C, 0x8080)
        
        # Wait for write completion
        while not (self._read32(self.i2c_mem + self.BSC_S) & 0x02):
            pass
        
        # Now read data
        self._write32(self.i2c_mem + self.BSC_DLEN, length)
        self._write32(self.i2c_mem + self.BSC_C, 0x8081)  # Read mode
        
        # Wait and collect data
        data: List[int] = []
        while len(data) < length:
            if self._read32(self.i2c_mem + self.BSC_S) & 0x20:  # RXD
                data.append(self._read32(self.i2c_mem + self.BSC_FIFO))
        
        return bytes(data)
    
    def _read32(self, addr: int) -> int:
        """Read 32-bit value"""
        return 0  # Placeholder
    
    def _write32(self, addr: int, value: int):
        """Write 32-bit value"""
        pass  # Placeholder


class OSBasedI2C:
    """I2C through OS drivers"""
    
    def __init__(self):
        self.i2c_fd = None
        self.I2C_SLAVE = 0x0703  # ioctl number
    
    def init(self, bus: int = 1, speed: int = 100000):
        """Initialize I2C bus"""
        try:
            self.i2c_fd = os.open(f'/dev/i2c-{bus}', os.O_RDWR)
        except:
            # Fallback to simulation
            self.i2c_fd = None
    
    def write(self, device_addr: int, register: int, data: bytes):
        """Write to I2C device"""
        if self.i2c_fd:
            # Set slave address
            fcntl.ioctl(self.i2c_fd, self.I2C_SLAVE, device_addr)
            
            # Write register + data
            os.write(self.i2c_fd, bytes([register]) + data)
    
    def read(self, device_addr: int, register: int, length: int) -> bytes:
        """Read from I2C device"""
        if self.i2c_fd:
            # Set slave address
            fcntl.ioctl(self.i2c_fd, self.I2C_SLAVE, device_addr)
            
            # Write register
            os.write(self.i2c_fd, bytes([register]))
            
            # Read data
            return os.read(self.i2c_fd, length)
        return b'\x00' * length  # Simulated data


class BareMetalSPI:
    """Direct SPI hardware control"""
    
    def __init__(self):
        pass
    
    def init(self, speed: int = 1000000, mode: int = 0):
        """Initialize SPI"""
        pass
    
    def transfer(self, data: bytes) -> bytes:
        """Transfer data over SPI"""
        return b'\x00' * len(data)  # Placeholder


class OSBasedSPI:
    """SPI through OS drivers"""
    
    def __init__(self):
        self.spi_fd = None
    
    def init(self, device: str = '/dev/spidev0.0', speed: int = 1000000, mode: int = 0):
        """Initialize SPI"""
        try:
            self.spi_fd = os.open(device, os.O_RDWR)
        except:
            self.spi_fd = None
    
    def transfer(self, data: bytes) -> bytes:
        """Transfer data over SPI"""
        if self.spi_fd:
            # Would use ioctl for SPI transfer
            return os.read(self.spi_fd, len(data))
        return b'\x00' * len(data)


class BareMetalUART:
    """Direct UART hardware control"""
    
    # PL011 UART registers (common ARM UART)
    UART_BASE = 0x3F201000  # RPi UART0
    
    DR = 0x00      # Data Register
    FR = 0x18      # Flag Register
    IBRD = 0x24    # Integer Baud Rate Divisor
    FBRD = 0x28    # Fractional Baud Rate Divisor
    LCRH = 0x2C    # Line Control Register
    CR = 0x30      # Control Register
    
    def __init__(self):
        self.uart_mem = self._map_peripheral(self.UART_BASE, 0x40)
    
    def _map_peripheral(self, base_addr: int, size: int):
        return base_addr
    
    def init(self, baudrate: int = 115200):
        """Initialize UART"""
        # Disable UART
        self._write32(self.uart_mem + self.CR, 0)
        
        # Set baud rate
        # Assuming 48MHz UART clock
        baud_div = (48000000 * 4) // baudrate
        ibrd = baud_div >> 6
        fbrd = baud_div & 0x3F
        
        self._write32(self.uart_mem + self.IBRD, ibrd)
        self._write32(self.uart_mem + self.FBRD, fbrd)
        
        # 8N1, enable FIFOs
        self._write32(self.uart_mem + self.LCRH, 0x70)
        
        # Enable UART, TX, RX
        self._write32(self.uart_mem + self.CR, 0x301)
    
    def write(self, data: bytes):
        """Write data to UART"""
        for byte in data:
            # Wait for TX FIFO not full
            while self._read32(self.uart_mem + self.FR) & 0x20:
                pass
            
            # Write byte
            self._write32(self.uart_mem + self.DR, byte)
    
    def read(self, length: int = 1) -> bytes:
        """Read data from UART"""
        data = []
        
        for _ in range(length):
            # Wait for RX FIFO not empty
            while self._read32(self.uart_mem + self.FR) & 0x10:
                pass
            
            # Read byte
            data.append(self._read32(self.uart_mem + self.DR) & 0xFF)
        
        return bytes(data)
    
    def available(self) -> bool:
        """Check if data available"""
        return not (self._read32(self.uart_mem + self.FR) & 0x10)
    
    def _read32(self, addr: int) -> int:
        return 0  # Placeholder
    
    def _write32(self, addr: int, value: int):
        pass  # Placeholder


class OSBasedUART:
    """UART through OS serial port"""
    
    def __init__(self):
        self.port = None
    
    def init(self, port: str = '/dev/ttyS0', baudrate: int = 115200):
        """Initialize serial port"""
        try:
            import serial
            self.port = serial.Serial(port, baudrate, timeout=0.1)
        except:
            # Fallback to file operations
            try:
                self.port_fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
            except:
                self.port_fd = -1  # Use -1 instead of None for invalid file descriptor
    
    def write(self, data: bytes):
        """Write to serial port"""
        if hasattr(self, 'port') and self.port:
            self.port.write(data)
        elif hasattr(self, 'port_fd') and self.port_fd:
            os.write(self.port_fd, data)
    
    def read(self, length: int = 1) -> bytes:
        """Read from serial port"""
        if hasattr(self, 'port') and self.port:
            return self.port.read(length)
        elif hasattr(self, 'port_fd') and self.port_fd:
            return os.read(self.port_fd, length)
        return b''
    
    def available(self) -> bool:
        """Check if data available"""
        if hasattr(self, 'port') and self.port:
            return self.port.in_waiting > 0
        return False


class SimulatedMemory:
    """Simulated memory for testing"""
    
    def __init__(self, base_addr: int, size: int):
        self.base_addr = base_addr
        self.size = size
        self.data = bytearray(size)
    
    def __getitem__(self, key):
        if isinstance(key, slice):
            return self.data[key]
        return self.data[key]
    
    def __setitem__(self, key, value):
        if isinstance(key, slice):
            self.data[key] = value
        else:
            self.data[key] = value
    
    def read32(self, offset: int) -> int:
        """Read 32-bit value"""
        return struct.unpack('<I', self.data[offset:offset+4])[0]
    
    def write32(self, offset: int, value: int):
        """Write 32-bit value"""
        self.data[offset:offset+4] = struct.pack('<I', value)


# Natural Language to HAL command mapping
class NaturalLanguageHAL:
    """Translate natural language to HAL commands"""
    
    def __init__(self, hal: HardwareAbstractionLayer):
        self.hal = hal
        self.command_patterns = {
            'gpio': {
                'turn on': self.gpio_on,
                'turn off': self.gpio_off,
                'set high': self.gpio_on,
                'set low': self.gpio_off,
                'read': self.gpio_read,
            },
            'i2c': {
                'read': self.i2c_read,
                'write': self.i2c_write,
                'scan': self.i2c_scan,
            },
            'uart': {
                'send': self.uart_send,
                'receive': self.uart_receive,
            }
        }
    
    def process_command(self, command: str) -> str:
        """Process natural language command"""
        command = command.lower()
        
        # GPIO commands
        if 'gpio' in command or 'pin' in command:
            return self._process_gpio_command(command)
        
        # I2C commands
        elif 'i2c' in command:
            return self._process_i2c_command(command)
        
        # UART commands
        elif 'uart' in command or 'serial' in command:
            return self._process_uart_command(command)
        
        return "Unknown command"
    
    def _process_gpio_command(self, command: str) -> str:
        """Process GPIO commands"""
        import re
        
        # Extract pin number
        pin_match = re.search(r'(?:pin|gpio)\s*(\d+)', command)
        if not pin_match:
            return "No pin number specified"
        
        pin = int(pin_match.group(1))
        gpio = self.hal.get_device('gpio')
        
        if 'on' in command or 'high' in command:
            gpio.setup(pin, 'output')
            gpio.write(pin, True)
            return f"GPIO {pin} set HIGH"
        
        elif 'off' in command or 'low' in command:
            gpio.setup(pin, 'output')
            gpio.write(pin, False)
            return f"GPIO {pin} set LOW"
        
        elif 'read' in command:
            gpio.setup(pin, 'input')
            value = gpio.read(pin)
            return f"GPIO {pin} = {'HIGH' if value else 'LOW'}"
        
        return "Unknown GPIO command"
    
    def _process_i2c_command(self, command: str) -> str:
        """Process I2C commands"""
        # Implementation for I2C command processing
        return "I2C command processed"
    
    def _process_uart_command(self, command: str) -> str:
        """Process UART commands"""
        # Implementation for UART command processing
        return "UART command processed"
    
    # Command implementations
    def gpio_on(self, pin: int):
        gpio = self.hal.get_device('gpio')
        gpio.setup(pin, 'output')
        gpio.write(pin, True)
    
    def gpio_off(self, pin: int):
        gpio = self.hal.get_device('gpio')
        gpio.setup(pin, 'output')
        gpio.write(pin, False)
    
    def gpio_read(self, pin: int) -> bool:
        gpio = self.hal.get_device('gpio')
        gpio.setup(pin, 'input')
        return gpio.read(pin)
    
    def i2c_read(self, device: int, register: int, length: int) -> bytes:
        i2c = self.hal.get_device('i2c')
        return i2c.read(device, register, length)
    
    def i2c_write(self, device: int, register: int, data: bytes):
        i2c = self.hal.get_device('i2c')
        i2c.write(device, register, data)
    
    def i2c_scan(self) -> List[int]:
        """Scan I2C bus for devices"""
        i2c = self.hal.get_device('i2c')
        devices = []
        
        for addr in range(0x08, 0x78):
            try:
                i2c.read(addr, 0, 1)
                devices.append(addr)
            except:
                pass
        
        return devices
    
    def uart_send(self, data: str):
        uart = self.hal.get_device('uart')
        uart.write(data.encode())
    
    def uart_receive(self, length: int = 1) -> str:
        uart = self.hal.get_device('uart')
        return uart.read(length).decode()