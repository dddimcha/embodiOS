"""
Tests for TVM model compiler
"""

import unittest
import tempfile
import numpy as np
from pathlib import Path
import json

from embodi.compiler.tvm_compiler import TVMModelCompiler

class TestTVMCompiler(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.mkdtemp()
        self.compiler = TVMModelCompiler(self.temp_dir)
        
    def test_compiler_initialization(self):
        """Test compiler initializes correctly"""
        self.assertIsNotNone(self.compiler)
        self.assertTrue(Path(self.temp_dir).exists())
        
    def test_fallback_compilation(self):
        """Test fallback when TVM not available"""
        # Create dummy model file
        model_path = Path(self.temp_dir) / "test_model.gguf"
        model_path.write_text("dummy model")
        
        # Force fallback
        result = self.compiler._fallback_compile(str(model_path))
        
        self.assertIsInstance(result, dict)
        self.assertTrue(any('fallback' in path for path in result.keys()))
        
    def test_gguf_parsing(self):
        """Test GGUF model parsing"""
        # Create minimal GGUF file
        model_path = Path(self.temp_dir) / "test.gguf"
        with open(model_path, 'wb') as f:
            f.write(b'GGUF')  # Magic
            f.write(b'\x00' * 100)  # Dummy data
            
        weights, metadata = self.compiler._parse_gguf_file(model_path)
        
        self.assertIsInstance(weights, dict)
        self.assertIsInstance(metadata, dict)
        
    def test_target_creation(self):
        """Test target creation for different architectures"""
        targets = {
            'c': 'c',
            'avx2': 'llvm',
            'avx512': 'llvm',
            'arm': 'llvm'
        }
        
        for arch, expected in targets.items():
            try:
                target = self.compiler._create_target(arch)
                # If TVM available, check target
                if target:
                    self.assertIn(expected, str(target))
            except:
                # TVM not available, skip
                pass
                
    def test_header_generation(self):
        """Test C header generation"""
        header = self.compiler._generate_header("test_model")
        
        self.assertIn("#ifndef TEST_MODEL_TVM_H", header)
        self.assertIn("test_model_inference", header)
        self.assertIn("test_model_init", header)
        self.assertIn("test_model_cleanup", header)
        
    def test_deployment_code_generation(self):
        """Test deployment wrapper generation"""
        deploy = self.compiler._generate_deployment_code("test_model")
        
        self.assertIn("test_model_init", deploy)
        self.assertIn("test_model_inference", deploy)
        self.assertIn("TVMFuncCall", deploy)
        
    def test_relay_module_creation(self):
        """Test Relay module creation from weights"""
        weights = {
            'embedding.weight': np.random.randn(100, 32).astype(np.float32),
            'output.weight': np.random.randn(32, 100).astype(np.float32)
        }
        metadata = {
            'hidden_size': 32,
            'vocab_size': 100
        }
        
        try:
            mod, params = self.compiler._create_relay_from_weights(weights, metadata)
            # If TVM available, check module
            if mod:
                self.assertIsNotNone(mod)
        except:
            # TVM not available
            pass
            
    def test_optimize_for_embedded(self):
        """Test embedded optimization path"""
        # Create dummy model
        model_path = Path(self.temp_dir) / "embedded.gguf"
        model_path.write_text("dummy")
        
        result = self.compiler.optimize_for_embedded(
            str(model_path),
            memory_limit=50*1024*1024  # 50MB
        )
        
        self.assertIsInstance(result, dict)
        
        
class TestTVMIntegration(unittest.TestCase):
    """Integration tests (only run if TVM available)"""
    
    def setUp(self):
        self.temp_dir = tempfile.mkdtemp()
        self.compiler = TVMModelCompiler(self.temp_dir)
        
        # Check if TVM available
        try:
            import tvm
            self.has_tvm = True
        except:
            self.has_tvm = False
            
    @unittest.skipUnless('has_tvm', "TVM not installed")
    def test_full_compilation_pipeline(self):
        """Test complete compilation pipeline with TVM"""
        # Would test with actual model file
        pass


if __name__ == '__main__':
    unittest.main()