#!/usr/bin/env python3
"""
HuggingFace Model Integration for EMBODIOS
Downloads and converts models directly from HuggingFace
"""

import os
import json
import hashlib
import tempfile
from pathlib import Path
from typing import Optional, Dict, Tuple

try:
    from huggingface_hub import snapshot_download, hf_hub_download, model_info
    from transformers import AutoConfig
    HF_AVAILABLE = True
except ImportError:
    HF_AVAILABLE = False

class HuggingFaceDownloader:
    def __init__(self):
        self.cache_dir = Path.home() / '.embodi' / 'models' / 'cache'
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        
    def download_and_convert(self, model_id: str, output_path: Path, 
                           quantization: Optional[int] = None,
                           auth_token: Optional[str] = None) -> bool:
        """Download model from HuggingFace and convert to EMBODIOS format"""
        
        if not HF_AVAILABLE:
            print("Error: huggingface_hub not installed")
            print("Install with: pip install huggingface-hub transformers")
            return False
            
        print(f"Downloading {model_id} from HuggingFace...")
        
        try:
            # Get model info
            info = model_info(model_id, token=auth_token)
            
            # Check model size
            model_size = self._estimate_model_size(info)
            print(f"Model size: ~{model_size / 1024 / 1024 / 1024:.1f}GB")
            
            # Download to cache
            with tempfile.TemporaryDirectory() as temp_dir:
                print("Downloading model files...")
                
                # Download model snapshot
                local_path = snapshot_download(
                    repo_id=model_id,
                    cache_dir=self.cache_dir,
                    token=auth_token,
                    local_dir=temp_dir
                )
                
                # Load config
                config_path = Path(local_path) / 'config.json'
                if config_path.exists():
                    with open(config_path) as f:
                        config = json.load(f)
                else:
                    # Try to load with transformers
                    config = AutoConfig.from_pretrained(model_id).to_dict()
                
                # Convert to EMBODIOS format
                print("Converting to EMBODIOS format...")
                return self._convert_to_embodi(
                    local_path, 
                    output_path, 
                    config, 
                    quantization
                )
                
        except Exception as e:
            print(f"Error downloading model: {e}")
            return False
            
    def _estimate_model_size(self, info) -> int:
        """Estimate model size from repo info"""
        total_size = 0
        
        for sibling in info.siblings:
            if sibling.rfilename.endswith(('.bin', '.safetensors', '.gguf')):
                total_size += sibling.size or 0
                
        return total_size
        
    def _convert_to_embodi(self, model_path: str, output_path: Path, 
                        config: Dict, quantization: Optional[int]) -> bool:
        """Convert downloaded model to EMBODIOS format"""
        
        # Determine model format
        model_files = list(Path(model_path).glob('*.safetensors'))
        if not model_files:
            model_files = list(Path(model_path).glob('*.bin'))
        if not model_files:
            model_files = list(Path(model_path).glob('*.gguf'))
            
        if not model_files:
            print("No model files found")
            return False
            
        # Use the first model file
        model_file = model_files[0]
        print(f"Converting {model_file.name}...")
        
        # Import converter
        from embodi.builder.converter import ModelConverter
        
        converter = ModelConverter()
        
        # Enhance config with HuggingFace metadata
        enhanced_config = {
            'source': 'huggingface',
            'model_id': config.get('_name_or_path', 'unknown'),
            'architecture': config.get('architectures', ['unknown'])[0],
            'hidden_size': config.get('hidden_size', config.get('n_embd', 4096)),
            'num_layers': config.get('num_hidden_layers', config.get('n_layer', 32)),
            'num_heads': config.get('num_attention_heads', config.get('n_head', 32)),
            'vocab_size': config.get('vocab_size', 32000),
            'max_length': config.get('max_position_embeddings', config.get('n_positions', 2048)),
            'quantization': quantization
        }
        
        # Set optimal quantization based on model size
        if quantization is None:
            model_size_gb = model_file.stat().st_size / 1024 / 1024 / 1024
            if model_size_gb > 10:
                quantization = 4  # 4-bit for large models
                print("Auto-selected 4-bit quantization for large model")
            elif model_size_gb > 3:
                quantization = 8  # 8-bit for medium models
                print("Auto-selected 8-bit quantization for medium model")
                
        # Convert based on format
        if model_file.suffix == '.safetensors':
            return converter.convert_safetensors(model_file, output_path, quantization)
        elif model_file.suffix == '.gguf':
            return converter.convert_gguf(model_file, output_path, quantization)
        elif model_file.suffix == '.bin':
            return converter.convert_pytorch(model_file, output_path, quantization)
        else:
            print(f"Unsupported format: {model_file.suffix}")
            return False

class ModelCache:
    """Cache for downloaded models"""
    
    def __init__(self):
        self.cache_dir = Path.home() / '.embodi' / 'models' / 'cache'
        self.cache_index = self.cache_dir / 'index.json'
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self._load_index()
        
    def _load_index(self):
        """Load cache index"""
        if self.cache_index.exists():
            with open(self.cache_index) as f:
                self.index = json.load(f)
        else:
            self.index = {}
            
    def _save_index(self):
        """Save cache index"""
        with open(self.cache_index, 'w') as f:
            json.dump(self.index, f, indent=2)
            
    def get_cached(self, model_id: str, quantization: Optional[int]) -> Optional[Path]:
        """Get cached model if available"""
        cache_key = f"{model_id}_{quantization or 'fp32'}"
        
        if cache_key in self.index:
            path = Path(self.index[cache_key]['path'])
            if path.exists():
                print(f"Using cached model: {path}")
                return path
                
        return None
        
    def add_to_cache(self, model_id: str, quantization: Optional[int], 
                     path: Path, metadata: Dict):
        """Add model to cache"""
        cache_key = f"{model_id}_{quantization or 'fp32'}"
        
        self.index[cache_key] = {
            'path': str(path),
            'model_id': model_id,
            'quantization': quantization,
            'size': path.stat().st_size,
            'hash': self._calculate_hash(path),
            'metadata': metadata
        }
        
        self._save_index()
        
    def _calculate_hash(self, path: Path) -> str:
        """Calculate file hash"""
        sha256 = hashlib.sha256()
        with open(path, 'rb') as f:
            for chunk in iter(lambda: f.read(8192), b''):
                sha256.update(chunk)
        return sha256.hexdigest()[:16]
        
    def list_cached(self):
        """List all cached models"""
        models = []
        
        for key, info in self.index.items():
            models.append({
                'model_id': info['model_id'],
                'quantization': info['quantization'],
                'size': info['size'],
                'path': info['path']
            })
            
        return models
        
    def clear_cache(self):
        """Clear model cache"""
        import shutil
        
        print("Clearing model cache...")
        
        # Remove cached files
        for info in self.index.values():
            path = Path(info['path'])
            if path.exists():
                path.unlink()
                
        # Clear index
        self.index = {}
        self._save_index()
        
        # Remove cache directory contents
        if self.cache_dir.exists():
            shutil.rmtree(self.cache_dir)
            self.cache_dir.mkdir(parents=True, exist_ok=True)
            
        print("Cache cleared")

def pull_model(model_id: str, quantization: Optional[int] = None, 
               force: bool = False) -> Optional[Path]:
    """Pull model from HuggingFace (main entry point)"""
    
    # Parse model ID
    if model_id.startswith('huggingface:'):
        model_id = model_id[len('huggingface:'):]
        
    # Check cache first
    cache = ModelCache()
    
    if not force:
        cached_path = cache.get_cached(model_id, quantization)
        if cached_path:
            return cached_path
            
    # Download and convert
    downloader = HuggingFaceDownloader()
    
    # Output path
    models_dir = Path.home() / '.embodi' / 'models'
    models_dir.mkdir(parents=True, exist_ok=True)
    
    safe_name = model_id.replace('/', '-')
    quant_suffix = f"-{quantization}bit" if quantization else ""
    output_path = models_dir / f"{safe_name}{quant_suffix}.aios"
    
    success = downloader.download_and_convert(
        model_id, 
        output_path,
        quantization
    )
    
    if success:
        # Add to cache
        cache.add_to_cache(model_id, quantization, output_path, {
            'source': 'huggingface',
            'downloaded': True
        })
        return output_path
    else:
        return None