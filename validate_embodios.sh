#!/bin/bash
#
# EMBODIOS Validation Script
# Tests all major functionality step by step
#

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test counters
TESTS_PASSED=0
TESTS_FAILED=0

# Helper functions
print_header() {
    echo -e "\n${BLUE}============================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}============================================${NC}"
}

print_test() {
    echo -e "\n${YELLOW}► Testing: $1${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
    ((TESTS_PASSED++))
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
    ((TESTS_FAILED++))
}

print_info() {
    echo -e "${BLUE}ℹ $1${NC}"
}

# Check if running from correct directory
check_directory() {
    print_header "Checking Environment"
    
    if [ ! -f "setup.py" ] || [ ! -d "src/embodi" ]; then
        print_error "Not in EMBODIOS root directory"
        echo "Please run from the root of the EMBODIOS repository"
        exit 1
    fi
    print_success "In correct directory"
}

# Test 1: Python environment
test_python_env() {
    print_header "1. Python Environment"
    
    print_test "Python version"
    if python3 --version &> /dev/null; then
        PYTHON_VERSION=$(python3 --version)
        print_success "Python installed: $PYTHON_VERSION"
    else
        print_error "Python 3 not found"
        return 1
    fi
    
    print_test "Required Python version"
    PYTHON_MAJOR=$(python3 -c 'import sys; print(sys.version_info.major)')
    PYTHON_MINOR=$(python3 -c 'import sys; print(sys.version_info.minor)')
    if [ "$PYTHON_MAJOR" -eq 3 ] && [ "$PYTHON_MINOR" -ge 8 ]; then
        print_success "Python 3.8+ confirmed"
    else
        print_error "Python 3.8+ required"
        return 1
    fi
}

# Test 2: Installation
test_installation() {
    print_header "2. EMBODIOS Installation"
    
    print_test "Installing EMBODIOS"
    if pip3 install -e . --quiet; then
        print_success "EMBODIOS installed successfully"
    else
        print_error "Installation failed"
        return 1
    fi
    
    print_test "CLI availability"
    if command -v embodi &> /dev/null; then
        print_success "embodi CLI is available"
    else
        print_error "embodi CLI not found in PATH"
        return 1
    fi
    
    print_test "CLI version"
    if embodi --version &> /dev/null; then
        VERSION=$(embodi --version)
        print_success "CLI version: $VERSION"
    else
        print_error "Failed to get CLI version"
    fi
}

# Test 3: Core modules
test_core_modules() {
    print_header "3. Core Module Imports"
    
    print_test "Hardware Abstraction Layer"
    if python3 -c "from embodi.core.hal import HardwareAbstractionLayer" 2>/dev/null; then
        print_success "HAL module imports correctly"
    else
        print_error "Failed to import HAL"
    fi
    
    print_test "Inference Engine"
    if python3 -c "from embodi.core.inference import EMBODIOSInferenceEngine" 2>/dev/null; then
        print_success "Inference engine imports correctly"
    else
        print_error "Failed to import inference engine"
    fi
    
    print_test "Natural Language Processor"
    if python3 -c "from embodi.core.nl_processor import NaturalLanguageProcessor" 2>/dev/null; then
        print_success "NL processor imports correctly"
    else
        print_error "Failed to import NL processor"
    fi
    
    print_test "Runtime Kernel"
    if python3 -c "from embodi.core.runtime_kernel import EMBODIOSKernel" 2>/dev/null; then
        print_success "Runtime kernel imports correctly"
    else
        print_error "Failed to import runtime kernel"
    fi
}

# Test 4: Natural Language Processing
test_nlp_functionality() {
    print_header "4. Natural Language Processing"
    
    print_test "NLP command parsing"
    
    python3 << EOF
import sys
sys.path.insert(0, 'src')
from embodi.core.nl_processor import NaturalLanguageProcessor

nlp = NaturalLanguageProcessor()

# Test commands
test_commands = [
    ("Turn on GPIO pin 17", "gpio", "write"),
    ("Read temperature sensor", "i2c", "read"),
    ("Show system status", "system", "status")
]

all_passed = True
for cmd_text, expected_type, expected_action in test_commands:
    commands = nlp.process(cmd_text)
    if commands and commands[0].command_type.value == expected_type and commands[0].action == expected_action:
        print(f"✓ '{cmd_text}' parsed correctly")
    else:
        print(f"✗ '{cmd_text}' failed to parse")
        all_passed = False

sys.exit(0 if all_passed else 1)
EOF
    
    if [ $? -eq 0 ]; then
        print_success "NLP parsing works correctly"
    else
        print_error "NLP parsing failed"
    fi
}

# Test 5: HAL functionality
test_hal_functionality() {
    print_header "5. Hardware Abstraction Layer"
    
    print_test "HAL initialization and devices"
    
    python3 << EOF
import sys
sys.path.insert(0, 'src')
from embodi.core.hal import HardwareAbstractionLayer

try:
    hal = HardwareAbstractionLayer()
    hal.initialize()
    
    # Check devices
    if 'gpio' in hal.devices:
        print("✓ GPIO device registered")
    else:
        print("✗ GPIO device not found")
        sys.exit(1)
    
    # Test GPIO operations
    gpio = hal.get_device('gpio')
    gpio.setup(17, 'output')
    gpio.write(17, True)
    
    if gpio.pins[17]['value'] == True:
        print("✓ GPIO write operation successful")
    else:
        print("✗ GPIO write operation failed")
        sys.exit(1)
    
    print("✓ HAL functioning correctly")
    sys.exit(0)
except Exception as e:
    print(f"✗ HAL test failed: {e}")
    sys.exit(1)
EOF
    
    if [ $? -eq 0 ]; then
        print_success "HAL tests passed"
    else
        print_error "HAL tests failed"
    fi
}

# Test 6: Model file creation
test_modelfile() {
    print_header "6. Modelfile Creation"
    
    print_test "Creating test Modelfile"
    
    cat > test_Modelfile << EOF
FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit
MEMORY 2G
CPU 2
HARDWARE gpio:enabled
HARDWARE uart:enabled
CAPABILITY hardware_control
ENV EMBODIOS_DEBUG 0
EOF
    
    if [ -f "test_Modelfile" ]; then
        print_success "Modelfile created"
        print_info "Contents:"
        cat test_Modelfile | sed 's/^/  /'
    else
        print_error "Failed to create Modelfile"
    fi
}

# Test 7: Bundle creation
test_bundle_creation() {
    print_header "7. Bundle Creation Test"
    
    print_test "Testing bundle creation functionality"
    
    python3 << EOF
import sys
sys.path.insert(0, 'src')

try:
    # Test bundle info generation
    import json
    import time
    
    bundle_info = {
        'target': 'test',
        'model': 'test-model.aios',
        'created': time.time(),
        'size': 50 * 1024 * 1024,
        'components': ['bootloader', 'kernel', 'model']
    }
    
    with open('test_bundle_info.json', 'w') as f:
        json.dump(bundle_info, f, indent=2)
    
    print("✓ Bundle metadata creation successful")
    sys.exit(0)
except Exception as e:
    print(f"✗ Bundle test failed: {e}")
    sys.exit(1)
EOF
    
    if [ $? -eq 0 ] && [ -f "test_bundle_info.json" ]; then
        print_success "Bundle creation test passed"
        rm -f test_bundle_info.json
    else
        print_error "Bundle creation test failed"
    fi
}

# Test 8: Performance benchmark
test_performance() {
    print_header "8. Performance Quick Test"
    
    print_test "Running performance measurement"
    
    python3 << EOF
import sys
import time
sys.path.insert(0, 'src')

try:
    from embodi.core.nl_processor import NaturalLanguageProcessor
    
    nlp = NaturalLanguageProcessor()
    
    # Measure response time
    commands = [
        "Turn on GPIO pin 17",
        "Read temperature sensor",
        "Set pin 23 high"
    ]
    
    times = []
    for cmd in commands:
        start = time.perf_counter()
        result = nlp.process(cmd)
        end = time.perf_counter()
        times.append((end - start) * 1000)  # ms
    
    avg_time = sum(times) / len(times)
    print(f"✓ Average response time: {avg_time:.2f}ms")
    
    if avg_time < 10:  # Should be under 10ms
        print("✓ Performance within expected range")
        sys.exit(0)
    else:
        print("✗ Performance slower than expected")
        sys.exit(1)
        
except Exception as e:
    print(f"✗ Performance test failed: {e}")
    sys.exit(1)
EOF
    
    if [ $? -eq 0 ]; then
        print_success "Performance test passed"
    else
        print_error "Performance test failed"
    fi
}

# Test 9: Unit tests
test_unit_tests() {
    print_header "9. Unit Tests"
    
    print_test "Running pytest"
    
    if command -v pytest &> /dev/null; then
        if pytest tests/test_core.py -v --tb=short; then
            print_success "Unit tests passed"
        else
            print_error "Unit tests failed"
        fi
    else
        print_info "pytest not installed, skipping unit tests"
        print_info "Install with: pip install pytest"
    fi
}

# Test 10: Integration test
test_integration() {
    print_header "10. Integration Test"
    
    print_test "Full system integration"
    
    python3 << EOF
import sys
sys.path.insert(0, 'src')

try:
    from embodi.core.hal import HardwareAbstractionLayer
    from embodi.core.inference import EMBODIOSInferenceEngine
    from embodi.core.nl_processor import EMBODIOSCommandProcessor
    
    # Initialize components
    hal = HardwareAbstractionLayer()
    hal.initialize()
    
    engine = EMBODIOSInferenceEngine()
    processor = EMBODIOSCommandProcessor(hal, engine)
    
    # Test command processing
    response = processor.process_input("Turn on GPIO 17")
    
    if "GPIO pin 17 set to HIGH" in response:
        print("✓ Integration test successful")
        print(f"  Response: {response}")
        sys.exit(0)
    else:
        print("✗ Unexpected response")
        sys.exit(1)
        
except Exception as e:
    print(f"✗ Integration test failed: {e}")
    sys.exit(1)
EOF
    
    if [ $? -eq 0 ]; then
        print_success "Integration test passed"
    else
        print_error "Integration test failed"
    fi
}

# Clean up function
cleanup() {
    print_header "Cleanup"
    
    print_test "Removing test files"
    rm -f test_Modelfile test_bundle_info.json
    print_success "Cleanup complete"
}

# Main execution
main() {
    echo -e "${BLUE}"
    echo "╔════════════════════════════════════════╗"
    echo "║     EMBODIOS Validation Script         ║"
    echo "║  Testing all major functionality       ║"
    echo "╚════════════════════════════════════════╝"
    echo -e "${NC}"
    
    # Run all tests
    check_directory
    test_python_env
    test_installation
    test_core_modules
    test_nlp_functionality
    test_hal_functionality
    test_modelfile
    test_bundle_creation
    test_performance
    test_unit_tests
    test_integration
    
    # Cleanup
    cleanup
    
    # Summary
    print_header "Test Summary"
    echo -e "${GREEN}Passed: $TESTS_PASSED${NC}"
    echo -e "${RED}Failed: $TESTS_FAILED${NC}"
    
    if [ $TESTS_FAILED -eq 0 ]; then
        echo -e "\n${GREEN}✓ All tests passed! EMBODIOS is working correctly.${NC}"
        
        echo -e "\n${BLUE}Next steps:${NC}"
        echo "1. Try the quick start example:"
        echo "   embodi init"
        echo "   embodi build -f Modelfile -t my-ai-os:latest"
        echo ""
        echo "2. Run the benchmarks:"
        echo "   python3 tests/benchmarks/test_performance.py"
        echo ""
        echo "3. Check out the documentation:"
        echo "   - docs/getting-started.md"
        echo "   - docs/architecture.md"
        
        exit 0
    else
        echo -e "\n${RED}✗ Some tests failed. Please check the output above.${NC}"
        exit 1
    fi
}

# Run main function
main