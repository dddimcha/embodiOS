"""
EMBODIOS Natural Language Processor
Translates natural language commands to hardware operations
"""

import re
import json
from typing import Dict, List, Tuple, Optional, Any
from dataclasses import dataclass
from enum import Enum

class CommandType(Enum):
    GPIO = "gpio"
    I2C = "i2c"
    SPI = "spi"
    UART = "uart"
    MEMORY = "memory"
    SYSTEM = "system"
    QUERY = "query"
    UNKNOWN = "unknown"

@dataclass
class HardwareCommand:
    """Represents a hardware command"""
    command_type: CommandType
    action: str
    parameters: Dict[str, Any]
    confidence: float
    
    def to_tokens(self, token_map: Dict[str, int]) -> List[int]:
        """Convert command to token sequence"""
        tokens = []
        
        if self.command_type == CommandType.GPIO:
            if self.action == "write":
                tokens.append(token_map['<GPIO_WRITE>'])
                tokens.append(self.parameters['pin'])
                tokens.append(token_map['<GPIO_HIGH>'] if self.parameters['value'] else token_map['<GPIO_LOW>'])
            elif self.action == "read":
                tokens.append(token_map['<GPIO_READ>'])
                tokens.append(self.parameters['pin'])
                
        elif self.command_type == CommandType.I2C:
            if self.action == "read":
                tokens.append(token_map['<I2C_READ>'])
                tokens.append(self.parameters['device'])
                tokens.append(self.parameters['register'])
            elif self.action == "write":
                tokens.append(token_map['<I2C_WRITE>'])
                tokens.append(self.parameters['device'])
                tokens.append(self.parameters['register'])
                tokens.extend(self.parameters['data'])
                
        return tokens

class NaturalLanguageProcessor:
    """Process natural language commands for hardware control"""
    
    def __init__(self):
        self.patterns = self._init_patterns()
        self.device_aliases = self._init_device_aliases()
        self.context = {}
        
    def _init_patterns(self) -> Dict[CommandType, List[Dict]]:
        """Initialize command patterns"""
        return {
            CommandType.GPIO: [
                {
                    'pattern': r'(?:turn|switch|set)\s+(?:on|high)\s+(?:gpio|pin)\s*(\d+)',
                    'action': 'write',
                    'extract': lambda m: {'pin': int(m.group(1)), 'value': True}
                },
                {
                    'pattern': r'(?:turn|switch|set)\s+(?:off|low)\s+(?:gpio|pin)\s*(\d+)',
                    'action': 'write',
                    'extract': lambda m: {'pin': int(m.group(1)), 'value': False}
                },
                {
                    'pattern': r'(?:gpio|pin)\s*(\d+)\s+(?:on|high)',
                    'action': 'write',
                    'extract': lambda m: {'pin': int(m.group(1)), 'value': True}
                },
                {
                    'pattern': r'(?:gpio|pin)\s*(\d+)\s+(?:off|low)',
                    'action': 'write',
                    'extract': lambda m: {'pin': int(m.group(1)), 'value': False}
                },
                {
                    'pattern': r'(?:read|check|get)\s+(?:gpio|pin)\s*(\d+)',
                    'action': 'read',
                    'extract': lambda m: {'pin': int(m.group(1))}
                },
                {
                    'pattern': r'(?:blink|flash)\s+(?:gpio|pin)\s*(\d+)(?:\s+(\d+)\s*times)?',
                    'action': 'blink',
                    'extract': lambda m: {'pin': int(m.group(1)), 'count': int(m.group(2) or 3)}
                }
            ],
            
            CommandType.I2C: [
                {
                    'pattern': r'(?:read|get)\s+i2c\s+(?:device\s+)?(?:0x)?([0-9a-fA-F]+)(?:\s+register\s+(?:0x)?([0-9a-fA-F]+))?',
                    'action': 'read',
                    'extract': lambda m: {
                        'device': int(m.group(1), 16),
                        'register': int(m.group(2), 16) if m.group(2) else 0
                    }
                },
                {
                    'pattern': r'(?:write|set)\s+i2c\s+(?:device\s+)?(?:0x)?([0-9a-fA-F]+)\s+register\s+(?:0x)?([0-9a-fA-F]+)\s+(?:to\s+)?(?:0x)?([0-9a-fA-F]+)',
                    'action': 'write',
                    'extract': lambda m: {
                        'device': int(m.group(1), 16),
                        'register': int(m.group(2), 16),
                        'data': [int(m.group(3), 16)]
                    }
                },
                {
                    'pattern': r'scan\s+i2c(?:\s+bus)?',
                    'action': 'scan',
                    'extract': lambda m: {}
                }
            ],
            
            CommandType.UART: [
                {
                    'pattern': r'(?:send|write|transmit)\s+["\']([^"\']+)["\']\s+(?:to\s+)?uart',
                    'action': 'send',
                    'extract': lambda m: {'data': m.group(1)}
                },
                {
                    'pattern': r'(?:read|receive|get)\s+(?:from\s+)?uart',
                    'action': 'receive',
                    'extract': lambda m: {'length': 1}
                }
            ],
            
            CommandType.SYSTEM: [
                {
                    'pattern': r'(?:show|display|get)\s+(?:system\s+)?status',
                    'action': 'status',
                    'extract': lambda m: {}
                },
                {
                    'pattern': r'(?:show|list)\s+(?:all\s+)?devices',
                    'action': 'list_devices',
                    'extract': lambda m: {}
                },
                {
                    'pattern': r'(?:show|get)\s+memory\s+(?:usage|info)',
                    'action': 'memory_info',
                    'extract': lambda m: {}
                },
                {
                    'pattern': r'(?:reboot|restart)(?:\s+system)?',
                    'action': 'reboot',
                    'extract': lambda m: {}
                }
            ]
        }
    
    def _init_device_aliases(self) -> Dict[str, Dict]:
        """Initialize device name aliases"""
        return {
            # Common devices
            'led': {'type': 'gpio', 'default_pin': 13},
            'button': {'type': 'gpio', 'default_pin': 2},
            'relay': {'type': 'gpio', 'default_pin': 4},
            
            # I2C devices
            'temperature sensor': {'type': 'i2c', 'address': 0x48},
            'oled display': {'type': 'i2c', 'address': 0x3C},
            'accelerometer': {'type': 'i2c', 'address': 0x68},
            
            # Named pins
            'red led': {'type': 'gpio', 'pin': 17},
            'green led': {'type': 'gpio', 'pin': 27},
            'blue led': {'type': 'gpio', 'pin': 22},
        }
    
    def process(self, input_text: str) -> List[HardwareCommand]:
        """Process natural language input and return hardware commands"""
        
        input_lower = input_text.lower().strip()
        commands = []
        
        # Check for device aliases first
        for alias, info in self.device_aliases.items():
            if alias in input_lower:
                input_lower = self._expand_alias(input_lower, alias, info)
        
        # Try to match patterns
        for cmd_type, patterns in self.patterns.items():
            for pattern_info in patterns:
                match = re.search(pattern_info['pattern'], input_lower)
                if match:
                    try:
                        params = pattern_info['extract'](match)
                        command = HardwareCommand(
                            command_type=cmd_type,
                            action=pattern_info['action'],
                            parameters=params,
                            confidence=0.9
                        )
                        commands.append(command)
                    except:
                        pass
        
        # If no patterns matched, try to understand intent
        if not commands:
            commands = self._infer_intent(input_lower)
        
        # Update context
        self._update_context(commands)
        
        return commands
    
    def _expand_alias(self, text: str, alias: str, info: Dict) -> str:
        """Expand device alias to actual hardware reference"""
        
        if info['type'] == 'gpio':
            pin = info.get('pin', info.get('default_pin', 0))
            return text.replace(alias, f"gpio {pin}")
        elif info['type'] == 'i2c':
            addr = info['address']
            return text.replace(alias, f"i2c device 0x{addr:02x}")
        
        return text
    
    def _infer_intent(self, text: str) -> List[HardwareCommand]:
        """Try to infer intent when no patterns match"""
        
        commands = []
        
        # Check for common words that indicate intent
        if any(word in text for word in ['on', 'activate', 'enable', 'start']):
            # Try to find a number that might be a pin
            numbers = re.findall(r'\d+', text)
            if numbers:
                commands.append(HardwareCommand(
                    command_type=CommandType.GPIO,
                    action='write',
                    parameters={'pin': int(numbers[0]), 'value': True},
                    confidence=0.6
                ))
        
        elif any(word in text for word in ['off', 'deactivate', 'disable', 'stop']):
            numbers = re.findall(r'\d+', text)
            if numbers:
                commands.append(HardwareCommand(
                    command_type=CommandType.GPIO,
                    action='write',
                    parameters={'pin': int(numbers[0]), 'value': False},
                    confidence=0.6
                ))
        
        elif any(word in text for word in ['read', 'check', 'measure', 'get']):
            if 'temperature' in text:
                commands.append(HardwareCommand(
                    command_type=CommandType.I2C,
                    action='read',
                    parameters={'device': 0x48, 'register': 0},
                    confidence=0.7
                ))
        
        return commands
    
    def _update_context(self, commands: List[HardwareCommand]):
        """Update context based on executed commands"""
        
        for cmd in commands:
            if cmd.command_type == CommandType.GPIO:
                self.context[f'gpio_{cmd.parameters["pin"]}'] = cmd.parameters.get('value')
    
    def generate_response(self, commands: List[HardwareCommand], results: Optional[List[Any]] = None) -> str:
        """Generate natural language response for executed commands"""
        
        if not commands:
            return "I didn't understand that command. Try something like 'turn on gpio 17' or 'read temperature sensor'."
        
        responses = []
        
        for i, cmd in enumerate(commands):
            result = results[i] if results and i < len(results) else None
            
            if cmd.command_type == CommandType.GPIO:
                if cmd.action == 'write':
                    pin = cmd.parameters['pin']
                    value = cmd.parameters['value']
                    responses.append(f"GPIO pin {pin} set to {'HIGH' if value else 'LOW'}")
                elif cmd.action == 'read':
                    pin = cmd.parameters['pin']
                    if result is not None:
                        responses.append(f"GPIO pin {pin} is {'HIGH' if result else 'LOW'}")
                    else:
                        responses.append(f"Reading GPIO pin {pin}...")
                        
            elif cmd.command_type == CommandType.I2C:
                if cmd.action == 'read':
                    device = cmd.parameters['device']
                    if result is not None:
                        responses.append(f"I2C device 0x{device:02X} returned: 0x{result:02X}")
                    else:
                        responses.append(f"Reading from I2C device 0x{device:02X}...")
                elif cmd.action == 'scan':
                    if result:
                        devices = ", ".join([f"0x{d:02X}" for d in result])
                        responses.append(f"Found I2C devices at: {devices}")
                    else:
                        responses.append("No I2C devices found")
                        
            elif cmd.command_type == CommandType.SYSTEM:
                if cmd.action == 'status':
                    responses.append("System Status: Running")
                elif cmd.action == 'memory_info':
                    responses.append("Memory: 512MB used / 2GB total")
        
        return " ".join(responses)

class CommandExecutor:
    """Execute hardware commands through HAL"""
    
    def __init__(self, hal):
        self.hal = hal
        self.command_history = []
        
    def execute(self, commands: List[HardwareCommand]) -> List[Any]:
        """Execute a list of hardware commands"""
        
        results = []
        
        for cmd in commands:
            try:
                result = self._execute_single(cmd)
                results.append(result)
                self.command_history.append((cmd, result, True))
            except Exception as e:
                results.append(None)
                self.command_history.append((cmd, str(e), False))
        
        return results
    
    def _execute_single(self, cmd: HardwareCommand) -> Any:
        """Execute a single hardware command"""
        
        if cmd.command_type == CommandType.GPIO:
            gpio = self.hal.get_device('gpio')
            
            if cmd.action == 'write':
                gpio.setup(cmd.parameters['pin'], 'output')
                gpio.write(cmd.parameters['pin'], cmd.parameters['value'])
                return True
                
            elif cmd.action == 'read':
                gpio.setup(cmd.parameters['pin'], 'input')
                return gpio.read(cmd.parameters['pin'])
                
            elif cmd.action == 'blink':
                import time
                pin = cmd.parameters['pin']
                count = cmd.parameters['count']
                
                gpio.setup(pin, 'output')
                for _ in range(count):
                    gpio.write(pin, True)
                    time.sleep(0.5)
                    gpio.write(pin, False)
                    time.sleep(0.5)
                return True
                
        elif cmd.command_type == CommandType.I2C:
            i2c = self.hal.get_device('i2c')
            
            if cmd.action == 'read':
                data = i2c.read(
                    cmd.parameters['device'],
                    cmd.parameters['register'],
                    1
                )
                return data[0] if data else None
                
            elif cmd.action == 'write':
                i2c.write(
                    cmd.parameters['device'],
                    cmd.parameters['register'],
                    bytes(cmd.parameters['data'])
                )
                return True
                
            elif cmd.action == 'scan':
                devices = []
                for addr in range(0x08, 0x78):
                    try:
                        i2c.read(addr, 0, 1)
                        devices.append(addr)
                    except:
                        pass
                return devices
                
        elif cmd.command_type == CommandType.UART:
            uart = self.hal.get_device('uart')
            
            if cmd.action == 'send':
                uart.write(cmd.parameters['data'].encode())
                return True
                
            elif cmd.action == 'receive':
                data = uart.read(cmd.parameters.get('length', 1))
                return data.decode() if data else None
                
        elif cmd.command_type == CommandType.SYSTEM:
            if cmd.action == 'status':
                return {
                    'uptime': 3600,
                    'memory_used': 536870912,
                    'memory_total': 2147483648,
                    'devices': len(self.hal.devices)
                }
                
        return None

# Integration with EMBODIOS
class EMBODIOSCommandProcessor:
    """Main command processor for EMBODIOS"""
    
    def __init__(self, hal, inference_engine):
        self.hal = hal
        self.inference_engine = inference_engine
        self.nl_processor = NaturalLanguageProcessor()
        self.executor = CommandExecutor(hal)
        
    def process_input(self, text: str) -> str:
        """Process natural language input and return response"""
        
        # Parse natural language to commands
        commands = self.nl_processor.process(text)
        
        if not commands:
            # Let the AI model handle it
            try:
                return self._process_with_ai(text)
            except RuntimeError as e:
                if "No model loaded" in str(e):
                    return f"I didn't understand that command. Try something like 'turn on gpio 17' or 'read temperature sensor'."
                else:
                    raise
        
        # Execute hardware commands
        results = self.executor.execute(commands)
        
        # Generate response
        response = self.nl_processor.generate_response(commands, results)
        
        return response
    
    def _process_with_ai(self, text: str) -> str:
        """Process input through AI model when no direct commands found"""
        
        # Convert text to tokens
        tokens = self._text_to_tokens(text)
        
        # Run inference
        output_tokens, hw_operations = self.inference_engine.inference(tokens)
        
        # Execute any hardware operations from AI
        if hw_operations:
            self._execute_ai_operations(hw_operations)
        
        # Convert output tokens back to text
        response = self._tokens_to_text(output_tokens)
        
        return response
    
    def _text_to_tokens(self, text: str) -> List[int]:
        """Convert text to token IDs"""
        # Simplified tokenization
        # Real implementation would use proper tokenizer
        words = text.lower().split()
        tokens = []
        
        # Simple word to token mapping
        word_to_token = {
            'turn': 100, 'on': 101, 'off': 102,
            'gpio': 103, 'pin': 104, 'read': 105,
            'write': 106, 'high': 107, 'low': 108
        }
        
        for word in words:
            if word.isdigit():
                tokens.append(int(word))
            elif word in word_to_token:
                tokens.append(word_to_token[word])
            else:
                tokens.append(hash(word) % 30000)  # Simple hash
        
        return tokens
    
    def _tokens_to_text(self, tokens: List[int]) -> str:
        """Convert tokens back to text"""
        # Simplified detokenization
        # Real implementation would use proper detokenizer
        
        token_to_word = {
            32000: 'gpio_read',
            32001: 'gpio_write',
            32002: 'high',
            32003: 'low'
        }
        
        words = []
        for token in tokens:
            if token in token_to_word:
                words.append(token_to_word[token])
            elif token < 100:
                words.append(str(token))
            else:
                words.append(f"token_{token}")
        
        return " ".join(words)
    
    def _execute_ai_operations(self, operations: Dict):
        """Execute hardware operations from AI model"""
        
        for op_type, ops in operations.items():
            if op_type == 'gpio':
                for op in ops:
                    cmd = HardwareCommand(
                        command_type=CommandType.GPIO,
                        action=op['type'].replace('gpio_', ''),
                        parameters=op,
                        confidence=1.0
                    )
                    self.executor.execute([cmd])


# Example usage
def demo():
    """Demonstrate natural language processing"""
    
    processor = NaturalLanguageProcessor()
    
    test_commands = [
        "Turn on GPIO pin 17",
        "Set pin 23 high",
        "Turn off the LED",
        "Read gpio 22",
        "Blink pin 13 5 times",
        "Read temperature sensor",
        "Scan I2C bus",
        "Send 'Hello' to UART",
        "Show system status"
    ]
    
    print("Natural Language Command Processing Demo")
    print("=" * 50)
    
    for cmd_text in test_commands:
        print(f"\nInput: '{cmd_text}'")
        
        commands = processor.process(cmd_text)
        
        if commands:
            for cmd in commands:
                print(f"  Type: {cmd.command_type.value}")
                print(f"  Action: {cmd.action}")
                print(f"  Parameters: {cmd.parameters}")
                print(f"  Confidence: {cmd.confidence:.2f}")
        else:
            print("  No commands recognized")


if __name__ == '__main__':
    demo()