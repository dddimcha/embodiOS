# Modelfile Reference

A Modelfile is how you define and configure your EMBODIOS image. Think of it like a Dockerfile, but for building an OS where the kernel is a language model.

## Basic Structure

```dockerfile
FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit
MEMORY 2G
HARDWARE gpio:enabled uart:enabled
```

## Instructions

### FROM

Sets the base image. Currently only `scratch` is supported for bare-metal deployments.

```dockerfile
FROM scratch
```

### MODEL

Specifies which language model to use as the OS kernel. Supports HuggingFace models or local paths.

```dockerfile
# From HuggingFace
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0

# From local file
MODEL file:/path/to/model.gguf

# From EMBODIOS registry (future)
MODEL embodios:assistant-7b
```

### QUANTIZE

Sets the quantization level to reduce model size. Essential for embedded devices.

```dockerfile
QUANTIZE 4bit    # Smallest size, ~4x compression
QUANTIZE 8bit    # Better quality, ~2x compression
QUANTIZE none    # Full precision (not recommended for embedded)
```

### MEMORY

Defines memory allocation for the model and runtime.

```dockerfile
MEMORY 512M      # Minimum for tiny models
MEMORY 2G        # Recommended for most use cases
MEMORY 4G        # For larger models
```

### HARDWARE

Enables specific hardware capabilities. The model will have direct access to these peripherals.

```dockerfile
# Single capability
HARDWARE gpio:enabled

# Multiple capabilities
HARDWARE gpio:enabled uart:enabled i2c:enabled spi:enabled

# With parameters
HARDWARE gpio:enabled:pins=1-26 uart:enabled:baud=115200
```

Available hardware modules:
- `gpio` - General Purpose I/O pins
- `uart` - Serial communication
- `i2c` - I2C bus communication
- `spi` - SPI bus communication
- `pwm` - Pulse Width Modulation
- `adc` - Analog to Digital Converter
- `timer` - Hardware timers
- `wifi` - WiFi networking
- `bluetooth` - Bluetooth connectivity
- `can` - CAN bus (automotive/industrial)

### CAPABILITY

Defines high-level capabilities for the model. These are pre-configured prompts and behaviors.

```dockerfile
CAPABILITY motion_control sensor_fusion
CAPABILITY home_automation voice_control
CAPABILITY industrial_monitoring safety_protocols
```

### ENV

Sets environment variables accessible to the model runtime.

```dockerfile
ENV DEVICE_NAME "smart-sensor-01"
ENV LOCATION "warehouse-floor-2"
ENV DEBUG_MODE "false"
```

### PROMPT

Customizes the system prompt that initializes the model's behavior.

```dockerfile
PROMPT "You are an industrial control system. Monitor temperature sensors and control cooling fans based on thresholds."
```

## Complete Examples

### Minimal IoT Device

```dockerfile
FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit
MEMORY 512M
HARDWARE gpio:enabled
```

### Smart Home Controller

```dockerfile
FROM scratch
MODEL huggingface:microsoft/phi-2
QUANTIZE 4bit
MEMORY 1G
HARDWARE gpio:enabled uart:enabled wifi:enabled
CAPABILITY home_automation
ENV DEVICE_NAME "living-room-controller"
PROMPT "You control smart home devices. Respond to natural language commands for lights, temperature, and security."
```

### Industrial Sensor Node

```dockerfile
FROM scratch
MODEL huggingface:microsoft/Phi-3-mini-4k-instruct
QUANTIZE 8bit
MEMORY 2G
HARDWARE gpio:enabled i2c:enabled adc:enabled
CAPABILITY industrial_monitoring safety_protocols
ENV SAMPLE_RATE "1000"
ENV ALERT_THRESHOLD "85"
PROMPT "Monitor industrial equipment sensors. Alert on anomalies and maintain safety protocols."
```

### Robotics Platform

```dockerfile
FROM scratch
MODEL huggingface:meta-llama/Llama-2-7b-chat-hf
QUANTIZE 4bit
MEMORY 4G
HARDWARE gpio:enabled pwm:enabled i2c:enabled uart:enabled
CAPABILITY motion_control sensor_fusion path_planning
ENV ROBOT_ID "autonomous-bot-01"
PROMPT "You are a robotics control system. Process sensor data, plan paths, and control actuators safely."
```

## Build Process

After creating your Modelfile:

```bash
# Build the image
embodi build -f Modelfile -t my-device:latest

# Create bootable ISO
embodi bundle create --model my-device:latest --output device.iso

# Or create USB installer
embodi bundle create --model my-device:latest --output /dev/sdb --target usb
```

## Best Practices

1. **Start Small**: Begin with tiny models (1-3B parameters) and minimal hardware
2. **Quantize Aggressively**: 4-bit quantization works well for most embedded use cases
3. **Test Locally**: Use QEMU to test before deploying to hardware
4. **Memory Planning**: Account for model size + runtime overhead (usually +20%)
5. **Hardware Access**: Only enable hardware you actually need
6. **Prompt Engineering**: Craft specific prompts for your use case

## Troubleshooting

**Model too large for target device:**
- Use smaller models or more aggressive quantization
- Consider pruning unnecessary model layers

**Hardware not responding:**
- Verify hardware is enabled in Modelfile
- Check pin mappings and voltage levels
- Ensure proper permissions (though EMBODIOS runs privileged)

**Slow inference:**
- Enable CPU optimizations in build
- Use INT8 or INT4 quantization
- Consider a smaller model

**Unexpected behavior:**
- Review and refine your PROMPT instruction
- Add more specific CAPABILITY definitions
- Check ENV variables are being used correctly