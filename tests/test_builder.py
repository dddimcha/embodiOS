"""Tests for EMBODIOS builder functionality."""
import pytest
from pathlib import Path
from embodi.builder.modelfile import ModelfileParser
from embodi.builder.builder import EmbodiBuilder


class TestModelfile:
    """Test Modelfile parsing."""
    
    def test_parse_simple_modelfile(self, tmp_path):
        """Test parsing a simple Modelfile."""
        content = """FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit
MEMORY 2G"""
        
        # Write content to a temporary file
        modelfile = tmp_path / "Modelfile"
        modelfile.write_text(content)
        
        parser = ModelfileParser(str(modelfile))
        spec = parser.parse()
        
        assert spec['from'] == 'scratch'
        assert spec['model']['source'] == 'huggingface'
        assert spec['model']['name'] == 'TinyLlama/TinyLlama-1.1B-Chat-v1.0'
        assert spec['model']['quantization'] == '4bit'
        assert spec['system']['memory'] == '2G'
    
    def test_parse_modelfile_with_hardware(self, tmp_path):
        """Test parsing Modelfile with hardware specs."""
        content = """FROM scratch
MODEL test-model
HARDWARE gpio:enabled
HARDWARE uart:enabled"""
        
        # Write content to a temporary file
        modelfile = tmp_path / "Modelfile"
        modelfile.write_text(content)
        
        parser = ModelfileParser(str(modelfile))
        spec = parser.parse()
        
        assert spec['from'] == 'scratch'
        assert spec['model']['name'] == 'test-model'
        assert 'gpio' in spec['hardware']
        assert spec['hardware']['gpio'] == 'enabled'
        assert 'uart' in spec['hardware']
        assert spec['hardware']['uart'] == 'enabled'
    
    def test_parse_empty_modelfile(self, tmp_path):
        """Test parsing empty Modelfile."""
        # Write empty content to a temporary file
        modelfile = tmp_path / "Modelfile"
        modelfile.write_text("")
        
        parser = ModelfileParser(str(modelfile))
        spec = parser.parse()
        
        assert spec['from'] == 'scratch'  # default value
        assert spec['commands'] == []


class TestBuilder:
    """Test EMBODIOS builder."""
    
    def test_builder_initialization(self):
        """Test builder initialization."""
        builder = EmbodiBuilder()
        assert builder is not None
        assert hasattr(builder, 'build')
    
    def test_builder_config_from_modelfile(self, tmp_path):
        """Test builder config extraction from Modelfile."""
        content = """FROM scratch
MODEL test-model
MEMORY 1G
HARDWARE gpio:enabled"""
        
        # Write content to a temporary file
        modelfile = tmp_path / "Modelfile"
        modelfile.write_text(content)
        
        parser = ModelfileParser(str(modelfile))
        spec = parser.parse()
        
        # Test that we can extract config from parsed spec
        assert spec['model']['name'] == 'test-model'
        assert spec['system']['memory'] == '1G'
        assert 'gpio' in spec['hardware']
        assert spec['hardware']['gpio'] == 'enabled'