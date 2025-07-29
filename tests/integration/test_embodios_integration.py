#!/usr/bin/env python3
"""
EMBODIOS Integration Tests
"""

import os
import sys
from pathlib import Path

# Add src to path
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "src"))

def create_test_model():
    """Create a minimal test model"""
    import struct
    import json
    
    model_path = Path("test-model.aios")
    
    print("Creating test model...")
    
    with open(model_path, 'wb') as f:
        # Header
        f.write(b'AIOS')  # Magic
        f.write(struct.pack('<I', 1))  # Version
        
        # Metadata
        metadata = {
            'architecture': 'test',
            'vocab_size': 1000,
            'hidden_size': 64,
            'num_layers': 2,
            'hardware_tokens': {
                '<GPIO_WRITE>': 900,
                '<GPIO_HIGH>': 901,
                '<GPIO_LOW>': 902,
                '<GPIO_READ>': 903,
                '<I2C_READ>': 910,
                '<I2C_WRITE>': 911,
            }
        }
        
        metadata_json = json.dumps(metadata).encode()
        arch_json = json.dumps({
            'model_type': 'transformer',
            'vocab_size': 1000,
            'hidden_size': 64
        }).encode()
        
        # Calculate weights size based on model architecture
        # Need at least embedding weights: vocab_size * hidden_size * 4 bytes (float32)
        weights_size = metadata['vocab_size'] * metadata['hidden_size'] * 4
        
        # Calculate weights offset (after header + metadata + arch)
        # Header: magic(4) + version(4) + metadata_size(4) + arch_size(4) + weights_offset(4) + weights_size(4) = 24 bytes
        header_size = 24
        weights_offset = header_size + len(metadata_json) + len(arch_json)
        
        # Write sizes (must match what load_model expects)
        f.write(struct.pack('<I', len(metadata_json)))
        f.write(struct.pack('<I', len(arch_json)))
        f.write(struct.pack('<I', weights_offset))  # weights_offset
        f.write(struct.pack('<I', weights_size))  # weights_size
        
        # Write metadata
        f.write(metadata_json)
        f.write(arch_json)
        
        # Write dummy weights (zeros for simplicity)
        f.write(b'\x00' * weights_size)
    
    print(f"Test model created: {model_path}")
    return model_path

def test_natural_language_processor():
    """Test the natural language processor"""
    print("\n" + "="*60)
    print("Testing Natural Language Processor")
    print("="*60)
    
    from embodi.core.nl_processor import NaturalLanguageProcessor
    
    processor = NaturalLanguageProcessor()
    
    test_commands = [
        "Turn on GPIO pin 17",
        "Set pin 23 high",
        "Turn off pin 13",
        "Read gpio 22",
        "Blink pin 5 3 times",
        "Read temperature sensor",
        "Scan I2C bus",
        "Show system status"
    ]
    
    for cmd in test_commands:
        print(f"\nCommand: '{cmd}'")
        commands = processor.process(cmd)
        
        if commands:
            for c in commands:
                print(f"  Parsed: {c.command_type.value} - {c.action}")
                print(f"  Params: {c.parameters}")
        else:
            print("  No command recognized")

def test_hal():
    """Test Hardware Abstraction Layer"""
    print("\n" + "="*60)
    print("Testing Hardware Abstraction Layer")
    print("="*60)
    
    from embodi.core.hal import HardwareAbstractionLayer
    
    hal = HardwareAbstractionLayer()
    hal.initialize()
    
    print("\nRegistered devices:")
    for name in hal.devices:
        print(f"  - {name}")
    
    # Test GPIO
    gpio = hal.get_device('gpio')
    if gpio:
        print("\nTesting GPIO:")
        try:
            gpio.setup(17, 'output')
            gpio.write(17, True)
            print("  GPIO 17 set to HIGH")
            
            gpio.setup(22, 'input')
            value = gpio.read(22)
            print(f"  GPIO 22 reads: {value}")
        except Exception as e:
            print(f"  GPIO test error: {e}")

def test_inference_engine():
    """Test inference engine"""
    print("\n" + "="*60)
    print("Testing Inference Engine")
    print("="*60)
    
    from embodi.core.inference import EMBODIOSInferenceEngine
    
    engine = EMBODIOSInferenceEngine()
    
    print("Hardware tokens:")
    for token, id in list(engine.hardware_tokens.items())[:5]:
        print(f"  {token}: {id}")
    
    # Test with dummy model
    model_path = create_test_model()
    
    try:
        engine.load_model(str(model_path))
        print("\nModel loaded successfully!")
        
        # Test inference with hardware tokens
        test_tokens = [100, 101, 17]  # "turn on 17"
        output_tokens, hw_ops = engine.inference(test_tokens)
        
        print(f"\nInput tokens: {test_tokens}")
        print(f"Output tokens: {output_tokens}")
        print(f"Hardware operations: {hw_ops}")
        
    except Exception as e:
        print(f"Error: {e}")

def test_command_processor():
    """Test the full command processor"""
    print("\n" + "="*60)
    print("Testing Command Processor")
    print("="*60)
    
    from embodi.core.hal import HardwareAbstractionLayer
    from embodi.core.inference import EMBODIOSInferenceEngine
    from embodi.core.nl_processor import EMBODIOSCommandProcessor
    
    # Initialize components
    hal = HardwareAbstractionLayer()
    hal.initialize()
    
    engine = EMBODIOSInferenceEngine()
    
    # Load a test model for the command processor
    model_path = create_test_model()
    engine.load_model(str(model_path))
    
    # Create processor
    processor = EMBODIOSCommandProcessor(hal, engine)
    
    # Test commands that should be recognized by NL processor
    test_commands = [
        "Turn on GPIO pin 17",    # Should match GPIO pattern
        "Read GPIO pin 22",       # Should match GPIO read pattern
        "Show system status",     # Should match system status pattern
        "Turn off GPIO pin 13"    # Should match GPIO pattern
    ]
    
    for cmd in test_commands:
        print(f"\nUser: {cmd}")
        response = processor.process_input(cmd)
        print(f"AI: {response}")

def interactive_demo():
    """Run interactive demo"""
    print("\n" + "="*60)
    print("EMBODIOS Interactive Demo")
    print("="*60)
    print("Type natural language commands to control hardware.")
    print("Examples: 'turn on gpio 17', 'read pin 22', 'status'")
    print("Type 'exit' to quit.\n")
    
    from embodi.core.runtime_kernel import EMBODIOSRunner
    
    # Create test model
    model_path = create_test_model()
    
    # Run kernel
    runner = EMBODIOSRunner()
    runner.run_interactive(str(model_path), {
        'enabled': ['gpio', 'i2c', 'uart'],
        'platform': 'test'
    })

def main():
    """Main test function"""
    print("EMBODIOS System Test")
    print("===================")
    
    if len(sys.argv) > 1:
        if sys.argv[1] == "nl":
            test_natural_language_processor()
        elif sys.argv[1] == "hal":
            test_hal()
        elif sys.argv[1] == "inference":
            test_inference_engine()
        elif sys.argv[1] == "processor":
            test_command_processor()
        elif sys.argv[1] == "interactive":
            interactive_demo()
        else:
            print("Unknown test. Options: nl, hal, inference, processor, interactive")
    else:
        # Run all tests
        test_natural_language_processor()
        test_hal()
        test_inference_engine()
        test_command_processor()
        
        print("\n" + "="*60)
        print("All tests completed!")
        print("Run with 'interactive' argument for interactive demo")

if __name__ == "__main__":
    main()