# Hardware Compatibility

EMBODIOS runs on a variety of hardware platforms. Since the OS *is* the model, hardware requirements depend mainly on your chosen language model size.

## Supported Architectures

### x86_64
- Intel/AMD processors with AVX2 support (2013+)
- Minimum 2 cores recommended
- Best performance with AVX-512 capable CPUs

### ARM64 (aarch64)
- Raspberry Pi 3/4/5
- NVIDIA Jetson series
- Apple Silicon (M1/M2/M3)
- Qualcomm Snapdragon boards

### RISC-V
- StarFive VisionFive 2
- SiFive development boards
- Other RV64GC compatible processors

## Minimum Requirements

### Tiny Models (1-3B parameters)
- **CPU**: 1GHz+ single core
- **RAM**: 512MB - 1GB
- **Storage**: 2GB
- **Examples**: TinyLlama, Phi-2 (quantized)

### Small Models (3-7B parameters)
- **CPU**: 1.5GHz+ dual core
- **RAM**: 2GB - 4GB
- **Storage**: 8GB
- **Examples**: Llama-2-7B, Mistral-7B (quantized)

### Medium Models (7-13B parameters)
- **CPU**: 2GHz+ quad core
- **RAM**: 8GB - 16GB
- **Storage**: 32GB
- **Examples**: Llama-2-13B (quantized)

## Tested Boards

### Raspberry Pi
| Model | Tested | Performance | Notes |
|-------|---------|------------|-------|
| Pi 3B+ | ✓ | Adequate | Best with tiny models |
| Pi 4 (4GB) | ✓ | Good | Handles 3B models well |
| Pi 4 (8GB) | ✓ | Excellent | Can run 7B quantized |
| Pi 5 | ✓ | Excellent | 2x faster than Pi 4 |
| Pi Zero 2W | ✓ | Limited | Only tiny models |

### NVIDIA Jetson
| Model | Tested | Performance | Notes |
|-------|---------|------------|-------|
| Nano | ✓ | Good | GPU acceleration supported |
| Xavier NX | ✓ | Excellent | Handles 7B models easily |
| Orin Nano | ✓ | Excellent | Best price/performance |
| AGX Orin | ✓ | Outstanding | Can run 13B+ models |

### x86 Mini PCs
| Type | Examples | Performance |
|------|----------|-------------|
| Intel NUC | NUC11/12/13 | Excellent for all model sizes |
| Mini ITX | ASRock DeskMini | Good with modern CPUs |
| Industrial PC | Advantech, Kontron | Varies by model |

## Peripheral Support

### GPIO
- Direct memory-mapped access
- Interrupt support
- PWM on capable pins
- Pull-up/down configuration

### Communication Buses
- **UART**: Up to 4Mbps
- **I2C**: Standard/Fast/Fast+ modes
- **SPI**: Up to 50MHz
- **CAN**: CAN 2.0B support

### Networking
- **Ethernet**: Native support
- **WiFi**: Most common chipsets
- **Bluetooth**: BLE and Classic
- **LoRa**: Via SPI modules

### Storage
- **eMMC**: Recommended for embedded
- **SD Card**: Class 10 or better
- **NVMe**: Best performance
- **USB**: Boot and storage support

## Performance Optimization

### CPU Features
Enable these in your Modelfile for better performance:
```dockerfile
ENV CPU_FEATURES "avx2,fma"  # x86_64
ENV CPU_FEATURES "neon"      # ARM64
```

### Memory Configuration
```dockerfile
# Optimize for available RAM
MEMORY 1G          # Conservative
ENV SWAP_SIZE 2G   # Add swap if needed
```

### Quantization Impact
| Quantization | Model Size | Speed | Quality |
|--------------|------------|-------|---------|
| FP16 | 100% | Baseline | Best |
| INT8 | ~50% | 1.5-2x faster | Good |
| INT4 | ~25% | 2-3x faster | Acceptable |

## Hardware-Specific Builds

### Raspberry Pi Optimized
```dockerfile
FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit
MEMORY 1G
HARDWARE gpio:enabled:bcm i2c:enabled spi:enabled
ENV CPU_FEATURES "neon"
ENV GPIO_MODE "bcm"  # Use BCM pin numbering
```

### Jetson with GPU
```dockerfile
FROM scratch
MODEL huggingface:microsoft/phi-2
QUANTIZE 8bit
MEMORY 4G
HARDWARE gpio:enabled cuda:enabled
ENV USE_GPU "true"
ENV CUDA_CORES "384"  # Jetson Nano
```

### Industrial x86
```dockerfile
FROM scratch
MODEL huggingface:mistralai/Mistral-7B-v0.1
QUANTIZE 4bit
MEMORY 8G
HARDWARE gpio:enabled can:enabled modbus:enabled
ENV CPU_FEATURES "avx2,avx512"
ENV REALTIME_PRIORITY "true"
```

## Troubleshooting

**Boot Issues**
- Verify UEFI/BIOS settings
- Check secure boot is disabled
- Ensure proper boot media creation

**Performance Problems**
- Check thermal throttling
- Verify quantization settings
- Monitor memory usage
- Consider smaller model

**Hardware Not Detected**
- Check device tree (ARM)
- Verify kernel modules included
- Review dmesg output
- Check voltage levels

## Future Hardware

We're working on support for:
- ESP32 series (ultra-low power)
- Google Coral TPU acceleration
- AMD MI series accelerators
- Custom FPGA implementations
- Neuromorphic chips

## Contributing

Got EMBODIOS running on new hardware? Let us know! Submit a PR with:
- Board specifications
- Modelfile used
- Performance metrics
- Any special configuration