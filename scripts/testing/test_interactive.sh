#!/bin/bash
# Test EMBODIOS in interactive mode with real model

echo "EMBODIOS Interactive Test"
echo "========================"
echo ""

# Check if virtual environment exists
if [ ! -d "venv" ]; then
    echo "Creating virtual environment..."
    python3 -m venv venv
fi

# Activate virtual environment
source venv/bin/activate

# Install required packages
echo "Installing dependencies..."
pip install -q llama-cpp-python

echo ""
echo "Available test modes:"
echo "1. Real model inference (requires models/tinyllama/)"
echo "2. Simulated mode (no model required)"
echo "3. Hardware test mode"
echo ""

# Check if model exists
MODEL_PATH="models/tinyllama/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
if [ -f "$MODEL_PATH" ]; then
    echo "✓ TinyLlama model found at $MODEL_PATH"
    echo ""
    echo "Starting real model test..."
    python tests/integration/test_real_model_inference.py
else
    echo "✗ Model not found at $MODEL_PATH"
    echo ""
    echo "Starting in simulation mode..."
    python -m embodi.core.runtime_kernel test TinyLlama/TinyLlama-1.1B-Chat-v1.0 --memory 2G --hardware gpio uart i2c
fi