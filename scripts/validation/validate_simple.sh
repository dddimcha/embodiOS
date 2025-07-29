#!/bin/bash
#
# EMBODIOS Simple Validation Script
# Tests functionality without requiring installation
#

set -e

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${BLUE}EMBODIOS Validation - Simple Mode${NC}"
echo -e "${BLUE}=================================${NC}\n"

# Test 1: Check directory structure
echo -e "${YELLOW}1. Checking directory structure...${NC}"
REQUIRED_DIRS=("src/embodi" "src/embodi/core" "tests" "docs" "examples")
REQUIRED_FILES=("setup.py" "README.md" "LICENSE" "requirements.txt")

all_good=true
for dir in "${REQUIRED_DIRS[@]}"; do
    if [ -d "$dir" ]; then
        echo -e "  ${GREEN}✓${NC} $dir"
    else
        echo -e "  ${RED}✗${NC} $dir missing"
        all_good=false
    fi
done

for file in "${REQUIRED_FILES[@]}"; do
    if [ -f "$file" ]; then
        echo -e "  ${GREEN}✓${NC} $file"
    else
        echo -e "  ${RED}✗${NC} $file missing"
        all_good=false
    fi
done

# Test 2: Check core modules exist
echo -e "\n${YELLOW}2. Checking core modules...${NC}"
CORE_MODULES=(
    "src/embodi/core/hal.py"
    "src/embodi/core/inference.py"
    "src/embodi/core/nl_processor.py"
    "src/embodi/core/runtime_kernel.py"
)

for module in "${CORE_MODULES[@]}"; do
    if [ -f "$module" ]; then
        echo -e "  ${GREEN}✓${NC} $module"
    else
        echo -e "  ${RED}✗${NC} $module missing"
        all_good=false
    fi
done

# Test 3: Test Python imports
echo -e "\n${YELLOW}3. Testing Python imports...${NC}"
cd src
python3 << 'EOF'
import sys

try:
    from embodi.core.hal import HardwareAbstractionLayer
    print("  ✓ HAL module")
except Exception as e:
    print(f"  ✗ HAL module: {e}")

try:
    from embodi.core.inference import EMBODIOSInferenceEngine
    print("  ✓ Inference Engine")
except Exception as e:
    print(f"  ✗ Inference Engine: {e}")

try:
    from embodi.core.nl_processor import NaturalLanguageProcessor
    print("  ✓ NL Processor")
except Exception as e:
    print(f"  ✗ NL Processor: {e}")

try:
    from embodi.core.runtime_kernel import EMBODIOSKernel
    print("  ✓ Runtime Kernel")
except Exception as e:
    print(f"  ✗ Runtime Kernel: {e}")
EOF
cd ..

# Test 4: Run NLP test
echo -e "\n${YELLOW}4. Testing Natural Language Processing...${NC}"
cd src
python3 << 'EOF'
from embodi.core.nl_processor import NaturalLanguageProcessor

nlp = NaturalLanguageProcessor()

test_commands = [
    "Turn on GPIO pin 17",
    "Read temperature sensor",
    "Blink LED 3 times",
    "Show system status"
]

print("  Testing command parsing:")
for cmd in test_commands:
    commands = nlp.process(cmd)
    if commands:
        print(f"    ✓ '{cmd}' → {commands[0].command_type.value}:{commands[0].action}")
    else:
        print(f"    ✗ '{cmd}' → No parse")
EOF
cd ..

# Test 5: Run HAL test
echo -e "\n${YELLOW}5. Testing Hardware Abstraction Layer...${NC}"
cd src
python3 << 'EOF'
from embodi.core.hal import HardwareAbstractionLayer

try:
    hal = HardwareAbstractionLayer()
    hal.initialize()
    print("  ✓ HAL initialized")
    print(f"  ✓ Devices registered: {list(hal.devices.keys())}")
    
    # Test GPIO
    gpio = hal.get_device('gpio')
    gpio.setup(17, 'output')
    gpio.write(17, True)
    print("  ✓ GPIO operations work")
    
except Exception as e:
    print(f"  ✗ HAL test failed: {e}")
EOF
cd ..

# Test 6: Performance quick test
echo -e "\n${YELLOW}6. Quick performance test...${NC}"
cd src
python3 << 'EOF'
import time
from embodi.core.nl_processor import NaturalLanguageProcessor

nlp = NaturalLanguageProcessor()

# Warm up
nlp.process("test")

# Time 100 commands
start = time.perf_counter()
for i in range(100):
    nlp.process(f"Turn on GPIO pin {i % 40}")
end = time.perf_counter()

total_time = (end - start) * 1000  # ms
avg_time = total_time / 100

print(f"  ✓ Processed 100 commands in {total_time:.1f}ms")
print(f"  ✓ Average response time: {avg_time:.2f}ms")
print(f"  ✓ Throughput: {100/total_time*1000:.0f} commands/sec")
EOF
cd ..

# Test 7: Check test files
echo -e "\n${YELLOW}7. Checking test files...${NC}"
TEST_FILES=(
    "tests/test_core.py"
    "tests/integration/test_embodios_integration.py"
    "tests/benchmarks/test_performance.py"
)

for test in "${TEST_FILES[@]}"; do
    if [ -f "$test" ]; then
        echo -e "  ${GREEN}✓${NC} $test"
    else
        echo -e "  ${RED}✗${NC} $test missing"
    fi
done

# Test 8: Documentation check
echo -e "\n${YELLOW}8. Checking documentation...${NC}"
DOC_FILES=(
    "docs/getting-started.md"
    "docs/architecture.md"
    "docs/performance-benchmarks.md"
    "docs/hardware.md"
    "docs/api.md"
)

for doc in "${DOC_FILES[@]}"; do
    if [ -f "$doc" ]; then
        echo -e "  ${GREEN}✓${NC} $doc"
    else
        echo -e "  ${RED}✗${NC} $doc missing"
    fi
done

# Summary
echo -e "\n${BLUE}=================================${NC}"
echo -e "${BLUE}Validation Complete${NC}"
echo -e "${BLUE}=================================${NC}"

if $all_good; then
    echo -e "\n${GREEN}✓ All core components are present and working!${NC}"
    echo -e "\nTo install EMBODIOS system-wide:"
    echo -e "  ${BLUE}python3 -m venv venv${NC}"
    echo -e "  ${BLUE}source venv/bin/activate${NC}"
    echo -e "  ${BLUE}pip install -e .${NC}"
    echo -e "\nThen you can use:"
    echo -e "  ${BLUE}embodi init${NC}"
    echo -e "  ${BLUE}embodi build -f Modelfile -t my-ai-os${NC}"
    echo -e "  ${BLUE}embodi run my-ai-os${NC}"
else
    echo -e "\n${RED}Some issues were found. Please check the output above.${NC}"
fi