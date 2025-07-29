# EMBODIOS API Documentation

EMBODIOS doesn't have traditional APIs - instead, you communicate with the system through natural language. However, there are several integration points and programmatic interfaces for building and deploying EMBODIOS systems.

## Natural Language Interface

The primary "API" is natural language text. The model interprets your commands and translates them to hardware actions.

### Basic Commands

```bash
# GPIO Control
> Turn on GPIO pin 17
> Set pin 23 to high
> Read the state of GPIO 22
> Configure pin 18 as input with pull-up

# System Information
> Show system status
> What is the current memory usage?
> List active hardware modules
> Display CPU temperature

# Process Management
> List running tasks
> Stop the temperature monitor
> Start logging sensor data to memory
```

### Advanced Operations

```bash
# Conditional Logic
> When GPIO 22 goes high, turn on GPIO 17
> If temperature exceeds 80C, enable the cooling fan
> Monitor UART and log any errors

# Data Processing
> Read I2C device at address 0x48 every second
> Calculate average of last 10 temperature readings
> Send alert if pressure drops below 900 hPa
```

## Builder API (Python)

For programmatic image building:

```python
from embodi.builder import EmbodiBuilder

# Create builder instance
builder = EmbodiBuilder()

# Build from Modelfile
image_id = builder.build(
    modelfile_path="./Modelfile",
    tag="my-device:latest"
)

# Build programmatically
image_id = builder.build_from_spec({
    "base": "scratch",
    "model": "huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0",
    "quantization": "4bit",
    "memory": "2G",
    "hardware": ["gpio", "uart"],
    "capabilities": ["sensor_monitoring"]
})
```

## Runtime API (Python)

For managing EMBODIOS containers:

```python
from embodi.runtime import EmbodiRuntime

runtime = EmbodiRuntime()

# List images
images = runtime.list_images()
for img in images:
    print(f"{img.repository}:{img.tag} - {img.size_mb}MB")

# Run container
container = runtime.run(
    image="my-device:latest",
    name="sensor-node-01",
    hardware_passthrough=["gpio", "i2c"]
)

# Stop container
runtime.stop(container.container_id)

# Get container logs
logs = runtime.logs(container.container_id)
```

## Hardware Abstraction Layer (HAL)

Low-level hardware access (used internally by the model):

```python
from embodi.core.hal import HardwareAbstractionLayer

hal = HardwareAbstractionLayer()

# GPIO operations
hal.gpio_setup(17, "output")
hal.gpio_write(17, True)
state = hal.gpio_read(22)

# I2C operations
hal.i2c_init(bus=1)
data = hal.i2c_read(address=0x48, register=0x00, length=2)
hal.i2c_write(address=0x48, register=0x01, data=bytes([0x80]))

# UART operations
hal.uart_init(port="/dev/ttyS0", baudrate=115200)
hal.uart_write(b"Hello from EMBODIOS\n")
received = hal.uart_read(timeout=1.0)
```

## Model Format Specification

EMBODIOS uses a custom `.aios` format for models:

```python
# Model metadata structure
{
    "format": "aios",
    "version": "1.0",
    "model": {
        "architecture": "transformer",
        "parameters": 1_100_000_000,
        "quantization": "int4",
        "vocab_size": 32000
    },
    "hardware": {
        "required": ["gpio"],
        "optional": ["uart", "i2c"],
        "memory_min": 536870912  # 512MB in bytes
    },
    "capabilities": ["text_generation", "hardware_control"]
}
```

## CLI Commands

The `embodi` command-line tool:

```bash
# Image management
embodi build -f Modelfile -t my-os:latest
embodi images
embodi rmi my-os:latest

# Container operations
embodi run my-os:latest
embodi ps
embodi stop <container-id>
embodi logs <container-id>

# Model operations
embodi pull huggingface:TinyLlama/TinyLlama-1.1B
embodi convert model.gguf -o model.aios
embodi quantize model.aios -b 4 -o model-4bit.aios

# Bundle creation
embodi bundle create --model my-os:latest --output bootable.iso
embodi bundle write --model my-os:latest --device /dev/sdb
```

## Integration Examples

### Python Script Integration

```python
import subprocess
import json

def embodi_command(text):
    """Send command to EMBODIOS instance"""
    result = subprocess.run(
        ["embodi", "exec", "my-container", text],
        capture_output=True,
        text=True
    )
    return result.stdout

# Control GPIO from Python
response = embodi_command("Turn on GPIO pin 17")
print(response)  # "GPIO Pin 17 -> HIGH"

# Read sensor data
temp = embodi_command("Read temperature from I2C sensor at 0x48")
print(temp)  # "Temperature: 23.5°C"
```

### REST API Wrapper (Community Project)

```python
from embodi_rest import EmbodiAPI

api = EmbodiAPI("http://localhost:8080")

# Send natural language command
response = api.command("Turn on the LED")

# Get system status
status = api.status()
print(status.memory_used, status.model_loaded)

# Stream responses
for chunk in api.stream_command("Monitor all sensors"):
    print(chunk)
```

### MQTT Integration

```python
import paho.mqtt.client as mqtt
from embodi.runtime import EmbodiRuntime

runtime = EmbodiRuntime()
container = runtime.get_container("sensor-node")

def on_message(client, userdata, message):
    command = message.payload.decode()
    response = runtime.exec(container.id, command)
    client.publish("embodios/response", response)

client = mqtt.Client()
client.on_message = on_message
client.connect("localhost", 1883)
client.subscribe("embodios/command")
client.loop_forever()
```

## WebSocket Interface

For real-time communication:

```javascript
const ws = new WebSocket('ws://embodios-device:8080/ws');

ws.onopen = () => {
    // Send command
    ws.send(JSON.stringify({
        type: 'command',
        text: 'Monitor GPIO pin 22'
    }));
};

ws.onmessage = (event) => {
    const response = JSON.parse(event.data);
    console.log('EMBODIOS:', response.text);
    
    if (response.hardware_event) {
        console.log('Hardware event:', response.hardware_event);
    }
};
```

## Security Considerations

EMBODIOS runs with full hardware access. In production:

1. **Isolate Networks**: Run on isolated networks when possible
2. **Validate Input**: Pre-filter commands if exposed to untrusted input
3. **Monitor Commands**: Log all commands for audit trails
4. **Hardware Limits**: Use hardware-based protection where available
5. **Update Models**: Keep models updated with security patches

## Performance Metrics API

```python
from embodi.metrics import MetricsCollector

metrics = MetricsCollector()

# Get inference statistics
stats = metrics.get_inference_stats()
print(f"Average latency: {stats.avg_latency_ms}ms")
print(f"Tokens/second: {stats.tokens_per_second}")

# Hardware utilization
hw_stats = metrics.get_hardware_stats()
print(f"GPIO operations/sec: {hw_stats.gpio_ops_per_sec}")
print(f"I2C bandwidth: {hw_stats.i2c_bandwidth_kbps} kbps")
```

## Extending EMBODIOS

### Custom Hardware Modules

```python
from embodi.core.hal import HardwareModule

class CustomSensor(HardwareModule):
    def __init__(self):
        super().__init__("custom_sensor")
    
    def read_value(self):
        # Your hardware code here
        return 42.0
    
    def get_commands(self):
        return {
            "read custom sensor": self.read_value,
            "calibrate sensor": self.calibrate
        }

# Register with HAL
hal.register_module(CustomSensor())
```

### Model Plugins

```python
from embodi.plugins import ModelPlugin

class WeatherPlugin(ModelPlugin):
    def __init__(self):
        self.commands = {
            "weather": self.get_weather,
            "forecast": self.get_forecast
        }
    
    def process(self, text):
        # Extend model capabilities
        for cmd, func in self.commands.items():
            if cmd in text.lower():
                return func()
        return None
```

## Debugging

Enable debug mode for detailed API traces:

```bash
# Set debug environment variable
export EMBODIOS_DEBUG=true

# Or in Modelfile
ENV DEBUG_MODE "true"
ENV LOG_LEVEL "debug"
```

Debug output includes:
- Natural language parsing steps
- Hardware call sequences
- Memory allocation details
- Inference timing breakdowns