# EMBODIOS Bare Metal Deployment Guide

Deploy EMBODIOS directly on physical hardware without a host OS.

## Overview

EMBODIOS can run directly on bare metal, turning any computer into an AI-powered system. This guide covers creating bootable media and deploying EMBODIOS on real hardware.

## Quick Start

```bash
# 1. Pull a model from HuggingFace
embodi pull huggingface:microsoft/phi-2 --quantize 4

# 2. Create bootable ISO
embodi bundle create \
  --model microsoft/phi-2 \
  --output embodi-phi2.iso \
  --target bare-metal \
  --arch x86_64 \
  --features gpio,uart,network

# 3. Write to USB drive
embodi bundle write embodi-phi2.iso /dev/sdb --verify
```

## Supported Hardware

### x86_64 Systems
- **CPU**: Any x86_64 processor (Intel/AMD)
- **RAM**: Minimum 2GB (4GB+ recommended)
- **Storage**: 4GB USB drive or larger
- **Boot**: UEFI or Legacy BIOS

### ARM64 Systems
- **Boards**: Raspberry Pi 4, NVIDIA Jetson
- **RAM**: Minimum 1GB
- **Storage**: 8GB SD card or larger
- **Boot**: UEFI or U-Boot

## Step-by-Step Deployment

### 1. Choose Your Model

Select a model based on your hardware:

```bash
# For embedded/IoT (< 2GB RAM)
embodi pull huggingface:gpt2 --quantize 4

# For desktop (4-8GB RAM)
embodi pull huggingface:microsoft/phi-2 --quantize 4

# For server (16GB+ RAM)
embodi pull huggingface:mistralai/Mistral-7B-Instruct-v0.2 --quantize 4
```

### 2. Select Hardware Profile

Use predefined profiles or customize:

```bash
# Embedded profile
embodi bundle create --model gpt2 --profile embedded ...

# Desktop profile  
embodi bundle create --model phi-2 --profile desktop ...

# Custom features
embodi bundle create --model mistral-7b \
  --features gpio,uart,i2c,spi,network,display \
  ...
```

### 3. Create Bootable Media

#### ISO for CD/DVD or USB
```bash
embodi bundle create \
  --model microsoft/phi-2 \
  --output embodi.iso \
  --target bare-metal
```

#### Direct USB Image
```bash
embodi bundle create \
  --model microsoft/phi-2 \
  --output embodi.img \
  --target bare-metal \
  --format usb
```

### 4. Write to Media

#### Linux
```bash
# Find your USB device
lsblk

# Write ISO to USB (replace sdX with your device)
sudo embodi bundle write embodi.iso /dev/sdX --verify
```

#### macOS
```bash
# Find your USB device
diskutil list

# Write to USB (replace diskN with your device)
sudo embodi bundle write embodi.iso /dev/diskN --verify
```

#### Windows
Use tools like Rufus or Etcher, or use WSL2:
```bash
wsl embodi bundle write embodi.iso /dev/sdb
```

### 5. Boot Configuration

#### UEFI Systems
1. Enter UEFI/BIOS (usually F2, F12, or DEL at boot)
2. Disable Secure Boot
3. Set USB as first boot device
4. Save and exit

#### Legacy BIOS
1. Enter BIOS setup
2. Set USB as first boot device
3. Enable Legacy/CSM mode if needed
4. Save and exit

### 6. First Boot

On successful boot, you'll see:
```
EMBODIOS Bootloader v0.1.0
Loading AI model... [OK]
Initializing hardware... [OK]
Starting AI kernel...

EMBODIOS - Natural Language Operating System
Model: microsoft/phi-2 (4-bit quantized)
Memory: 4GB available
Hardware: GPIO, UART, Network enabled

> _
```

## Hardware-Specific Setup

### Raspberry Pi 4

```bash
# Create SD card image
embodi bundle create \
  --model TinyLlama/TinyLlama-1.1B-Chat-v1.0 \
  --output embodi-rpi4.img \
  --target bare-metal \
  --arch arm64 \
  --board rpi4

# Write to SD card
sudo dd if=embodi-rpi4.img of=/dev/mmcblk0 bs=4M status=progress
```

### NVIDIA Jetson

```bash
embodi bundle create \
  --model microsoft/phi-2 \
  --output embodi-jetson.img \
  --target bare-metal \
  --arch arm64 \
  --board jetson-nano \
  --features gpio,i2c,spi,cuda
```

## Network Boot (PXE)

For multiple machines:

```bash
# Create PXE bundle
embodi bundle create \
  --model microsoft/phi-2 \
  --output embodi-pxe \
  --target pxe \
  --features network

# Setup TFTP server
sudo cp embodi-pxe/* /srv/tftp/
```

## Troubleshooting

### Boot Issues

**Black screen after boot**
- Try adding `--video-mode text` to bundle creation
- Disable UEFI secure boot
- Try legacy BIOS mode

**"No bootable device" error**
- Verify USB is first boot device
- Try different USB ports (USB 2.0 often more compatible)
- Recreate bootable media

**Kernel panic**
- Check minimum RAM requirements
- Try smaller model or more aggressive quantization
- Verify hardware compatibility

### Hardware Detection

Check supported hardware:
```bash
# In EMBODIOS console
> show hardware
AI: Detected hardware:
- CPU: Intel Core i5-8250U (4 cores)
- RAM: 8GB DDR4
- GPIO: Not available (x86)
- Network: Intel Wireless-AC 9260
- Storage: NVMe SSD 256GB
```

### Performance Tuning

```bash
# Create optimized bundle
embodi bundle create \
  --model microsoft/phi-2 \
  --output embodi-optimized.iso \
  --optimize-for latency \
  --memory-reserved 512M \
  --cpu-governor performance
```

## Security Considerations

1. **Secure Boot**: Currently requires disabling Secure Boot
2. **Updates**: Use `embodi bundle update` to rebuild with latest patches
3. **Network**: Enable firewall features if exposing to network
4. **Access Control**: Set up authentication in Modelfile

## Example Deployments

### IoT Gateway
```dockerfile
FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit
MEMORY 1G
HARDWARE mqtt:enabled zigbee:enabled wifi:enabled
CAPABILITY iot_protocols sensor_aggregation
```

### Edge AI Server
```dockerfile
FROM scratch  
MODEL huggingface:mistralai/Mistral-7B-Instruct-v0.2
QUANTIZE 4bit
MEMORY 16G
HARDWARE network:enabled gpu:enabled
CAPABILITY inference_serving load_balancing
```

### Embedded Controller
```dockerfile
FROM scratch
MODEL huggingface:microsoft/phi-1.5
QUANTIZE 4bit  
MEMORY 512M
HARDWARE gpio:enabled pwm:enabled adc:enabled
CAPABILITY realtime_control pid_loops
```

## Next Steps

- Check the [Hardware Documentation](hardware.md) for supported devices
- See [Performance Benchmarks](performance-benchmarks.md) for optimization tips
- Review the [API Documentation](api.md) for custom integrations
- Explore [example Modelfiles](../examples/) for different configurations

Welcome to the future of operating systems - powered by AI, controlled by natural language!