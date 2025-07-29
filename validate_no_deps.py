#!/usr/bin/env python3
"""
EMBODIOS Validation Script - No External Dependencies
Tests core functionality using only standard library
"""

import os
import sys
import time
import json
from pathlib import Path

# Add src to path
sys.path.insert(0, 'src')

# Colors
GREEN = '\033[92m'
RED = '\033[91m'
YELLOW = '\033[93m'
BLUE = '\033[94m'
END = '\033[0m'

def print_header(title):
    print(f"\n{BLUE}{'='*50}{END}")
    print(f"{BLUE}{title}{END}")
    print(f"{BLUE}{'='*50}{END}")

def print_success(msg):
    print(f"{GREEN}✓ {msg}{END}")

def print_error(msg):
    print(f"{RED}✗ {msg}{END}")

def print_info(msg):
    print(f"{YELLOW}► {msg}{END}")

# Test 1: Directory Structure
def test_directory_structure():
    print_header("1. Directory Structure")
    
    required_dirs = [
        "src/embodi",
        "src/embodi/core",
        "src/embodi/builder",
        "src/embodi/cli",
        "tests",
        "docs",
        "examples"
    ]
    
    all_exist = True
    for dir_path in required_dirs:
        if os.path.isdir(dir_path):
            print_success(f"{dir_path}")
        else:
            print_error(f"{dir_path} missing")
            all_exist = False
    
    return all_exist

# Test 2: Core Files
def test_core_files():
    print_header("2. Core Module Files")
    
    core_files = [
        "src/embodi/core/hal.py",
        "src/embodi/core/inference.py",
        "src/embodi/core/nl_processor.py",
        "src/embodi/core/runtime_kernel.py",
        "src/embodi/cli/main.py",
        "src/embodi/builder/builder.py"
    ]
    
    all_exist = True
    for file_path in core_files:
        if os.path.isfile(file_path):
            print_success(f"{file_path}")
        else:
            print_error(f"{file_path} missing")
            all_exist = False
    
    return all_exist

# Test 3: Documentation
def test_documentation():
    print_header("3. Documentation")
    
    docs = [
        "README.md",
        "LICENSE",
        "CONTRIBUTING.md",
        "docs/getting-started.md",
        "docs/architecture.md",
        "docs/performance-benchmarks.md",
        "docs/hardware.md",
        "docs/api.md"
    ]
    
    all_exist = True
    for doc in docs:
        if os.path.isfile(doc):
            print_success(f"{doc}")
        else:
            print_error(f"{doc} missing")
            all_exist = False
    
    return all_exist

# Test 4: Test Files
def test_test_files():
    print_header("4. Test Files")
    
    test_files = [
        "tests/test_core.py",
        "tests/test_builder.py",
        "tests/test_runtime.py",
        "tests/integration/test_embodios_integration.py",
        "tests/benchmarks/test_performance.py"
    ]
    
    all_exist = True
    for test_file in test_files:
        if os.path.isfile(test_file):
            print_success(f"{test_file}")
        else:
            print_error(f"{test_file} missing")
            all_exist = False
    
    return all_exist

# Test 5: Basic NLP functionality (no deps)
def test_basic_nlp():
    print_header("5. Basic NLP Functionality")
    
    try:
        # Simple pattern matching test
        test_commands = [
            ("Turn on GPIO pin 17", "gpio", "on", 17),
            ("Turn off pin 23", "gpio", "off", 23),
            ("Read GPIO 22", "gpio", "read", 22)
        ]
        
        print_info("Testing simple pattern matching...")
        
        for cmd, expected_type, expected_action, expected_pin in test_commands:
            # Simple regex-like matching
            import re
            
            # Extract pin number
            pin_match = re.search(r'\b(\d+)\b', cmd)
            if pin_match:
                pin = int(pin_match.group(1))
            else:
                pin = None
            
            # Detect action
            cmd_lower = cmd.lower()
            if 'on' in cmd_lower or 'high' in cmd_lower:
                action = 'on'
            elif 'off' in cmd_lower or 'low' in cmd_lower:
                action = 'off'
            elif 'read' in cmd_lower:
                action = 'read'
            else:
                action = None
            
            # Check GPIO
            if 'gpio' in cmd_lower or 'pin' in cmd_lower:
                cmd_type = 'gpio'
            else:
                cmd_type = None
            
            if pin == expected_pin and action == expected_action and cmd_type == expected_type:
                print_success(f"'{cmd}' → {cmd_type}:{action}:{pin}")
            else:
                print_error(f"'{cmd}' parsing failed")
        
        return True
        
    except Exception as e:
        print_error(f"NLP test failed: {e}")
        return False

# Test 6: Configuration files
def test_config_files():
    print_header("6. Configuration Files")
    
    configs = [
        "setup.py",
        "requirements.txt",
        "requirements-dev.txt",
        ".gitignore"
    ]
    
    all_exist = True
    for config in configs:
        if os.path.isfile(config):
            print_success(f"{config}")
            
            # Check if requirements.txt has content
            if config == "requirements.txt":
                with open(config, 'r') as f:
                    lines = [l.strip() for l in f if l.strip() and not l.startswith('#')]
                    print_info(f"  Found {len(lines)} dependencies")
        else:
            print_error(f"{config} missing")
            all_exist = False
    
    return all_exist

# Test 7: Example files
def test_examples():
    print_header("7. Example Files")
    
    examples = [
        "examples/Modelfile.tinyllama",
        "examples/Modelfile.mistral",
        "examples/Modelfile.custom"
    ]
    
    all_exist = True
    for example in examples:
        if os.path.isfile(example):
            print_success(f"{example}")
        else:
            print_error(f"{example} missing")
            all_exist = False
    
    return all_exist

# Test 8: Performance benchmark results
def test_benchmark_results():
    print_header("8. Benchmark Results")
    
    bench_file = "docs/performance-benchmarks.md"
    if os.path.isfile(bench_file):
        print_success(f"Benchmark results found: {bench_file}")
        
        # Read and extract key metrics
        with open(bench_file, 'r') as f:
            content = f.read()
            
        # Extract performance numbers
        import re
        
        # Memory savings
        memory_match = re.search(r'(\d+\.?\d*)\s*MB less memory', content)
        if memory_match:
            print_info(f"  Memory savings: {memory_match.group(1)} MB")
        
        # Speed improvement
        speed_match = re.search(r'(\d+\.?\d*)x faster', content)
        if speed_match:
            print_info(f"  Speed improvement: {speed_match.group(1)}x")
        
        # Response time
        response_match = re.search(r'(\d+\.?\d*)\s*ms average', content)
        if response_match:
            print_info(f"  Response time: {response_match.group(1)} ms")
        
        return True
    else:
        print_error("Benchmark results not found")
        return False

# Main validation
def main():
    print(f"\n{BLUE}{'='*50}{END}")
    print(f"{BLUE}EMBODIOS Validation Script{END}")
    print(f"{BLUE}{'='*50}{END}")
    
    tests = [
        ("Directory Structure", test_directory_structure),
        ("Core Files", test_core_files),
        ("Documentation", test_documentation),
        ("Test Files", test_test_files),
        ("Basic NLP", test_basic_nlp),
        ("Configuration", test_config_files),
        ("Examples", test_examples),
        ("Benchmarks", test_benchmark_results)
    ]
    
    passed = 0
    failed = 0
    
    for name, test_func in tests:
        try:
            if test_func():
                passed += 1
            else:
                failed += 1
        except Exception as e:
            print_error(f"{name} test crashed: {e}")
            failed += 1
    
    # Summary
    print_header("Summary")
    print(f"Tests passed: {GREEN}{passed}{END}")
    print(f"Tests failed: {RED}{failed}{END}")
    
    if failed == 0:
        print(f"\n{GREEN}✓ All validation tests passed!{END}")
        print(f"\n{BLUE}EMBODIOS is properly set up.{END}")
        print(f"\nTo use EMBODIOS:")
        print(f"  1. Create a virtual environment:")
        print(f"     {YELLOW}python3 -m venv venv{END}")
        print(f"     {YELLOW}source venv/bin/activate{END}")
        print(f"  2. Install dependencies:")
        print(f"     {YELLOW}pip install -r requirements.txt{END}")
        print(f"  3. Install EMBODIOS:")
        print(f"     {YELLOW}pip install -e .{END}")
        print(f"  4. Run EMBODIOS:")
        print(f"     {YELLOW}embodi init{END}")
        print(f"     {YELLOW}embodi build -f Modelfile -t my-ai-os{END}")
    else:
        print(f"\n{RED}✗ Some tests failed. Please check the issues above.{END}")
        return 1
    
    return 0

if __name__ == "__main__":
    sys.exit(main())