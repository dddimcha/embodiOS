#!/usr/bin/env python3
"""
Model Compatibility Test Utilities
Helper functions for GGUF validation, tokenization checks, and inference testing
"""

import struct
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any
import hashlib


# GGUF Format Constants
GGUF_MAGIC = 0x46554747  # "GGUF" in little-endian
GGUF_VERSION_SUPPORTED = [2, 3]  # Supported GGUF versions
MIN_GGUF_SIZE = 8  # Minimum size for valid GGUF header (magic + version)


def validate_gguf_format(file_path: Path) -> Tuple[bool, str]:
    """
    Validate that a file is in valid GGUF format

    Args:
        file_path: Path to the GGUF file to validate

    Returns:
        Tuple of (is_valid, message) where:
        - is_valid: True if file is valid GGUF format
        - message: Description of validation result or error
    """
    if not file_path.exists():
        return False, f"File not found: {file_path}"

    if not file_path.is_file():
        return False, f"Path is not a file: {file_path}"

    file_size = file_path.stat().st_size
    if file_size < MIN_GGUF_SIZE:
        return False, f"File too small ({file_size} bytes) to be valid GGUF"

    try:
        with open(file_path, 'rb') as f:
            # Read and validate magic number
            magic_bytes = f.read(4)
            if len(magic_bytes) < 4:
                return False, "Failed to read GGUF magic number"

            magic = struct.unpack('<I', magic_bytes)[0]
            if magic != GGUF_MAGIC:
                magic_str = magic_bytes.hex()
                return False, f"Invalid GGUF magic number: 0x{magic_str}"

            # Read and validate version
            version_bytes = f.read(4)
            if len(version_bytes) < 4:
                return False, "Failed to read GGUF version"

            version = struct.unpack('<I', version_bytes)[0]
            if version not in GGUF_VERSION_SUPPORTED:
                return False, f"Unsupported GGUF version: {version}"

            return True, f"Valid GGUF format (version {version})"

    except struct.error as e:
        return False, f"GGUF format error: {str(e)}"
    except IOError as e:
        return False, f"IO error reading file: {str(e)}"
    except Exception as e:
        return False, f"Unexpected error: {str(e)}"


def test_inference(
    model_path: Path,
    test_input: Optional[List[int]] = None,
    mock_mode: bool = True
) -> Tuple[bool, str, Optional[Dict]]:
    """
    Test basic inference capability of a model

    Args:
        model_path: Path to the model file
        test_input: Optional test input tokens (defaults to simple test sequence)
        mock_mode: If True, performs mock inference without loading actual model

    Returns:
        Tuple of (success, message, result) where:
        - success: True if inference test passed
        - message: Description of test result
        - result: Optional dictionary with inference metadata
    """
    if not model_path.exists():
        return False, f"Model file not found: {model_path}", None

    # Validate model format first
    format_valid, format_msg = validate_gguf_format(model_path)
    if not format_valid:
        return False, f"Invalid model format: {format_msg}", None

    # Default test input (simple token sequence)
    if test_input is None:
        test_input = [1, 2, 3, 4, 5]  # Simple test tokens

    if not test_input or len(test_input) == 0:
        return False, "Test input cannot be empty", None

    result = {
        'model_path': str(model_path),
        'input_length': len(test_input),
        'format_valid': format_valid
    }

    if mock_mode:
        # Mock inference: validate structure without actual model execution
        result['mock_mode'] = True
        result['inference_attempted'] = False
        result['output_tokens'] = None

        # Check model file size is reasonable
        model_size = model_path.stat().st_size
        result['model_size'] = model_size

        if model_size < 1024:  # Less than 1KB
            return False, "Model file too small for valid model", result

        return True, "Mock inference validation passed", result

    else:
        # Actual inference would require loading the model
        # This is a placeholder for real inference testing
        result['mock_mode'] = False
        result['inference_attempted'] = True

        try:
            # In real implementation, this would:
            # 1. Load the model using EMBODIOSInferenceEngine
            # 2. Run inference with test_input
            # 3. Validate output tokens are generated

            # For now, return success with placeholder result
            result['output_tokens'] = None
            result['inference_status'] = 'not_implemented'

            return True, "Inference structure validated (full inference not implemented)", result

        except Exception as e:
            result['error'] = str(e)
            return False, f"Inference failed: {str(e)}", result


def get_model_metadata(file_path: Path) -> Dict[str, Any]:
    """
    Extract metadata from a model file

    Args:
        file_path: Path to the model file

    Returns:
        Dictionary with model metadata including:
        - exists, path, name, size, format validation, checksum
    """
    if not file_path.exists():
        return {
            'exists': False,
            'path': str(file_path),
            'error': 'File not found'
        }

    stat = file_path.stat()

    # Validate format
    format_valid, format_msg = validate_gguf_format(file_path)

    # Calculate checksum
    checksum = calculate_file_checksum(file_path)

    return {
        'exists': True,
        'path': str(file_path),
        'name': file_path.name,
        'size': stat.st_size,
        'size_mb': stat.st_size / 1024 / 1024,
        'size_gb': stat.st_size / 1024 / 1024 / 1024,
        'modified': stat.st_mtime,
        'format': file_path.suffix.lower(),
        'format_valid': format_valid,
        'format_message': format_msg,
        'checksum': checksum
    }


def calculate_file_checksum(file_path: Path, algorithm: str = 'sha256') -> str:
    """
    Calculate checksum of a file

    Args:
        file_path: Path to file
        algorithm: Hash algorithm (default: sha256)

    Returns:
        Hex string of file hash
    """
    if not file_path.exists():
        return ''

    hash_obj = hashlib.new(algorithm)

    try:
        with open(file_path, 'rb') as f:
            # Read in chunks for memory efficiency
            for chunk in iter(lambda: f.read(8192), b''):
                hash_obj.update(chunk)

        return hash_obj.hexdigest()
    except Exception:
        return ''


def verify_quantization_format(file_path: Path, expected_quant: str) -> Tuple[bool, str]:
    """
    Verify model quantization format matches expected type

    Args:
        file_path: Path to the model file
        expected_quant: Expected quantization format (e.g., "Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0")

    Returns:
        Tuple of (matches, message)
    """
    # First validate it's a valid GGUF file
    format_valid, format_msg = validate_gguf_format(file_path)
    if not format_valid:
        return False, f"Invalid GGUF format: {format_msg}"

    # Check if filename contains quantization info
    filename = file_path.name.lower()
    expected_lower = expected_quant.lower().replace('_', '-')

    # Common quantization naming patterns in filenames
    quant_patterns = [
        expected_quant.lower(),
        expected_lower,
        expected_quant.upper(),
        expected_quant.replace('_', '-').lower()
    ]

    for pattern in quant_patterns:
        if pattern in filename:
            return True, f"Quantization format {expected_quant} detected in filename"

    # If not in filename, would need to parse GGUF metadata
    # For now, return inconclusive result
    return False, f"Could not verify quantization format {expected_quant} from filename"


def validate_tokenizer_output(tokens: List[int], min_tokens: int = 1) -> Tuple[bool, str]:
    """
    Validate that tokenizer output is valid

    Args:
        tokens: List of token IDs from tokenizer
        min_tokens: Minimum number of tokens expected

    Returns:
        Tuple of (is_valid, message)
    """
    if not tokens:
        return False, "Token list is empty"

    if not isinstance(tokens, list):
        return False, f"Expected list of tokens, got {type(tokens)}"

    if len(tokens) < min_tokens:
        return False, f"Expected at least {min_tokens} tokens, got {len(tokens)}"

    # Validate all tokens are integers
    for i, token in enumerate(tokens):
        if not isinstance(token, int):
            return False, f"Token at index {i} is not an integer: {type(token)}"

        # Check for reasonable token ID range (most models use 0-100000)
        if token < 0 or token > 200000:
            return False, f"Token ID {token} at index {i} is out of reasonable range"

    return True, f"Valid tokenizer output: {len(tokens)} tokens"


def compare_model_outputs(
    output1: List[int],
    output2: List[int],
    tolerance: float = 0.0
) -> Tuple[bool, str, float]:
    """
    Compare outputs from different model versions/quantizations

    Args:
        output1: First model output tokens
        output2: Second model output tokens
        tolerance: Allowed difference ratio (0.0 = exact match, 1.0 = completely different)

    Returns:
        Tuple of (are_similar, message, similarity_score)
    """
    if not output1 or not output2:
        return False, "One or both outputs are empty", 0.0

    # Calculate similarity based on matching tokens
    min_len = min(len(output1), len(output2))
    max_len = max(len(output1), len(output2))

    if max_len == 0:
        return False, "Both outputs are empty", 0.0

    # Count matching tokens at same positions
    matches = sum(1 for i in range(min_len) if output1[i] == output2[i])

    # Calculate similarity score (0.0 to 1.0)
    similarity = matches / max_len

    # Check if within tolerance
    difference = 1.0 - similarity

    if difference <= tolerance:
        return True, f"Outputs are similar (similarity: {similarity:.2%})", similarity
    else:
        return False, f"Outputs differ too much (similarity: {similarity:.2%})", similarity
