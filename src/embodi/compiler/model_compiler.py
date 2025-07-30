"""
Model Compiler - Compiles AI models for EMBODIOS using TVM and custom techniques
"""

import struct
from pathlib import Path
from typing import Dict, List, Tuple, Any, Optional
import json
import mmap
import logging

# NumPy - optional
try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False
    # Minimal compatibility layer
    class ndarray:
        def __init__(self, data):
            self.data = data
            self.shape = (len(data),)
            self.size = len(data)
            self.dtype = float
            self.flat = data
            
    class np:
        ndarray = ndarray
        
        @staticmethod
        def zeros(shape, dtype=None):
            if isinstance(shape, tuple):
                size = 1
                for dim in shape:
                    size *= dim
                return [0.0] * size
            return [0.0] * shape
            
        @staticmethod
        def frombuffer(data, dtype=None):
            return list(data)
            
        @staticmethod
        def prod(dims):
            result = 1
            for d in dims:
                result *= d
            return result
            
        float32 = float
        float16 = float
        int8 = int
        uint8 = int
        uint16 = int

try:
    from .tvm_compiler import TVMModelCompiler, HAS_TVM
except ImportError:
    from tvm_compiler import TVMModelCompiler, HAS_TVM

class ModelToNativeCompiler:
    """
    Compiles model weights directly into kernel binary
    Uses TVM when available, falls back to custom compilation
    """
    
    def __init__(self, output_dir: str = "build/native"):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.tvm_compiler = TVMModelCompiler(output_dir)
        self.logger = logging.getLogger(__name__)
        
    def compile_model(self, model_path: str, architecture: str = "native") -> Dict[str, str]:
        """
        Transform model to native code
        
        Args:
            model_path: Path to model file (GGUF/SafeTensors/ONNX)
            architecture: Target architecture
            
        Returns:
            Dictionary with generated files
        """
        model_path = Path(model_path)
        if not model_path.exists():
            raise FileNotFoundError(f"Model not found: {model_path}")
        
        # Try TVM first if available
        if HAS_TVM:
            try:
                return self.tvm_compiler.compile_model(str(model_path), architecture)
            except Exception as e:
                self.logger.warning(f"TVM compilation failed, using custom: {e}")
        
        # Custom compilation fallback
        if model_path.suffix == '.gguf':
            weights, metadata = self.parse_gguf(model_path)
        elif model_path.suffix in ['.safetensors', '.st']:
            weights, metadata = self.parse_safetensors(model_path)
        else:
            raise ValueError(f"Unsupported model format: {model_path.suffix}")
            
        # Generate assembly with embedded weights
        asm_file = self.generate_weight_assembly(weights, metadata)
        
        # Generate optimized inference code
        c_files = self.generate_inference_code(weights, metadata, architecture)
        
        return {
            'asm_files': [str(asm_file)],
            'c_files': c_files,
            'metadata': metadata
        }
        
    def parse_gguf(self, model_path: Path) -> Tuple[Dict[str, Any], Dict]:
        """Parse GGUF format and extract weights"""
        weights = {}
        metadata = {}
        
        with open(model_path, 'rb') as f:
            # Read GGUF header
            magic = f.read(4)
            if magic != b'GGUF':
                raise ValueError("Invalid GGUF file")
                
            version = struct.unpack('<I', f.read(4))[0]
            metadata['version'] = version
            
            # Read tensor count and metadata
            tensor_count = struct.unpack('<Q', f.read(8))[0]
            metadata_kv_count = struct.unpack('<Q', f.read(8))[0]
            
            metadata['tensor_count'] = tensor_count
            
            # Parse metadata key-values
            for _ in range(metadata_kv_count):
                key_length = struct.unpack('<Q', f.read(8))[0]
                key = f.read(key_length).decode('utf-8')
                value_type = struct.unpack('<I', f.read(4))[0]
                value = self._read_gguf_value(f, value_type)
                metadata[key] = value
                
            # Parse tensor info
            tensor_infos = []
            for _ in range(tensor_count):
                name_length = struct.unpack('<Q', f.read(8))[0]
                name = f.read(name_length).decode('utf-8')
                
                n_dims = struct.unpack('<I', f.read(4))[0]
                dims = [struct.unpack('<Q', f.read(8))[0] for _ in range(n_dims)]
                
                tensor_type = struct.unpack('<I', f.read(4))[0]
                offset = struct.unpack('<Q', f.read(8))[0]
                
                tensor_infos.append({
                    'name': name,
                    'dims': dims,
                    'type': tensor_type,
                    'offset': offset
                })
                
            # Memory map the file for efficient weight reading
            with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mmapped_file:
                for info in tensor_infos:
                    tensor_data = self._read_tensor_data(
                        mmapped_file,
                        info['offset'],
                        info['dims'],
                        info['type']
                    )
                    weights[info['name']] = tensor_data
                    
        return weights, metadata
    
    def parse_safetensors(self, model_path: Path) -> Tuple[Dict[str, np.ndarray], Dict]:
        """Parse SafeTensors format"""
        try:
            from safetensors import safe_open
        except ImportError:
            raise ImportError("safetensors package required for .safetensors files")
            
        weights = {}
        metadata = {}
        
        with safe_open(model_path, framework="np") as f:
            metadata = f.metadata() or {}
            for key in f.keys():
                weights[key] = f.get_tensor(key)
                
        return weights, metadata
    
    def generate_weight_assembly(self, weights: Dict[str, Any], metadata: Dict) -> Path:
        """Create weights.S with all model data"""
        asm_path = self.output_dir / "weights.S"
        
        with open(asm_path, 'w') as f:
            # Write header
            f.write("/* Generated by EMBODIOS Model Compiler */\n")
            f.write(f"/* Model: {metadata.get('name', 'unknown')} */\n")
            f.write(f"/* Total tensors: {len(weights)} */\n\n")
            
            f.write(".section .model_weights, \"aw\"\n")
            f.write(".align 8\n\n")
            
            # Write metadata section
            f.write(".global model_metadata\n")
            f.write("model_metadata:\n")
            f.write(f"    .quad {len(weights)}  /* tensor_count */\n")
            f.write(f"    .quad tensor_info_table\n\n")
            
            # Write tensor info table
            f.write(".global tensor_info_table\n")
            f.write("tensor_info_table:\n")
            
            offset = 0
            tensor_offsets = {}
            
            for name, tensor in weights.items():
                safe_name = name.replace('.', '_').replace('/', '_')
                tensor_offsets[name] = offset
                
                # Get size and shape based on tensor type
                if hasattr(tensor, 'size'):
                    tensor_size = tensor.size
                elif isinstance(tensor, list):
                    tensor_size = len(tensor)
                else:
                    tensor_size = 1
                    
                if hasattr(tensor, 'shape'):
                    tensor_shape = tensor.shape
                    shape_len = len(tensor_shape)
                else:
                    tensor_shape = [tensor_size]
                    shape_len = 1
                
                f.write(f"    /* {name} */\n")
                f.write(f"    .quad {safe_name}_data  /* pointer */\n")
                f.write(f"    .quad {tensor_size}     /* total elements */\n")
                f.write(f"    .quad {shape_len}  /* dimensions */\n")
                
                # Write shape
                for dim in tensor_shape:
                    f.write(f"    .quad {dim}\n")
                    
                # Pad to 8 dimensions
                for _ in range(8 - shape_len):
                    f.write("    .quad 0\n")
                    
                # Calculate offset
                if hasattr(tensor, 'nbytes'):
                    offset += tensor.nbytes
                else:
                    # Assume float32 (4 bytes per element)
                    offset += tensor_size * 4
                
            # Write actual tensor data
            f.write("\n/* Tensor data */\n")
            for name, tensor in weights.items():
                safe_name = name.replace('.', '_').replace('/', '_')
                
                f.write(f"\n.global {safe_name}_data\n")
                f.write(f".align 8\n")
                f.write(f"{safe_name}_data:\n")
                
                # Write tensor data based on type
                if HAS_NUMPY and hasattr(tensor, 'dtype'):
                    # NumPy array handling
                    if tensor.dtype == np.float32:
                        # Write as 32-bit floats
                        for i in range(0, len(tensor.flat), 4):
                            values = tensor.flat[i:i+4]
                            f.write("    .float ")
                            f.write(", ".join(f"{v:.6e}" for v in values))
                            f.write("\n")
                            
                    elif tensor.dtype == np.float16:
                        # Write as 16-bit floats
                        for i in range(0, len(tensor.flat), 8):
                            values = tensor.flat[i:i+8]
                            f.write("    .short ")
                            f.write(", ".join(f"0x{v.view(np.uint16):04x}" for v in values))
                            f.write("\n")
                            
                    elif tensor.dtype in [np.int8, np.uint8]:
                        # Write as bytes
                        for i in range(0, len(tensor.flat), 16):
                            values = tensor.flat[i:i+16]
                            f.write("    .byte ")
                            f.write(", ".join(f"{int(v)}" for v in values))
                            f.write("\n")
                else:
                    # Fallback for lists or custom arrays
                    flat_data = tensor.flat if hasattr(tensor, 'flat') else tensor
                    if isinstance(flat_data, list) or hasattr(flat_data, '__iter__'):
                        # Write as floats
                        flat_list = list(flat_data)
                        for i in range(0, len(flat_list), 4):
                            values = flat_list[i:i+4]
                            f.write("    .float ")
                            f.write(", ".join(f"{float(v):.6e}" for v in values))
                            f.write("\n")
                        
        return asm_path
    
    def generate_inference_code(self, weights: Dict[str, np.ndarray], 
                              metadata: Dict, architecture: str) -> List[str]:
        """Generate optimized inference code based on model architecture"""
        c_files = []
        
        # Detect model type from weights
        model_type = self._detect_model_type(weights)
        
        # Generate layer-specific implementations
        if model_type == "transformer":
            c_files.extend(self._generate_transformer_code(weights, metadata, architecture))
        elif model_type == "cnn":
            c_files.extend(self._generate_cnn_code(weights, metadata, architecture))
        else:
            # Generic implementation
            c_files.append(self._generate_generic_inference(weights, metadata, architecture))
            
        return c_files
    
    def _detect_model_type(self, weights: Dict[str, np.ndarray]) -> str:
        """Detect model type from weight names"""
        weight_names = set(weights.keys())
        
        # Check for transformer-specific layers
        transformer_indicators = ['attention', 'transformer', 'q_proj', 'k_proj', 'v_proj', 
                                'mlp', 'feed_forward', 'layer_norm']
        if any(indicator in name.lower() for name in weight_names for indicator in transformer_indicators):
            return "transformer"
            
        # Check for CNN layers
        cnn_indicators = ['conv', 'pool', 'batch_norm']
        if any(indicator in name.lower() for name in weight_names for indicator in cnn_indicators):
            return "cnn"
            
        return "unknown"
            
    def _generate_transformer_code(self, weights: Dict, metadata: Dict, arch: str) -> List[str]:
        """Generate transformer-specific inference code"""
        files = []
        
        # Generate attention implementation
        attention_code = self._generate_attention_code(weights, arch)
        attention_path = self.output_dir / "attention_native.c"
        attention_path.write_text(attention_code)
        files.append(str(attention_path))
        
        # Generate FFN implementation
        ffn_code = self._generate_ffn_code(weights, arch)
        ffn_path = self.output_dir / "ffn_native.c"
        ffn_path.write_text(ffn_code)
        files.append(str(ffn_path))
        
        # Generate main transformer code
        transformer_code = self._generate_transformer_main(weights, metadata, arch)
        transformer_path = self.output_dir / "transformer_native.c"
        transformer_path.write_text(transformer_code)
        files.append(str(transformer_path))
        
        return files
    
    def _generate_cnn_code(self, weights: Dict, metadata: Dict, arch: str) -> List[str]:
        """Generate CNN-specific inference code"""
        cnn_code = """/* CNN Inference Implementation */
#include <stdint.h>
#include <string.h>

void conv2d_forward(const float* input, const float* weight, const float* bias,
                   float* output, int in_c, int out_c, int h, int w,
                   int kernel_size, int stride, int padding) {
    /* Convolution implementation */
    /* TODO: Add optimized convolution */
}

void maxpool2d_forward(const float* input, float* output,
                      int channels, int h, int w, int pool_size) {
    /* Max pooling implementation */
}
"""
        cnn_path = self.output_dir / "cnn_native.c"
        cnn_path.write_text(cnn_code)
        return [str(cnn_path)]
    
    def _generate_generic_inference(self, weights: Dict, metadata: Dict, arch: str) -> str:
        """Generate generic inference code"""
        generic_code = """/* Generic Model Inference */
#include <stdint.h>
#include <string.h>
#include "embodios.h"

extern const float* tensor_data[];
extern const size_t tensor_sizes[];

int generic_inference(const float* input, float* output, size_t input_size) {
    /* Generic forward pass */
    /* This would implement the specific model architecture */
    
    /* For now, just copy input to output */
    memcpy(output, input, input_size * sizeof(float));
    
    return 0;
}
"""
        generic_path = self.output_dir / "inference_generic.c"
        generic_path.write_text(generic_code)
        return str(generic_path)
    
    def _read_gguf_value(self, f, value_type: int) -> Any:
        """Read GGUF metadata value based on type"""
        # GGUF value types
        if value_type == 0:  # UINT8
            return struct.unpack('B', f.read(1))[0]
        elif value_type == 1:  # INT8
            return struct.unpack('b', f.read(1))[0]
        elif value_type == 2:  # UINT16
            return struct.unpack('<H', f.read(2))[0]
        elif value_type == 3:  # INT16
            return struct.unpack('<h', f.read(2))[0]
        elif value_type == 4:  # UINT32
            return struct.unpack('<I', f.read(4))[0]
        elif value_type == 5:  # INT32
            return struct.unpack('<i', f.read(4))[0]
        elif value_type == 6:  # FLOAT32
            return struct.unpack('<f', f.read(4))[0]
        elif value_type == 7:  # BOOL
            return bool(struct.unpack('B', f.read(1))[0])
        elif value_type == 8:  # STRING
            length = struct.unpack('<Q', f.read(8))[0]
            return f.read(length).decode('utf-8')
        elif value_type == 9:  # ARRAY
            array_type = struct.unpack('<I', f.read(4))[0]
            length = struct.unpack('<Q', f.read(8))[0]
            return [self._read_gguf_value(f, array_type) for _ in range(length)]
        else:
            raise ValueError(f"Unknown GGUF value type: {value_type}")
            
    def _read_tensor_data(self, mmap_file: mmap.mmap, offset: int, 
                         dims: List[int], tensor_type: int) -> np.ndarray:
        """Read tensor data from memory-mapped file"""
        # GGUF tensor types
        type_map = {
            0: np.float32,
            1: np.float16,
            2: np.int8,
            3: np.uint8,
            4: np.int16,
            5: np.uint16,
            6: np.int32,
            7: np.uint32,
            8: np.int64,
            9: np.uint64,
        }
        
        dtype = type_map.get(tensor_type, np.float32)
        size = np.prod(dims) * dtype().itemsize
        
        # Read data
        mmap_file.seek(offset)
        data = mmap_file.read(size)
        
        # Convert to numpy array
        array = np.frombuffer(data, dtype=dtype)
        return array.reshape(dims)
    
    def _generate_attention_code(self, weights: Dict, arch: str) -> str:
        """Generate optimized attention implementation"""
        use_avx = 'avx' in arch.lower()
        
        return f"""/* Optimized Attention Implementation */
#include <stdint.h>
#include <math.h>
{'#include <immintrin.h>' if use_avx else ''}

void attention_forward(const float* q, const float* k, const float* v,
                      float* output, int seq_len, int d_model, int n_heads) {{
    const int d_k = d_model / n_heads;
    const float scale = 1.0f / sqrtf((float)d_k);
    
    /* Compute attention scores */
    float* scores = (float*)malloc(seq_len * seq_len * sizeof(float));
    
    {'/* AVX-optimized implementation */' if use_avx else '/* Standard implementation */'}
    for (int i = 0; i < seq_len; i++) {{
        for (int j = 0; j < seq_len; j++) {{
            float score = 0.0f;
            {'/* TODO: Add AVX implementation */' if use_avx else ''}
            for (int k = 0; k < d_model; k++) {{
                score += q[i * d_model + k] * k[j * d_model + k];
            }}
            scores[i * seq_len + j] = score * scale;
        }}
    }}
    
    /* Softmax */
    for (int i = 0; i < seq_len; i++) {{
        float max_score = scores[i * seq_len];
        for (int j = 1; j < seq_len; j++) {{
            if (scores[i * seq_len + j] > max_score) {{
                max_score = scores[i * seq_len + j];
            }}
        }}
        
        float sum = 0.0f;
        for (int j = 0; j < seq_len; j++) {{
            scores[i * seq_len + j] = expf(scores[i * seq_len + j] - max_score);
            sum += scores[i * seq_len + j];
        }}
        
        for (int j = 0; j < seq_len; j++) {{
            scores[i * seq_len + j] /= sum;
        }}
    }}
    
    /* Apply attention to values */
    for (int i = 0; i < seq_len; i++) {{
        for (int j = 0; j < d_model; j++) {{
            float sum = 0.0f;
            for (int k = 0; k < seq_len; k++) {{
                sum += scores[i * seq_len + k] * v[k * d_model + j];
            }}
            output[i * d_model + j] = sum;
        }}
    }}
    
    free(scores);
}}
"""
    
    def _generate_ffn_code(self, weights: Dict, arch: str) -> str:
        """Generate optimized FFN implementation"""
        return """/* Optimized Feed-Forward Network */
#include <stdint.h>
#include <math.h>

/* GELU activation */
static inline float gelu(float x) {
    return 0.5f * x * (1.0f + tanhf(0.7978845608f * (x + 0.044715f * x * x * x)));
}

void ffn_forward(const float* input, const float* w1, const float* w2,
                const float* b1, const float* b2,
                float* output, int seq_len, int d_model, int d_ff) {
    /* Temporary buffer for intermediate activations */
    float* hidden = (float*)malloc(seq_len * d_ff * sizeof(float));
    
    /* First linear layer */
    for (int i = 0; i < seq_len; i++) {
        for (int j = 0; j < d_ff; j++) {
            float sum = b1 ? b1[j] : 0.0f;
            for (int k = 0; k < d_model; k++) {
                sum += input[i * d_model + k] * w1[k * d_ff + j];
            }
            hidden[i * d_ff + j] = gelu(sum);
        }
    }
    
    /* Second linear layer */
    for (int i = 0; i < seq_len; i++) {
        for (int j = 0; j < d_model; j++) {
            float sum = b2 ? b2[j] : 0.0f;
            for (int k = 0; k < d_ff; k++) {
                sum += hidden[i * d_ff + k] * w2[k * d_model + j];
            }
            output[i * d_model + j] = sum;
        }
    }
    
    free(hidden);
}
"""
    
    def _generate_transformer_main(self, weights: Dict, metadata: Dict, arch: str) -> str:
        """Generate main transformer inference code"""
        num_layers = metadata.get('num_layers', 12)
        hidden_size = metadata.get('hidden_size', 768)
        
        return f"""/* Transformer Model Main Inference */
#include <stdint.h>
#include <string.h>
#include "embodios.h"

/* Model configuration */
#define NUM_LAYERS {num_layers}
#define HIDDEN_SIZE {hidden_size}
#define MAX_SEQ_LEN 2048

/* External functions */
extern void attention_forward(const float* q, const float* k, const float* v,
                            float* output, int seq_len, int d_model, int n_heads);
extern void ffn_forward(const float* input, const float* w1, const float* w2,
                       const float* b1, const float* b2,
                       float* output, int seq_len, int d_model, int d_ff);

/* Layer normalization */
void layer_norm(const float* input, const float* gamma, const float* beta,
               float* output, int size) {{
    float mean = 0.0f, variance = 0.0f;
    
    /* Compute mean */
    for (int i = 0; i < size; i++) {{
        mean += input[i];
    }}
    mean /= size;
    
    /* Compute variance */
    for (int i = 0; i < size; i++) {{
        float diff = input[i] - mean;
        variance += diff * diff;
    }}
    variance /= size;
    
    /* Normalize */
    float std_inv = 1.0f / sqrtf(variance + 1e-5f);
    for (int i = 0; i < size; i++) {{
        output[i] = gamma[i] * (input[i] - mean) * std_inv + beta[i];
    }}
}}

/* Main transformer forward pass */
int transformer_forward(const int32_t* input_ids, size_t seq_len,
                       float* output_logits) {{
    /* Allocate buffers */
    float* hidden = (float*)malloc(seq_len * HIDDEN_SIZE * sizeof(float));
    float* temp = (float*)malloc(seq_len * HIDDEN_SIZE * sizeof(float));
    
    /* Token embedding */
    /* TODO: Implement embedding lookup */
    
    /* Process through transformer layers */
    for (int layer = 0; layer < NUM_LAYERS; layer++) {{
        /* Self-attention */
        /* TODO: Load attention weights for this layer */
        
        /* FFN */
        /* TODO: Load FFN weights for this layer */
        
        /* Layer norm */
        /* TODO: Apply layer normalization */
    }}
    
    /* Output projection */
    /* TODO: Project to vocabulary size */
    
    free(hidden);
    free(temp);
    
    return 0;
}}
"""