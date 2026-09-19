# EMBODIOS Model Repository

This directory contains AI models for EMBODIOS. Models are downloaded on-demand and not stored in git.

## Quick Start

Download the default model (SmolLM-135M-Instruct Q4_K_M, ~100 MB):
```bash
./scripts/download-models.sh            # or: ./embodi pull smollm
```

Download a specific model:
```bash
./scripts/download-models.sh tinyllama  # or: ./embodi pull tinyllama
```

Downloads are verified against the size + SHA256 recorded in `manifest.json`.
hf-mirror.com is tried first, huggingface.co is the fallback.

## Model Versioning

All models are tracked in `manifest.json` with:
- Exact version and revision
- SHA256 checksums for integrity
- Download URLs
- File sizes

This ensures reproducible testing across different environments.

## Using EMBODIOS CLI

```bash
embodi pull smollm      # default model
embodi pull tinyllama   # optional, ~669 MB
embodi pull all         # everything
```

## Available Models

| Model | Version | Size | License | Status |
|-------|---------|------|---------|--------|
| SmolLM-135M-Instruct | Q4_K_M | 100 MB | Apache-2.0 | Verified in QEMU (default) |
| TinyLlama-1.1B-Chat | v1.0-Q4_K_M | 669 MB | Apache-2.0 | Runtime-supported |

## Directory Structure

```
models/
├── manifest.json                         # Model registry with versions
├── README.md                             # This file
├── .gitkeep                              # Ensures directory exists in git
└── smollm-135m-instruct-q4_k_m.gguf      # Downloaded (git-ignored)
```

The kernel Makefile embeds `models/smollm-135m-instruct-q4_k_m.gguf` into
the kernel image automatically when the file is present.

## Adding New Models

1. Update `manifest.json` with model details:
   - Version and revision
   - Download URL (plus hf-mirror.com mirror)
   - SHA256 checksum
   - File size

2. Add an entry to `scripts/download-models.sh` and run it:
   ```bash
   ./scripts/download-models.sh your-model
   ```

## Important Notes

- Models are **NOT** stored in git (see `.gitignore`)
- Always use the download script to ensure correct versions
- The manifest ensures everyone uses the same model versions
- Models are cached locally after first download
