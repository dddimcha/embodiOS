"""
Unit tests for EMBODIOS core components
"""

import pytest
from pathlib import Path
import sys

# Add src to path
sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

from embodi.core.hal import HardwareAbstractionLayer
from embodi.core.inference import EMBODIOSInferenceEngine
from embodi.core.nl_processor import NaturalLanguageProcessor, CommandType


class TestHAL:
    """Test Hardware Abstraction Layer"""
    
    def test_hal_initialization(self):
        """Test HAL can be initialized"""
        hal = HardwareAbstractionLayer()
        hal.initialize()
        assert len(hal.devices) > 0
        assert 'gpio' in hal.devices
    
    def test_gpio_device(self):
        """Test GPIO device operations"""
        hal = HardwareAbstractionLayer()
        hal.initialize()
        
        gpio = hal.get_device('gpio')
        assert gpio is not None
        
        # Test setup
        gpio.setup(17, 'output')
        assert 17 in gpio.pins
        assert gpio.pins[17]['mode'] == 'output'


class TestInferenceEngine:
    """Test Inference Engine"""
    
    def test_engine_initialization(self):
        """Test engine can be initialized"""
        engine = EMBODIOSInferenceEngine()
        assert engine.model_loaded == False
        assert engine.hardware_tokens is not None
        assert '<GPIO_WRITE>' in engine.hardware_tokens
    
    def test_hardware_token_extraction(self):
        """Test extracting hardware commands from tokens"""
        engine = EMBODIOSInferenceEngine()
        
        # Test GPIO write command
        tokens = [
            engine.hardware_tokens['<GPIO_WRITE>'],
            17,
            engine.hardware_tokens['<GPIO_HIGH>']
        ]
        
        commands = engine._extract_hardware_commands(tokens)
        assert len(commands) == 1
        assert commands[0]['type'] == 'gpio_write'
        assert commands[0]['pin'] == 17
        assert commands[0]['value'] == True


class TestNaturalLanguageProcessor:
    """Test Natural Language Processor"""
    
    def test_nlp_initialization(self):
        """Test NLP can be initialized"""
        nlp = NaturalLanguageProcessor()
        assert nlp.patterns is not None
        assert CommandType.GPIO in nlp.patterns
    
    def test_gpio_command_parsing(self):
        """Test parsing GPIO commands"""
        nlp = NaturalLanguageProcessor()
        
        # Test turn on command
        commands = nlp.process("Turn on GPIO pin 17")
        assert len(commands) == 1
        assert commands[0].command_type == CommandType.GPIO
        assert commands[0].action == 'write'
        assert commands[0].parameters['pin'] == 17
        assert commands[0].parameters['value'] == True
        
        # Test turn off command
        commands = nlp.process("Turn off pin 23")
        assert len(commands) == 1
        assert commands[0].parameters['pin'] == 23
        assert commands[0].parameters['value'] == False
    
    def test_device_alias_expansion(self):
        """Test device alias expansion"""
        nlp = NaturalLanguageProcessor()
        
        # Test LED alias
        commands = nlp.process("Turn on the LED")
        assert len(commands) == 1
        assert commands[0].command_type == CommandType.GPIO
        assert commands[0].parameters['pin'] == 13  # Default LED pin
    
    def test_i2c_command_parsing(self):
        """Test parsing I2C commands"""
        nlp = NaturalLanguageProcessor()
        
        # Test temperature sensor
        commands = nlp.process("Read temperature sensor")
        assert len(commands) == 1
        assert commands[0].command_type == CommandType.I2C
        assert commands[0].action == 'read'
        assert commands[0].parameters['device'] == 0x48  # Temperature sensor address
    
    def test_response_generation(self):
        """Test natural language response generation"""
        nlp = NaturalLanguageProcessor()
        
        commands = nlp.process("Turn on pin 17")
        response = nlp.generate_response(commands, [True])
        assert "GPIO pin 17 set to HIGH" in response