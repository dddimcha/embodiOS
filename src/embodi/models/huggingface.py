#!/usr/bin/env python3
"""
HuggingFace Model Integration for EMBODIOS
Downloads and converts models directly from HuggingFace
"""

import os
import json
import hashlib
import tempfile
import time
from pathlib import Path
from typing import Optional, Dict, Tuple, Callable

try:
    from huggingface_hub import snapshot_download, hf_hub_download, model_info
    from transformers import AutoConfig
    HF_AVAILABLE = True
except ImportError:
    HF_AVAILABLE = False


class ProgressTracker:
    """Custom progress tracker for HuggingFace downloads

    This class provides a tqdm-compatible interface for progress tracking
    while calling a custom callback function with download metrics.
    """

    def __init__(self, iterable=None, desc=None, total=None, unit='B',
                 unit_scale=True, unit_divisor=1024, callback: Optional[Callable] = None,
                 **kwargs):
        """
        Initialize progress tracker (tqdm-compatible signature)

        Args:
            iterable: Iterable to decorate (for tqdm compatibility)
            desc: Description prefix
            total: Total number of expected iterations
            unit: Unit of iteration
            unit_scale: Whether to scale units
            unit_divisor: Divisor for unit scaling
            callback: Optional callback function(bytes_downloaded, total_bytes, speed, eta)
            **kwargs: Additional tqdm-compatible arguments
        """
        self.callback = callback
        self.start_time = time.time()
        self.bytes_downloaded = 0
        self.total_bytes = total or 0
        self.desc = desc or ""
        self.n = 0
        self.iterable = iterable

    def __iter__(self):
        """Iterate over iterable with progress tracking"""
        if self.iterable is not None:
            for obj in self.iterable:
                yield obj
                self.update()

    def __enter__(self):
        """Context manager entry"""
        return self

    def __exit__(self, *args):
        """Context manager exit"""
        self.close()

    def update(self, n: int = 1):
        """Update progress by n bytes"""
        self.n += n
        self.bytes_downloaded += n
        self._report_progress()

    def set_description(self, desc: str, refresh: bool = True):
        """Set description (for tqdm compatibility)"""
        self.desc = desc

    def set_postfix(self, ordered_dict=None, refresh=True, **kwargs):
        """Set postfix (for tqdm compatibility)"""
        pass

    def close(self):
        """Close progress tracker"""
        pass

    def refresh(self):
        """Refresh progress display (for tqdm compatibility)"""
        pass

    def _report_progress(self):
        """Report progress via callback"""
        if not self.callback:
            return

        elapsed = time.time() - self.start_time if self.start_time else 0

        # Calculate speed (bytes per second)
        speed = self.bytes_downloaded / elapsed if elapsed > 0 else 0

        # Calculate ETA (seconds)
        remaining_bytes = self.total_bytes - self.bytes_downloaded
        eta = remaining_bytes / speed if speed > 0 else 0

        try:
            self.callback(self.bytes_downloaded, self.total_bytes, speed, eta)
        except Exception:
            # Don't let callback errors break the download
            pass


class HuggingFaceDownloader:
    def __init__(self):
        self.cache_dir = Path.home() / '.embodi' / 'models' / 'cache'
        self.cache_dir.mkdir(parents=True, exist_ok=True)

    def _calculate_hash(self, file_path: Path, algorithm: str = 'sha256') -> str:
        """
        Calculate hash of a file

        Args:
            file_path: Path to file
            algorithm: Hash algorithm (default: sha256)

        Returns:
            Hex string of file hash
        """
        hash_obj = hashlib.new(algorithm)

        with open(file_path, 'rb') as f:
            # Read in chunks for memory efficiency
            for chunk in iter(lambda: f.read(8192), b''):
                hash_obj.update(chunk)

        return hash_obj.hexdigest()

    def _verify_checksum(self, file_path: Path, expected_checksum: str) -> bool:
        """
        Verify SHA256 checksum of a file

        Args:
            file_path: Path to file to verify
            expected_checksum: Expected SHA256 hash (hex string)

        Returns:
            True if checksum matches, False otherwise
        """
        if not file_path.exists():
            return False

        actual_checksum = self._calculate_hash(file_path)
        return actual_checksum.lower() == expected_checksum.lower()

    def _get_file_checksum(self, model_id: str, filename: str,
                           auth_token: Optional[str] = None) -> Optional[str]:
        """
        Get checksum for a file from HuggingFace metadata

        Args:
            model_id: HuggingFace model repository ID
            filename: Name of file in repository
            auth_token: Optional HuggingFace authentication token

        Returns:
            SHA256 checksum from metadata, or None if not available
        """
        if not HF_AVAILABLE:
            return None

        try:
            # Get model info
            info = model_info(model_id, token=auth_token)

            # Find the file in siblings
            for sibling in info.siblings:
                if sibling.rfilename == filename:
                    # HuggingFace provides lfs.sha256 in metadata
                    if hasattr(sibling, 'lfs') and sibling.lfs:
                        return sibling.lfs.get('sha256')
                    # Some files have sha256 directly
                    if hasattr(sibling, 'sha256'):
                        return sibling.sha256

            return None

        except Exception as e:
            print(f"Warning: Could not get checksum from metadata: {e}")
            return None
        
    def download_and_convert(self, model_id: str, output_path: Path,
                           quantization: Optional[int] = None,
                           auth_token: Optional[str] = None,
                           progress_callback: Optional[Callable] = None) -> bool:
        """Download model from HuggingFace and convert to EMBODIOS format

        Args:
            model_id: HuggingFace model repository ID
            output_path: Path where to save the converted model
            quantization: Optional quantization level (4, 8, etc.)
            auth_token: Optional HuggingFace authentication token
            progress_callback: Optional callback function(bytes_downloaded, total_bytes, speed, eta)

        Returns:
            True if download and conversion successful, False otherwise
        """

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

            # Check for direct GGUF files first
            gguf_files = self._detect_gguf_files(model_id, quantization, auth_token)

            if gguf_files:
                print(f"Found {len(gguf_files)} GGUF file(s) in repository")
                return self._download_gguf_direct(
                    model_id,
                    gguf_files,
                    output_path,
                    auth_token,
                    progress_callback
                )

            # Fallback to snapshot download and conversion
            print("No GGUF files found, downloading full model for conversion...")

            # Download to cache
            with tempfile.TemporaryDirectory() as temp_dir:
                print("Downloading model files...")

                # Create progress tracker if callback provided
                tqdm_class = None
                if progress_callback:
                    # Create a factory function that captures the callback
                    def make_progress(*args, **kwargs):
                        return ProgressTracker(*args, callback=progress_callback, **kwargs)
                    tqdm_class = make_progress

                # Download model snapshot
                local_path = snapshot_download(
                    repo_id=model_id,
                    cache_dir=self.cache_dir,
                    token=auth_token,
                    local_dir=temp_dir,
                    tqdm_class=tqdm_class
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

    def _detect_gguf_files(self, model_id: str, quantization: Optional[int] = None,
                          auth_token: Optional[str] = None) -> list:
        """Detect available GGUF files in HuggingFace repository

        Args:
            model_id: HuggingFace model repository ID
            quantization: Optional quantization level to match (4, 8, etc.)
            auth_token: Optional HuggingFace authentication token

        Returns:
            List of dicts with GGUF file info: {'filename': str, 'size': int, 'quantization': str, 'checksum': str}
        """
        if not HF_AVAILABLE:
            return []

        try:
            # Get model info
            info = model_info(model_id, token=auth_token)

            gguf_files = []

            # Scan siblings for GGUF files
            for sibling in info.siblings:
                if sibling.rfilename.endswith('.gguf'):
                    # Extract quantization info from filename
                    # Common patterns: Q4_K_M, Q4_0, Q8_0, etc.
                    filename = sibling.rfilename
                    quant_info = self._extract_quantization_info(filename)

                    # Extract checksum from metadata
                    checksum = None
                    if hasattr(sibling, 'lfs') and sibling.lfs:
                        checksum = sibling.lfs.get('sha256')
                    elif hasattr(sibling, 'sha256'):
                        checksum = sibling.sha256

                    file_info = {
                        'filename': filename,
                        'size': sibling.size or 0,
                        'quantization': quant_info,
                        'checksum': checksum
                    }

                    # Filter by quantization if specified
                    if quantization is None:
                        gguf_files.append(file_info)
                    elif quant_info and str(quantization) in quant_info:
                        gguf_files.append(file_info)

            return gguf_files

        except Exception as e:
            print(f"Warning: Could not detect GGUF files: {e}")
            return []

    def _extract_quantization_info(self, filename: str) -> Optional[str]:
        """Extract quantization level from GGUF filename

        Args:
            filename: GGUF filename (e.g., 'model.Q4_K_M.gguf')

        Returns:
            Quantization string (e.g., 'Q4_K_M', 'Q8_0') or None
        """
        import re

        # Match common quantization patterns: Q4_K_M, Q4_0, Q8_0, Q5_K_S, etc.
        patterns = [
            r'[._-](Q\d+_K_[MLS])',  # Q4_K_M, Q5_K_S, Q5_K_L
            r'[._-](Q\d+_\d+)',       # Q4_0, Q8_0
            r'[._-](Q\d+)',           # Q4, Q8
        ]

        for pattern in patterns:
            match = re.search(pattern, filename, re.IGNORECASE)
            if match:
                return match.group(1).upper()

        return None

    def _download_gguf_direct(self, model_id: str, gguf_files: list,
                              output_path: Path, auth_token: Optional[str] = None,
                              progress_callback: Optional[Callable] = None) -> bool:
        """Download GGUF file directly from HuggingFace repository

        Args:
            model_id: HuggingFace model repository ID
            gguf_files: List of GGUF file info dicts from _detect_gguf_files
            output_path: Path where to save the downloaded GGUF file
            auth_token: Optional HuggingFace authentication token
            progress_callback: Optional callback function(bytes_downloaded, total_bytes, speed, eta)

        Returns:
            True if download successful, False otherwise
        """
        if not gguf_files:
            return False

        # Select best GGUF file
        # Prefer Q4_K_M or Q4_0 for good balance of size/quality
        # Then Q5, Q6, Q8 in order of preference
        selected_file = None
        preference_order = ['Q4_K_M', 'Q4_0', 'Q5_K_M', 'Q5_0', 'Q6_K', 'Q8_0']

        for preferred_quant in preference_order:
            for gguf_file in gguf_files:
                if gguf_file['quantization'] == preferred_quant:
                    selected_file = gguf_file
                    break
            if selected_file:
                break

        # If no preferred quantization found, use the first file
        if not selected_file:
            selected_file = gguf_files[0]

        filename = selected_file['filename']
        file_size_gb = selected_file['size'] / 1024 / 1024 / 1024
        quantization = selected_file['quantization'] or 'unknown'

        print(f"Selected GGUF file: {filename}")
        print(f"Quantization: {quantization}, Size: {file_size_gb:.2f}GB")
        print("Downloading GGUF file directly...")

        try:
            # Create progress tracker if callback provided
            tqdm_class = None
            if progress_callback:
                # Create a factory function that captures the callback
                def make_progress(*args, **kwargs):
                    return ProgressTracker(*args, callback=progress_callback, **kwargs)
                tqdm_class = make_progress

            # Download GGUF file directly
            downloaded_path = hf_hub_download(
                repo_id=model_id,
                filename=filename,
                cache_dir=self.cache_dir,
                token=auth_token,
                tqdm_class=tqdm_class
            )

            print(f"Downloaded to: {downloaded_path}")

            # Verify checksum if available
            expected_checksum = self._get_file_checksum(model_id, filename, auth_token)
            if expected_checksum:
                print("Verifying checksum...")
                if self._verify_checksum(Path(downloaded_path), expected_checksum):
                    print("Checksum verification passed")
                else:
                    print("Error: Checksum verification failed")
                    return False
            else:
                print("Warning: No checksum available from HuggingFace metadata")

            # Copy to output path
            import shutil
            output_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(downloaded_path, output_path)

            print(f"GGUF file ready at: {output_path}")
            return True

        except Exception as e:
            print(f"Error downloading GGUF file: {e}")
            return False

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
        
    def _calculate_hash(self, path: Path, algorithm: str = 'sha256') -> str:
        """
        Calculate hash of a file

        Args:
            path: Path to file
            algorithm: Hash algorithm (default: sha256)

        Returns:
            Hex string of file hash (first 16 chars for cache key)
        """
        hash_obj = hashlib.new(algorithm)

        with open(path, 'rb') as f:
            # Read in chunks for memory efficiency
            for chunk in iter(lambda: f.read(8192), b''):
                hash_obj.update(chunk)

        return hash_obj.hexdigest()[:16]
        
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

    # Import registry
    from .ollama_registry import OllamaRegistry

    # Parse model ID
    if model_id.startswith('huggingface:'):
        model_id = model_id[len('huggingface:'):]

    # Resolve Ollama-style names to HuggingFace repo IDs
    registry = OllamaRegistry()
    resolved_id = registry.resolve(model_id)
    if resolved_id:
        model_id = resolved_id

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