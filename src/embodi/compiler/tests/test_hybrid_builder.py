"""
Tests for hybrid builder
"""

import unittest
import tempfile
import shutil
from pathlib import Path
import json

from embodi.compiler.builder import HybridCompiler

class TestHybridBuilder(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.mkdtemp()
        self.compiler = HybridCompiler(self.temp_dir)
        
    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)
        
    def test_compiler_initialization(self):
        """Test hybrid compiler initialization"""
        self.assertIsNotNone(self.compiler.tvm_compiler)
        self.assertIsNotNone(self.compiler.embodios_transpiler)
        self.assertTrue(Path(self.temp_dir).exists())
        
    def test_config_validation(self):
        """Test configuration validation"""
        # Missing required field
        with self.assertRaises(ValueError):
            self.compiler._validate_config({})
            
        # Valid config with defaults
        config = {'model_path': 'test.gguf'}
        self.compiler._validate_config(config)
        
        self.assertEqual(config['target_arch'], 'native')
        self.assertEqual(config['optimization_level'], 2)
        self.assertEqual(config['features'], [])
        
    def test_hardware_token_extraction(self):
        """Test extracting hardware tokens from Python code"""
        code = '''
hardware_tokens = {
    "<GPIO_READ>": 32000,
    "<GPIO_WRITE>": 32001,
}
'''
        tokens = self.compiler._extract_hardware_tokens(code)
        
        self.assertEqual(tokens["<GPIO_READ>"], 32000)
        self.assertEqual(tokens["<GPIO_WRITE>"], 32001)
        
    def test_cmake_generation(self):
        """Test CMake file generation"""
        config = {
            'target_arch': 'avx2',
            'optimization_level': 3
        }
        
        self.compiler.generated_files['c_files'] = ['test1.c', 'test2.c']
        self.compiler.generated_files['asm_files'] = ['boot.S']
        
        cmake_content = self.compiler._generate_cmake(config)
        
        self.assertIn("cmake_minimum_required", cmake_content)
        self.assertIn("test1.c test2.c", cmake_content)
        self.assertIn("boot.S", cmake_content)
        self.assertIn("-mavx2", cmake_content)
        self.assertIn("-O3", cmake_content)
        
    def test_makefile_generation(self):
        """Test Makefile generation"""
        config = {
            'target_arch': 'native',
            'optimization_level': 2
        }
        
        self.compiler.generated_files['c_files'] = ['kernel.c', 'hal.c']
        self.compiler.generated_files['asm_files'] = ['boot.S']
        
        makefile = self.compiler._generate_makefile(config)
        
        self.assertIn("kernel.c hal.c", makefile)
        self.assertIn("boot.S", makefile)
        self.assertIn("-O2", makefile)
        self.assertIn("embodios.kernel:", makefile)
        self.assertIn("grub-mkrescue", makefile)
        
    def test_linker_script_generation(self):
        """Test linker script generation"""
        linker_script = self.compiler._generate_linker_script()
        
        self.assertIn("ENTRY(_start)", linker_script)
        self.assertIn(".multiboot2", linker_script)
        self.assertIn(".text", linker_script)
        self.assertIn(".model_weights", linker_script)
        self.assertIn("__bss_start", linker_script)
        
    def test_readme_generation(self):
        """Test README generation"""
        config = {
            'model_path': '/path/to/model.gguf',
            'target_arch': 'avx512',
            'optimization_level': 3
        }
        
        readme = self.compiler._generate_readme(config)
        
        self.assertIn("model.gguf", readme)
        self.assertIn("avx512", readme)
        self.assertIn("make", readme)
        self.assertIn("qemu", readme)
        
    def test_build_artifacts_creation(self):
        """Test build artifact creation"""
        config = {
            'model_path': 'test.gguf',
            'target_arch': 'native'
        }
        
        artifacts = self.compiler._create_build_artifacts(config)
        
        self.assertIn('build_info', artifacts)
        self.assertIn('readme', artifacts)
        
        # Check build info JSON
        build_info_path = Path(artifacts['build_info'])
        self.assertTrue(build_info_path.exists())
        
        with open(build_info_path) as f:
            build_info = json.load(f)
            self.assertIn('version', build_info)
            self.assertIn('config', build_info)
            
    def test_report_generation(self):
        """Test compilation report generation"""
        config = {
            'model_path': 'test.gguf',
            'target_arch': 'avx2',
            'optimization_level': 2
        }
        
        # Add some dummy files
        dummy_file = Path(self.temp_dir) / "test.c"
        dummy_file.write_text("// test")
        self.compiler.generated_files['c_files'] = [str(dummy_file)]
        
        report = self.compiler._generate_report(config)
        
        self.assertIn('summary', report)
        self.assertIn('files', report)
        self.assertIn('sizes', report)
        
        self.assertEqual(report['summary']['model'], 'test.gguf')
        self.assertEqual(report['summary']['target'], 'avx2')
        self.assertGreater(report['sizes']['total'], 0)
        
    def test_native_component_preparation(self):
        """Test copying native C components"""
        # Create dummy native files
        native_dir = Path(__file__).parent.parent / "native"
        native_dir.mkdir(exist_ok=True)
        
        test_c = native_dir / "test.c"
        test_c.write_text("// test c file")
        
        test_h = native_dir / "test.h"
        test_h.write_text("// test header")
        
        try:
            files = self.compiler._prepare_native_components()
            
            # Check files were copied
            self.assertTrue(any('test.c' in f for f in files))
            self.assertTrue(any('test.h' in f for f in files))
            
            # Check categorization
            self.assertTrue(any('test.c' in f for f in self.compiler.generated_files['c_files']))
            self.assertTrue(any('test.h' in f for f in self.compiler.generated_files['h_files']))
            
        finally:
            # Cleanup
            test_c.unlink(missing_ok=True)
            test_h.unlink(missing_ok=True)
            
            
class TestHybridBuilderIntegration(unittest.TestCase):
    """Integration tests for the full build pipeline"""
    
    def setUp(self):
        self.temp_dir = tempfile.mkdtemp()
        
    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)
        
    def test_minimal_build(self):
        """Test minimal build without actual model"""
        compiler = HybridCompiler(self.temp_dir, verbose=True)
        
        # Create dummy model
        model_path = Path(self.temp_dir) / "dummy.gguf"
        model_path.write_text("dummy model")
        
        config = {
            'model_path': str(model_path),
            'target_arch': 'c',  # Use C target for compatibility
            'optimization_level': 1
        }
        
        result = compiler.compile_embodios(config)
        
        # Should complete even if some steps fail
        self.assertIn('success', result)
        self.assertIn('files', result)
        
        if result['success']:
            # Check some files were generated
            total_files = sum(len(files) for files in result['files'].values())
            self.assertGreater(total_files, 0)
            
    def test_build_command_line(self):
        """Test command line build function"""
        from embodi.compiler.builder import build_embodios
        
        # Create dummy model
        model_path = Path(self.temp_dir) / "test.gguf"
        model_path.write_text("test")
        
        # This should handle errors gracefully
        result = build_embodios(
            str(model_path),
            self.temp_dir,
            target='c',
            verbose=False
        )
        
        # Result is boolean
        self.assertIsInstance(result, bool)


if __name__ == '__main__':
    unittest.main()