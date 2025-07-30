"""
Tests for EMBODIOS transpiler
"""

import unittest
from pathlib import Path

from embodi.compiler.embodios_transpiler import EMBODIOSTranspiler

class TestEMBODIOSTranspiler(unittest.TestCase):
    def setUp(self):
        self.transpiler = EMBODIOSTranspiler()
        
    def test_hardware_token_transpilation(self):
        """Test hardware token to C conversion"""
        tokens = {
            "<GPIO_READ>": 32000,
            "<GPIO_WRITE>": 32001,
            "<I2C_READ>": 32020,
        }
        
        result = self.transpiler.transpile_hardware_tokens(tokens)
        
        self.assertIn("#define GPIO_READ_TOKEN 32000", result)
        self.assertIn("#define GPIO_WRITE_TOKEN 32001", result)
        self.assertIn("#define I2C_READ_TOKEN 32020", result)
        self.assertIn("IS_GPIO_TOKEN", result)
        
    def test_hal_operations_generation(self):
        """Test HAL operations C generation"""
        result = self.transpiler.transpile_hal_operations("")
        
        self.assertIn("hal_gpio.c", result)
        self.assertIn("hal_i2c.c", result)
        self.assertIn("hal_uart.c", result)
        self.assertIn("hal.h", result)
        
        # Check GPIO implementation
        gpio_c = result["hal_gpio.c"]
        self.assertIn("gpio_init", gpio_c)
        self.assertIn("gpio_write", gpio_c)
        self.assertIn("BCM2835", gpio_c)
        
    def test_command_processor_generation(self):
        """Test command processor C generation"""
        result = self.transpiler.transpile_command_processor("")
        
        self.assertIn("process_command", result)
        self.assertIn("command_pattern_t", result)
        self.assertIn("turn on gpio", result)
        self.assertIn("read i2c device", result)
        
    def test_runtime_kernel_generation(self):
        """Test runtime kernel C generation"""
        result = self.transpiler.transpile_runtime_kernel("")
        
        self.assertIn("kernel_init", result)
        self.assertIn("kernel_main_loop", result)
        self.assertIn("kernel_shutdown", result)
        self.assertIn("process_command", result)
        self.assertIn("model_inference", result)
        
    def test_minimal_boot_generation(self):
        """Test boot code generation"""
        result = self.transpiler.transpile_minimal_boot()
        
        self.assertIn(".global _start", result)
        self.assertIn("_start:", result)
        self.assertIn("kernel_init", result)
        self.assertIn("kernel_main_loop", result)
        
    def test_gpio_c_correctness(self):
        """Test GPIO C code correctness"""
        gpio_c = self.transpiler._generate_gpio_c()
        
        # Check register definitions
        self.assertIn("GPFSEL0", gpio_c)
        self.assertIn("GPSET0", gpio_c)
        self.assertIn("GPCLR0", gpio_c)
        
        # Check functions
        self.assertIn("gpio_set_mode", gpio_c)
        self.assertIn("gpio_write", gpio_c)
        self.assertIn("gpio_read", gpio_c)
        
    def test_i2c_c_correctness(self):
        """Test I2C C code correctness"""
        i2c_c = self.transpiler._generate_i2c_c()
        
        # Check register definitions
        self.assertIn("BSC_C", i2c_c)
        self.assertIn("BSC_S", i2c_c)
        self.assertIn("BSC_DLEN", i2c_c)
        
        # Check functions
        self.assertIn("i2c_init", i2c_c)
        self.assertIn("i2c_write", i2c_c)
        self.assertIn("i2c_read", i2c_c)
        
    def test_uart_c_correctness(self):
        """Test UART C code correctness"""
        uart_c = self.transpiler._generate_uart_c()
        
        # Check register definitions
        self.assertIn("UART_DR", uart_c)
        self.assertIn("UART_FR", uart_c)
        self.assertIn("UART_CR", uart_c)
        
        # Check functions
        self.assertIn("uart_init", uart_c)
        self.assertIn("uart_write", uart_c)
        self.assertIn("uart_read", uart_c)
        self.assertIn("uart_available", uart_c)
        
    def test_hal_header_correctness(self):
        """Test HAL header file"""
        hal_h = self.transpiler._generate_hal_header()
        
        # Check include guards
        self.assertIn("#ifndef EMBODIOS_HAL_H", hal_h)
        self.assertIn("#define EMBODIOS_HAL_H", hal_h)
        
        # Check type definitions
        self.assertIn("gpio_mode_t", hal_h)
        self.assertIn("command_type_t", hal_h)
        self.assertIn("hardware_command_t", hal_h)
        self.assertIn("hal_state_t", hal_h)
        
        # Check function declarations
        self.assertIn("gpio_init", hal_h)
        self.assertIn("i2c_init", hal_h)
        self.assertIn("uart_init", hal_h)
        self.assertIn("hal_init", hal_h)
        
        
class TestTranspilerIntegration(unittest.TestCase):
    """Integration tests for transpiler"""
    
    def test_component_transpilation(self):
        """Test transpiling specific components"""
        from embodi.compiler.embodios_transpiler import transpile_embodios_component
        
        # Test hardware tokens
        token_code = """
hardware_tokens = {
    "<TEST_TOKEN>": 12345
}
"""
        result = transpile_embodios_component("hardware_tokens", token_code)
        self.assertIn("tokens.h", result)
        self.assertIn("TEST_TOKEN", result["tokens.h"])
        
    def test_pattern_matching_conversion(self):
        """Test pattern matching logic in C"""
        transpiler = EMBODIOSTranspiler()
        cmd_proc = transpiler.transpile_command_processor("")
        
        # Verify pattern matching implementation
        self.assertIn("strstr", cmd_proc)
        self.assertIn("extract_number", cmd_proc)
        self.assertIn("command_patterns", cmd_proc)


if __name__ == '__main__':
    unittest.main()