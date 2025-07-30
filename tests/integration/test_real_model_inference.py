#!/usr/bin/env python3
"""Test EMBODIOS with real TinyLlama model inference"""

import os
import sys
import pytest
from pathlib import Path

# Skip if llama-cpp-python not available
llama_cpp = pytest.importorskip("llama_cpp")
from llama_cpp import Llama


class TestRealModelInference:
    """Test real AI model integration with EMBODIOS"""
    
    @pytest.fixture
    def model_path(self):
        """Get path to test model"""
        # Look for model in central location
        model_path = Path("models/tinyllama/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf")
        if not model_path.exists():
            # Try from test directory
            model_path = Path(__file__).parent.parent.parent / "models/tinyllama/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
        
        if not model_path.exists():
            pytest.skip(f"Model not found at {model_path}")
        
        return str(model_path)
    
    def test_model_loading(self, model_path):
        """Test that real model can be loaded"""
        llm = Llama(
            model_path=model_path,
            n_ctx=512,
            n_threads=4,
            verbose=False
        )
        assert llm is not None
        
    def test_simple_inference(self, model_path):
        """Test basic inference with real model"""
        llm = Llama(
            model_path=model_path,
            n_ctx=512,
            n_threads=4,
            verbose=False
        )
        
        # Simple test prompt
        response = llm("Hello, what is your name?", max_tokens=50)
        assert response is not None
        assert 'choices' in response
        assert len(response['choices']) > 0
        assert 'text' in response['choices'][0]
        
    def test_hardware_command_inference(self, model_path):
        """Test hardware-related command inference"""
        llm = Llama(
            model_path=model_path,
            n_ctx=512,
            n_threads=4,
            verbose=False
        )
        
        # Hardware command
        response = llm("Turn on GPIO pin 17", max_tokens=50)
        assert response is not None
        text = response['choices'][0]['text']
        assert len(text) > 0
        
    def test_system_query_inference(self, model_path):
        """Test system status query"""
        llm = Llama(
            model_path=model_path,
            n_ctx=512,
            n_threads=4,
            verbose=False
        )
        
        # System query
        response = llm("Show system status", max_tokens=100)
        assert response is not None
        text = response['choices'][0]['text']
        assert len(text) > 0
        
    def test_calculation_inference(self, model_path):
        """Test mathematical calculation"""
        llm = Llama(
            model_path=model_path,
            n_ctx=512,
            n_threads=4,
            verbose=False
        )
        
        # Math query
        response = llm("What is 15 plus 27?", max_tokens=50)
        assert response is not None
        text = response['choices'][0]['text']
        assert len(text) > 0


def run_interactive_demo():
    """Run interactive demo with real model (for manual testing)"""
    model_path = "models/tinyllama/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
    
    if not os.path.exists(model_path):
        print(f"ERROR: Model not found at {model_path}")
        return
    
    print("=" * 70)
    print("EMBODIOS - Real Model Interactive Demo")
    print("=" * 70)
    print("[BOOT] Loading TinyLlama 1.1B model...")
    
    llm = Llama(
        model_path=model_path,
        n_ctx=2048,
        n_threads=4,
        verbose=False
    )
    
    print("[BOOT] ✓ Model loaded successfully!")
    print("\nType 'exit' to quit")
    print("=" * 70)
    
    while True:
        try:
            user_input = input("\n> ")
            if user_input.lower() == 'exit':
                break
                
            # Generate response
            response = llm(user_input, max_tokens=200)
            ai_text = response['choices'][0]['text'].strip()
            print(f"AI: {ai_text}")
            
        except KeyboardInterrupt:
            break
    
    print("\nShutting down EMBODIOS...")


if __name__ == "__main__":
    # Run interactive demo if called directly
    run_interactive_demo()