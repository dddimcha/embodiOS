"""
EMBODIOS Hybrid Builder - Orchestrates the compilation process
"""

import os
import subprocess
import shutil
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Any
import json
import logging

try:
    from .tvm_compiler import TVMModelCompiler
    from .embodios_transpiler import EMBODIOSTranspiler, transpile_embodios_component
except ImportError:
    from tvm_compiler import TVMModelCompiler
    from embodios_transpiler import EMBODIOSTranspiler, transpile_embodios_component

class HybridCompiler:
    """Orchestrate the hybrid compilation process"""
    
    def __init__(self, output_dir: str = "build/native", verbose: bool = False):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        self.tvm_compiler = TVMModelCompiler(str(self.output_dir))
        self.embodios_transpiler = EMBODIOSTranspiler()
        
        self.verbose = verbose
        self.logger = logging.getLogger(__name__)
        if verbose:
            logging.basicConfig(level=logging.INFO)
            
        # Track generated files
        self.generated_files = {
            'c_files': [],
            'h_files': [],
            'asm_files': [],
            'pyx_files': [],
            'object_files': []
        }
        
    def compile_embodios(self, config: Dict[str, Any]) -> Dict[str, Any]:
        """
        Main compilation entry point
        
        Args:
            config: Build configuration including:
                - model_path: Path to AI model
                - target_arch: Target architecture
                - optimization_level: 0-3
                - features: List of features to enable
                
        Returns:
            Dictionary with build results
        """
        self.logger.info("Starting EMBODIOS hybrid compilation")
        
        # Validate configuration
        self._validate_config(config)
        
        try:
            # 1. Compile model with TVM
            self.logger.info("Compiling AI model with TVM...")
            model_files = self._compile_model(config)
            
            # 2. Transpile EMBODIOS-specific code
            self.logger.info("Transpiling EMBODIOS components...")
            embodios_files = self._transpile_embodios_code()
            
            # 3. Compile Cython modules
            self.logger.info("Building Cython performance modules...")
            cython_files = self._compile_cython_modules()
            
            # 4. Copy native C components
            self.logger.info("Preparing native C components...")
            native_files = self._prepare_native_components()
            
            # 5. Generate build configuration
            self.logger.info("Generating build configuration...")
            build_config = self._generate_build_config(config)
            
            # 6. Create final build artifacts
            self.logger.info("Creating build artifacts...")
            artifacts = self._create_build_artifacts(config)
            
            # 7. Generate compilation report
            report = self._generate_report(config)
            
            return {
                'success': True,
                'artifacts': artifacts,
                'report': report,
                'files': self.generated_files
            }
            
        except Exception as e:
            self.logger.error(f"Compilation failed: {e}")
            return {
                'success': False,
                'error': str(e),
                'files': self.generated_files
            }
    
    def _validate_config(self, config: Dict[str, Any]):
        """Validate build configuration"""
        required_fields = ['model_path']
        for field in required_fields:
            if field not in config:
                raise ValueError(f"Missing required configuration field: {field}")
                
        # Set defaults
        config.setdefault('target_arch', 'native')
        config.setdefault('optimization_level', 2)
        config.setdefault('features', [])
        
    def _compile_model(self, config: Dict[str, Any]) -> Dict[str, str]:
        """Compile AI model using TVM"""
        model_path = config['model_path']
        target_arch = config['target_arch']
        opt_level = config['optimization_level']
        
        # Map target architecture to TVM target
        if target_arch == 'native':
            # Detect native architecture
            import platform
            machine = platform.machine().lower()
            if 'x86' in machine or 'amd64' in machine:
                target_arch = 'avx2'  # Conservative default
            elif 'arm' in machine or 'aarch64' in machine:
                target_arch = 'arm'
            else:
                target_arch = 'c'
                
        # Compile model
        model_files = self.tvm_compiler.compile_model(
            model_path, 
            target_arch,
            opt_level
        )
        
        # Track generated files
        for path, content in model_files.items():
            if path.endswith('.c'):
                self.generated_files['c_files'].append(path)
            elif path.endswith('.h'):
                self.generated_files['h_files'].append(path)
                
        return model_files
    
    def _transpile_embodios_code(self) -> Dict[str, str]:
        """Transpile EMBODIOS Python code to C"""
        transpiled_files = {}
        
        # Get EMBODIOS source directory
        embodios_src = Path(__file__).parent.parent / "core"
        
        # Transpile hardware tokens
        if (embodios_src / "inference.py").exists():
            with open(embodios_src / "inference.py") as f:
                code = f.read()
                
            # Extract hardware tokens
            tokens = self._extract_hardware_tokens(code)
            token_h = self.embodios_transpiler.transpile_hardware_tokens(tokens)
            
            token_path = self.output_dir / "tokens.h"
            token_path.write_text(token_h)
            transpiled_files[str(token_path)] = token_h
            self.generated_files['h_files'].append(str(token_path))
        
        # Transpile HAL operations
        hal_files = self.embodios_transpiler.transpile_hal_operations("")
        for filename, content in hal_files.items():
            path = self.output_dir / filename
            path.write_text(content)
            transpiled_files[str(path)] = content
            
            if filename.endswith('.c'):
                self.generated_files['c_files'].append(str(path))
            elif filename.endswith('.h'):
                self.generated_files['h_files'].append(str(path))
        
        # Transpile command processor
        nl_c = self.embodios_transpiler.transpile_command_processor("")
        nl_path = self.output_dir / "nl_processor.c"
        nl_path.write_text(nl_c)
        transpiled_files[str(nl_path)] = nl_c
        self.generated_files['c_files'].append(str(nl_path))
        
        # Transpile runtime kernel
        kernel_c = self.embodios_transpiler.transpile_runtime_kernel("")
        kernel_path = self.output_dir / "kernel.c"
        kernel_path.write_text(kernel_c)
        transpiled_files[str(kernel_path)] = kernel_c
        self.generated_files['c_files'].append(str(kernel_path))
        
        return transpiled_files
    
    def _extract_hardware_tokens(self, code: str) -> Dict[str, int]:
        """Extract hardware token definitions from Python code"""
        import ast
        
        tokens = {}
        try:
            tree = ast.parse(code)
            for node in ast.walk(tree):
                if isinstance(node, ast.Assign):
                    for target in node.targets:
                        if isinstance(target, ast.Name) and target.id == 'hardware_tokens':
                            if isinstance(node.value, ast.Call):
                                # It's a method call, skip
                                continue
                            elif isinstance(node.value, ast.Dict):
                                for k, v in zip(node.value.keys, node.value.values):
                                    if isinstance(k, ast.Constant) and isinstance(v, ast.Constant):
                                        tokens[k.value] = v.value
        except:
            # Fallback to default tokens
            tokens = {
                "<GPIO_READ>": 32000,
                "<GPIO_WRITE>": 32001,
                "<GPIO_HIGH>": 32002,
                "<GPIO_LOW>": 32003,
                "<MEM_READ>": 32010,
                "<MEM_WRITE>": 32011,
                "<I2C_READ>": 32020,
                "<I2C_WRITE>": 32021,
                "<UART_TX>": 32023,
                "<UART_RX>": 32024,
            }
            
        return tokens
    
    def _compile_cython_modules(self) -> List[str]:
        """Compile Cython modules"""
        cython_dir = Path(__file__).parent / "cython_modules"
        if not cython_dir.exists():
            return []
            
        # Check if Cython is available
        try:
            import cython
            from Cython.Build import cythonize
        except ImportError:
            self.logger.warning("Cython not available, skipping Cython modules")
            return []
            
        # Build Cython extensions
        os.chdir(cython_dir)
        try:
            import sys
            result = subprocess.run(
                [sys.executable, "setup.py", "build_ext", "--inplace"],
                capture_output=True,
                text=True
            )
            
            if result.returncode != 0:
                self.logger.warning(f"Cython build failed: {result.stderr}")
                return []
                
            # Find generated .so/.pyd files
            cython_files = []
            for ext in ['*.so', '*.pyd']:
                cython_files.extend(cython_dir.glob(ext))
                
            # Copy to output directory
            for file in cython_files:
                dest = self.output_dir / file.name
                shutil.copy2(file, dest)
                self.generated_files['object_files'].append(str(dest))
                
            return [str(f) for f in cython_files]
            
        finally:
            os.chdir(Path(__file__).parent)
    
    def _prepare_native_components(self) -> List[str]:
        """Copy pre-written native C components"""
        native_dir = Path(__file__).parent / "native"
        if not native_dir.exists():
            return []
            
        native_files = []
        
        # Copy all C/H/S files
        for pattern in ['*.c', '*.h', '*.S']:
            for file in native_dir.glob(pattern):
                dest = self.output_dir / file.name
                shutil.copy2(file, dest)
                native_files.append(str(dest))
                
                if file.suffix == '.c':
                    self.generated_files['c_files'].append(str(dest))
                elif file.suffix == '.h':
                    self.generated_files['h_files'].append(str(dest))
                elif file.suffix == '.S':
                    self.generated_files['asm_files'].append(str(dest))
                    
        return native_files
    
    def _generate_build_config(self, config: Dict[str, Any]) -> Dict[str, Any]:
        """Generate build configuration files"""
        # Create CMakeLists.txt
        cmake_content = self._generate_cmake(config)
        cmake_path = self.output_dir / "CMakeLists.txt"
        cmake_path.write_text(cmake_content)
        
        # Create Makefile for simpler builds
        makefile_content = self._generate_makefile(config)
        makefile_path = self.output_dir / "Makefile"
        makefile_path.write_text(makefile_content)
        
        # Create linker script
        linker_script = self._generate_linker_script()
        linker_path = self.output_dir / "link.ld"
        linker_path.write_text(linker_script)
        
        return {
            'cmake': str(cmake_path),
            'makefile': str(makefile_path),
            'linker_script': str(linker_path)
        }
    
    def _generate_cmake(self, config: Dict[str, Any]) -> str:
        """Generate CMakeLists.txt"""
        c_files = ' '.join(Path(f).name for f in self.generated_files['c_files'])
        asm_files = ' '.join(Path(f).name for f in self.generated_files['asm_files'])
        
        return f"""cmake_minimum_required(VERSION 3.10)
project(embodios_native C ASM)

# Compiler flags
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS}} -ffreestanding -nostdlib -O{config['optimization_level']}")
set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS}} -Wall -Wextra -mno-red-zone -mno-mmx -mno-sse -mno-sse2")

# Architecture-specific flags
if("{config['target_arch']}" STREQUAL "avx2")
    set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS}} -mavx2")
elseif("{config['target_arch']}" STREQUAL "avx512")
    set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS}} -mavx512f")
endif()

# Source files
set(C_SOURCES {c_files})
set(ASM_SOURCES {asm_files})

# Create kernel executable
add_executable(embodios.kernel ${{C_SOURCES}} ${{ASM_SOURCES}})

# Link options
set_target_properties(embodios.kernel PROPERTIES LINK_FLAGS "-T ${{CMAKE_CURRENT_SOURCE_DIR}}/link.ld")

# Create bootable ISO
add_custom_command(
    TARGET embodios.kernel POST_BUILD
    COMMAND grub-mkrescue -o embodios.iso iso/
    COMMENT "Creating bootable ISO"
)
"""
    
    def _generate_makefile(self, config: Dict[str, Any]) -> str:
        """Generate Makefile"""
        c_files = ' '.join(Path(f).name for f in self.generated_files['c_files'])
        asm_files = ' '.join(Path(f).name for f in self.generated_files['asm_files'])
        obj_files = c_files.replace('.c', '.o') + ' ' + asm_files.replace('.S', '.o')
        
        return f"""# EMBODIOS Native Makefile
CC = gcc
AS = as
LD = ld

CFLAGS = -ffreestanding -nostdlib -O{config['optimization_level']} -Wall -Wextra
CFLAGS += -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel
ASFLAGS = 
LDFLAGS = -nostdlib -T link.ld

C_SOURCES = {c_files}
ASM_SOURCES = {asm_files}
OBJECTS = {obj_files}

all: embodios.kernel

embodios.kernel: $(OBJECTS)
\t$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c
\t$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
\t$(AS) $(ASFLAGS) -o $@ $<

clean:
\trm -f *.o embodios.kernel embodios.iso

iso: embodios.kernel
\tmkdir -p iso/boot/grub
\tcp embodios.kernel iso/boot/
\techo 'set timeout=0' > iso/boot/grub/grub.cfg
\techo 'set default=0' >> iso/boot/grub/grub.cfg
\techo 'menuentry "EMBODIOS" {{' >> iso/boot/grub/grub.cfg
\techo '  multiboot2 /boot/embodios.kernel' >> iso/boot/grub/grub.cfg
\techo '  boot' >> iso/boot/grub/grub.cfg
\techo '}}' >> iso/boot/grub/grub.cfg
\tgrub-mkrescue -o embodios.iso iso/

run: iso
\tqemu-system-x86_64 -cdrom embodios.iso -m 4G

.PHONY: all clean iso run
"""
    
    def _generate_linker_script(self) -> str:
        """Generate linker script"""
        return """/* EMBODIOS Linker Script */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    
    .multiboot2 :
    {
        *(.multiboot2)
    }
    
    .text :
    {
        *(.text.boot)
        *(.text)
    }
    
    .rodata :
    {
        *(.rodata)
    }
    
    .data :
    {
        *(.data)
    }
    
    .model_weights ALIGN(64) :
    {
        *(.model_weights)
    }
    
    .bss :
    {
        __bss_start = .;
        *(.bss)
        __bss_end = .;
    }
    
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
"""
    
    def _create_build_artifacts(self, config: Dict[str, Any]) -> Dict[str, str]:
        """Create final build artifacts"""
        artifacts = {}
        
        # Create build info JSON
        import time
        build_info = {
            'version': '0.1.0',
            'build_date': time.strftime('%Y-%m-%d %H:%M:%S'),
            'config': config,
            'files': self.generated_files
        }
        
        build_info_path = self.output_dir / "build_info.json"
        build_info_path.write_text(json.dumps(build_info, indent=2))
        artifacts['build_info'] = str(build_info_path)
        
        # Create README
        readme = self._generate_readme(config)
        readme_path = self.output_dir / "README.md"
        readme_path.write_text(readme)
        artifacts['readme'] = str(readme_path)
        
        return artifacts
    
    def _generate_report(self, config: Dict[str, Any]) -> Dict[str, Any]:
        """Generate compilation report"""
        report = {
            'summary': {
                'model': Path(config['model_path']).name,
                'target': config['target_arch'],
                'optimization': config['optimization_level'],
                'total_files': sum(len(files) for files in self.generated_files.values())
            },
            'files': self.generated_files,
            'sizes': {}
        }
        
        # Calculate file sizes
        total_size = 0
        for category, files in self.generated_files.items():
            category_size = 0
            for file in files:
                if Path(file).exists():
                    size = Path(file).stat().st_size
                    category_size += size
                    total_size += size
            report['sizes'][category] = category_size
            
        report['sizes']['total'] = total_size
        
        return report
    
    def _generate_readme(self, config: Dict[str, Any]) -> str:
        """Generate README for the build"""
        return f"""# EMBODIOS Native Build

This directory contains the native build of EMBODIOS with the following configuration:

- **Model**: {Path(config['model_path']).name}
- **Target Architecture**: {config['target_arch']}
- **Optimization Level**: {config['optimization_level']}

## Building

To build the kernel:
```bash
make
```

To create a bootable ISO:
```bash
make iso
```

To run in QEMU:
```bash
make run
```

## Files

- `embodios.kernel` - The main kernel binary
- `*.c` - C source files (generated and hand-written)
- `*.h` - Header files
- `*.S` - Assembly files
- `link.ld` - Linker script
- `Makefile` - Build configuration

## Architecture

The kernel consists of:
1. TVM-compiled AI model
2. EMBODIOS runtime components
3. Hardware abstraction layer
4. Memory management
5. Boot loader

Generated by EMBODIOS Hybrid Compiler v0.1.0
"""


# Convenience function for command-line usage
def build_embodios(model_path: str, output_dir: str = "build/native", 
                  target: str = "native", verbose: bool = False) -> bool:
    """
    Build EMBODIOS from command line
    
    Args:
        model_path: Path to AI model
        output_dir: Output directory
        target: Target architecture
        verbose: Enable verbose output
        
    Returns:
        True if build succeeded
    """
    compiler = HybridCompiler(output_dir, verbose)
    
    config = {
        'model_path': model_path,
        'target_arch': target,
        'optimization_level': 3,
        'features': []
    }
    
    result = compiler.compile_embodios(config)
    
    if result['success']:
        print(f"Build successful! Output in: {output_dir}")
        print(f"Total files generated: {result['report']['summary']['total_files']}")
        print(f"Total size: {result['report']['sizes']['total'] / 1024 / 1024:.2f} MB")
        return True
    else:
        print(f"Build failed: {result['error']}")
        return False


if __name__ == "__main__":
    import sys
    from datetime import datetime
    
    if len(sys.argv) < 2:
        print("Usage: python builder.py <model_path> [output_dir] [target]")
        sys.exit(1)
        
    model_path = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "build/native"
    target = sys.argv[3] if len(sys.argv) > 3 else "native"
    
    success = build_embodios(model_path, output_dir, target, verbose=True)
    sys.exit(0 if success else 1)