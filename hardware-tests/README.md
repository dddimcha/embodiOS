# EMBODIOS Hardware Tests

Scripts for testing EMBODIOS on various hardware platforms.

## Raspberry Pi 3B

```bash
# Show hardware requirements and recommended models
./rpi3b-test.sh info

# Download recommended model (SmolLM2-135M Q4_K_M, ~80MB)
./rpi3b-test.sh download

# Build ARM64 ISO
./rpi3b-test.sh build

# Flash to SD card
./rpi3b-test.sh flash /dev/sdX
```

### Recommended Model

| Model | Quantization | Size | RAM Usage | Speed |
|-------|--------------|------|-----------|-------|
| SmolLM2-135M | Q4_K_M | 80MB | ~150MB | 2-5 tok/s |
| SmolLM2-135M | Q4_0 | 70MB | ~120MB | 3-6 tok/s |

### Memory Budget (1GB RAM)

```
Kernel + Drivers:    ~50MB
Model Weights:       ~80MB
KV Cache:            ~100MB
Inference Buffers:   ~200MB
─────────────────────────
Total Used:          ~430MB
Available:           ~570MB headroom
```

## Hardware Compatibility

| Device | RAM | Recommended Model | Expected Speed |
|--------|-----|-------------------|----------------|
| RPi 3B | 1GB | SmolLM2-135M Q4_K_M | 2-5 tok/s |
| RPi 4 (2GB) | 2GB | SmolLM2-360M | 5-10 tok/s |
| RPi 4 (4GB) | 4GB | TinyLlama-1.1B | 8-15 tok/s |
| RPi 5 (8GB) | 8GB | Phi-2 / Mistral-7B Q4 | 15-30 tok/s |
| Intel NUC | 8GB+ | Mistral-7B Q4 | 50-100 tok/s |
