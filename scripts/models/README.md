# EMBODIOS Model Repository

This directory contains AI models for EMBODIOS. Models are downloaded on-demand and not stored in git.

## Quick Start

Download the default test model (TinyLlama):
```bash
./scripts/download-models.sh
```

Download a specific model:
```bash
./scripts/download-models.sh phi-2
```

## Model Versioning

All models are tracked in `manifest.json` with:
- Exact version and revision
- SHA256 checksums for integrity
- Download URLs
- File sizes

This ensures reproducible testing across different environments.

## Using EMBODIOS CLI

Pull models using the CLI:
```bash
# Pull from HuggingFace
embodi pull TinyLlama/TinyLlama-1.1B-Chat-v1.0

# Pull with quantization
embodi pull microsoft/phi-2 --quantize 4

# Pull direct URL
embodi pull https://huggingface.co/.../model.gguf
```

## Available Models

| Model | Version | Size | License |
|-------|---------|------|---------|
| TinyLlama | v1.0-Q4_K_M | 638MB | Apache-2.0 |
| Phi-2 | 2.0-Q4_K_M | 1.5GB | MIT |

## Directory Structure

```
models/
├── manifest.json      # Model registry with versions
├── README.md         # This file
├── .gitkeep          # Ensures directory exists in git
└── tinyllama/        # Downloaded models (git-ignored)
    ├── tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf
    ├── .version      # Version tracking
    └── .revision     # Git revision tracking
```

## Adding New Models

1. Update `manifest.json` with model details:
   - Version and revision
   - Download URL
   - SHA256 checksum
   - File size

2. Run download script:
   ```bash
   ./scripts/download-models.sh your-model
   ```

## Testing with Models

Run tests that use real models:
```bash
# Download model first
./scripts/download-models.sh

# Run tests
pytest tests/integration/test_real_model_inference.py
```

Run interactive demo:
```bash
./scripts/testing/test_interactive.sh
```

## Important Notes

- Models are **NOT** stored in git (see `.gitignore`)
- Always use the download script to ensure correct versions
- The manifest ensures everyone uses the same model versions
- Models are cached locally after first download
