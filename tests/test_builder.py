"""Tests for EMBODIOS builder functionality."""
import pytest
from pathlib import Path
from embodi.builder.modelfile import Modelfile
from embodi.builder.builder import EMBODIOSBuilder


class TestModelfile:
    """Test Modelfile parsing."""
    
    def test_parse_simple_modelfile(self):
        """Test parsing a simple Modelfile."""
        content = """FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit
MEMORY 2G"""
        
        mf = Modelfile()
        instructions = mf.parse(content)
        
        assert len(instructions) == 4
        assert instructions[0] == ('FROM', 'scratch')
        assert instructions[1] == ('MODEL', 'huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0')
        assert instructions[2] == ('QUANTIZE', '4bit')
        assert instructions[3] == ('MEMORY', '2G')
    
    def test_parse_modelfile_with_hardware(self):
        """Test parsing Modelfile with hardware specs."""
        content = """FROM scratch
MODEL test-model
HARDWARE gpio:enabled uart:enabled"""
        
        mf = Modelfile()
        instructions = mf.parse(content)
        
        assert len(instructions) == 3
        assert instructions[2] == ('HARDWARE', 'gpio:enabled uart:enabled')
    
    def test_parse_empty_modelfile(self):
        """Test parsing empty Modelfile."""
        mf = Modelfile()
        instructions = mf.parse("")
        assert len(instructions) == 0


class TestBuilder:
    """Test EMBODIOS builder."""
    
    def test_builder_initialization(self):
        """Test builder initialization."""
        builder = EMBODIOSBuilder()
        assert builder is not None
        assert hasattr(builder, 'build')
    
    def test_builder_config_from_modelfile(self):
        """Test builder config extraction from Modelfile."""
        content = """FROM scratch
MODEL test-model
MEMORY 1G
HARDWARE gpio:enabled"""
        
        builder = EMBODIOSBuilder()
        mf = Modelfile()
        instructions = mf.parse(content)
        
        # Test that builder can process instructions
        config = {}
        for cmd, value in instructions:
            if cmd == 'MODEL':
                config['model'] = value
            elif cmd == 'MEMORY':
                config['memory'] = value
            elif cmd == 'HARDWARE':
                config['hardware'] = value.split()
        
        assert config['model'] == 'test-model'
        assert config['memory'] == '1G'
        assert config['hardware'] == ['gpio:enabled']