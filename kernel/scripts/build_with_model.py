#!/usr/bin/env python3
"""
EMBODIOS Kernel Builder with Embedded Model
This follows EMBODIOS ideology: AI model is compiled directly into the kernel
"""

import sys
import os
import subprocess
import argparse
from pathlib import Path

# Add parent directory to path to import embodi modules
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'src'))

from embodi.compiler.model_compiler import ModelToNativeCompiler
from embodi.builder.converter import ModelConverter

def build_kernel_with_model(model_path, arch='aarch64', output='embodios_ai.bin'):
    """Build EMBODIOS kernel with embedded AI model"""
    
    print(f"[EMBODIOS] Building kernel with model: {model_path}")
    
    # Step 1: Convert model to EMBODIOS format if needed
    if not model_path.endswith('.emb'):
        print("[EMBODIOS] Converting model to EMBODIOS format...")
        converter = ModelConverter()
        emb_path = Path(model_path).with_suffix('.emb')
        
        if model_path.endswith('.gguf'):
            converter.convert_gguf(Path(model_path), emb_path, quantization=4)
        elif model_path.endswith('.safetensors'):
            converter.convert_safetensors(Path(model_path), emb_path, quantization=4)
        else:
            print(f"[ERROR] Unsupported model format: {model_path}")
            return False
            
        model_path = str(emb_path)
    
    # Step 2: Compile model to assembly
    print("[EMBODIOS] Compiling model to native code...")
    compiler = ModelToNativeCompiler(output_dir='build/model')
    result = compiler.compile_model(model_path, architecture=arch)
    
    asm_file = result.get('asm_files')
    if not asm_file:
        print("[ERROR] Failed to compile model")
        return False
    
    # Step 3: Build kernel with embedded model
    print("[EMBODIOS] Building kernel with embedded model...")
    
    # Change to kernel directory
    kernel_dir = Path(__file__).parent.parent
    os.chdir(kernel_dir)
    
    # Clean build
    subprocess.run(['make', 'clean'], check=True)
    
    # Build with model
    env = os.environ.copy()
    env['MODEL_ASM'] = asm_file
    result = subprocess.run(
        ['make', f'ARCH={arch}', 'MODEL=embedded'],
        env=env,
        capture_output=True,
        text=True
    )
    
    if result.returncode != 0:
        print(f"[ERROR] Kernel build failed:\n{result.stderr}")
        return False
    
    print(f"[SUCCESS] Built EMBODIOS kernel with embedded AI: {output}")
    
    # Step 4: Create bootable image
    print("[EMBODIOS] Creating bootable image...")
    # TODO: Create ISO/USB image with embedded AI kernel
    
    return True

def main():
    parser = argparse.ArgumentParser(description='Build EMBODIOS kernel with embedded AI model')
    parser.add_argument('model', help='Path to AI model (GGUF, SafeTensors, or .emb)')
    parser.add_argument('--arch', default='aarch64', choices=['x86_64', 'aarch64'], 
                       help='Target architecture')
    parser.add_argument('--output', default='embodios_ai.bin', 
                       help='Output kernel binary name')
    
    args = parser.parse_args()
    
    if not Path(args.model).exists():
        print(f"[ERROR] Model not found: {args.model}")
        sys.exit(1)
    
    success = build_kernel_with_model(args.model, args.arch, args.output)
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()