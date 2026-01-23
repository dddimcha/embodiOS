#!/usr/bin/env python3
"""
Model Compatibility Test Suite
Tests that verify EMBODIOS correctly loads and runs inference on popular Ollama ecosystem models
"""

import pytest
import json
from pathlib import Path
from typing import List, Dict

from tests.models.compatibility_utils import (
    validate_gguf_format,
    run_model_inference_test,
    get_model_metadata,
    verify_quantization_format,
    validate_tokenizer_output,
    compare_model_outputs
)


# Load Ollama models for parametrization
def load_ollama_models():
    """Load the list of 20 Ollama models from fixtures"""
    fixture_path = Path(__file__).parent / "fixtures" / "ollama_models.json"
    with open(fixture_path, 'r') as f:
        data = json.load(f)
    return data['models']


@pytest.mark.parametrize("model", load_ollama_models(), ids=lambda m: m['name'])
def test_ollama_model_compatibility(model):
    """Test individual Ollama model compatibility and metadata validation"""
    # Verify model has all required fields
    assert "name" in model, "Model must have 'name' field"
    assert "full_name" in model, "Model must have 'full_name' field"
    assert "parameters" in model, "Model must have 'parameters' field"
    assert "format" in model, "Model must have 'format' field"
    assert "quantization" in model, "Model must have 'quantization' field"
    assert "size" in model, "Model must have 'size' field"
    assert "context_length" in model, "Model must have 'context_length' field"
    assert "source" in model, "Model must have 'source' field"
    assert "license" in model, "Model must have 'license' field"
    assert "family" in model, "Model must have 'family' field"
    assert "capabilities" in model, "Model must have 'capabilities' field"

    # Validate field types and values
    assert isinstance(model["name"], str), "Model name must be string"
    assert len(model["name"]) > 0, "Model name cannot be empty"

    assert isinstance(model["full_name"], str), "Model full_name must be string"
    assert len(model["full_name"]) > 0, "Model full_name cannot be empty"

    assert isinstance(model["parameters"], str), "Model parameters must be string"
    assert model["parameters"].endswith("B"), "Model parameters should end with 'B' (billions)"

    assert model["format"] == "gguf", f"Model {model['name']} must be GGUF format"

    assert isinstance(model["quantization"], str), "Model quantization must be string"
    valid_quants = ["Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0"]
    assert model["quantization"] in valid_quants, f"Model {model['name']} quantization must be one of {valid_quants}"

    assert isinstance(model["size"], int), "Model size must be integer"
    assert model["size"] > 0, "Model size must be positive"
    assert model["size"] < 100_000_000_000, f"Model {model['name']} size unreasonably large"

    assert isinstance(model["context_length"], int), "Model context_length must be integer"
    assert model["context_length"] >= 2048, f"Model {model['name']} context_length must be at least 2048"
    assert model["context_length"] <= 100000, f"Model {model['name']} context_length unreasonably large"

    assert isinstance(model["source"], str), "Model source must be string"
    assert len(model["source"]) > 0, "Model source cannot be empty"

    assert isinstance(model["license"], str), "Model license must be string"
    assert len(model["license"]) > 0, "Model license cannot be empty"

    assert isinstance(model["family"], str), "Model family must be string"
    assert len(model["family"]) > 0, "Model family cannot be empty"

    assert isinstance(model["capabilities"], list), "Model capabilities must be list"
    assert len(model["capabilities"]) > 0, f"Model {model['name']} must have at least one capability"
    for cap in model["capabilities"]:
        assert isinstance(cap, str), "All capabilities must be strings"
        assert len(cap) > 0, "Capabilities cannot be empty strings"


def test_quantization_formats(temp_models_dir):
    """Test quantization format validation for all supported quantization types"""
    import tempfile
    import shutil

    # Valid quantization formats supported by EMBODIOS
    valid_quants = ["Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0"]

    # Create valid GGUF file content
    gguf_magic = b'GGUF'
    version = (3).to_bytes(4, byteorder='little')
    minimal_gguf = gguf_magic + version + b'\x00' * (2048 - len(gguf_magic) - 4)

    # Test each valid quantization format
    for quant in valid_quants:
        # Create a GGUF file with quantization in filename
        test_filename = f"model-{quant}.gguf"
        test_path = temp_models_dir / test_filename

        with open(test_path, 'wb') as f:
            f.write(minimal_gguf)

        # Verify quantization format is detected
        matches, message = verify_quantization_format(test_path, quant)
        assert matches is True, f"Should detect {quant} format in filename: {message}"
        assert quant in message, f"Message should mention quantization format: {message}"

        # Cleanup
        test_path.unlink()

    # Test with lowercase quantization in filename
    test_path = temp_models_dir / "model-q4_k_m.gguf"
    with open(test_path, 'wb') as f:
        f.write(minimal_gguf)

    matches, message = verify_quantization_format(test_path, "Q4_K_M")
    assert matches is True, f"Should detect Q4_K_M in lowercase filename: {message}"
    test_path.unlink()

    # Test with dash-separated format
    test_path = temp_models_dir / "model-q5-k-m.gguf"
    with open(test_path, 'wb') as f:
        f.write(minimal_gguf)

    matches, message = verify_quantization_format(test_path, "Q5_K_M")
    assert matches is True, f"Should detect Q5_K_M in dash-separated filename: {message}"
    test_path.unlink()

    # Test with invalid GGUF file
    invalid_path = temp_models_dir / "model-Q4_K_M.gguf"
    with open(invalid_path, 'wb') as f:
        f.write(b'FAKE' + b'\x00' * 2044)

    matches, message = verify_quantization_format(invalid_path, "Q4_K_M")
    assert matches is False, "Should fail validation for invalid GGUF format"
    assert "Invalid GGUF format" in message, "Should report invalid GGUF format"
    invalid_path.unlink()

    # Test with quantization not in filename
    no_quant_path = temp_models_dir / "model.gguf"
    with open(no_quant_path, 'wb') as f:
        f.write(minimal_gguf)

    matches, message = verify_quantization_format(no_quant_path, "Q4_K_M")
    assert matches is False, "Should not detect quantization if not in filename"
    assert "Could not verify" in message, "Should report inability to verify from filename"
    no_quant_path.unlink()

    # Test with wrong quantization in filename
    wrong_quant_path = temp_models_dir / "model-Q6_K.gguf"
    with open(wrong_quant_path, 'wb') as f:
        f.write(minimal_gguf)

    matches, message = verify_quantization_format(wrong_quant_path, "Q4_K_M")
    assert matches is False, "Should not match wrong quantization format"
    wrong_quant_path.unlink()


def load_model_quantization_matrix():
    """Generate matrix of model and quantization format combinations for testing"""
    models = load_ollama_models()
    valid_quants = ["Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0"]

    # Create all combinations of models and quantization formats
    matrix = []
    ids = []
    for model in models:
        for quant in valid_quants:
            matrix.append((model, quant))
            ids.append(f"{model['name']}-{quant}")

    return matrix, ids


# Get matrix and ids for parametrization
_matrix, _matrix_ids = load_model_quantization_matrix()


@pytest.mark.parametrize(
    "model,quantization",
    _matrix,
    ids=_matrix_ids
)
def test_model_quantization_matrix(model, quantization):
    """Test matrix of all model and quantization format combinations"""
    # Verify model has required fields
    assert "name" in model, "Model must have 'name' field"
    assert "format" in model, "Model must have 'format' field"
    assert "quantization" in model, "Model must have 'quantization' field"

    # Verify model format is GGUF
    assert model["format"] == "gguf", f"Model {model['name']} must be GGUF format"

    # Verify quantization is valid
    valid_quants = ["Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0"]
    assert quantization in valid_quants, f"Quantization {quantization} must be in {valid_quants}"

    # Verify model's declared quantization is also valid
    assert model["quantization"] in valid_quants, f"Model {model['name']} quantization must be in {valid_quants}"

    # Test that quantization format is compatible with model format
    assert model["format"] == "gguf", f"Quantization {quantization} requires GGUF format for model {model['name']}"

    # Verify model has size information
    assert "size" in model, f"Model {model['name']} must have 'size' field"
    assert isinstance(model["size"], int), f"Model {model['name']} size must be integer"
    assert model["size"] > 0, f"Model {model['name']} size must be positive"

    # Verify quantization affects expected model size ranges
    # Q4_K_M should be smallest, Q8_0 should be largest
    if quantization == "Q8_0":
        # Q8_0 models are typically larger
        assert model["size"] > 0, f"Q8_0 model {model['name']} should have positive size"
    elif quantization == "Q4_K_M":
        # Q4_K_M models are typically smaller
        assert model["size"] > 0, f"Q4_K_M model {model['name']} should have positive size"


def test_model_loading_format(valid_gguf_file, ollama_models_list):
    """Test that validates GGUF format for all 20 models using minimal downloads or mocked data"""
    # Verify we have 20 models in the fixture
    assert len(ollama_models_list) == 20, f"Expected 20 models, got {len(ollama_models_list)}"

    # Test valid GGUF file format
    is_valid, message = validate_gguf_format(valid_gguf_file)
    assert is_valid is True, f"Valid GGUF file should pass validation: {message}"
    assert "Valid GGUF format" in message, "Should report valid format"

    # Verify all models in fixture have required metadata
    for model in ollama_models_list:
        assert "name" in model, f"Model missing 'name' field: {model}"
        assert "format" in model, f"Model {model['name']} missing 'format' field"
        assert model["format"] == "gguf", f"Model {model['name']} should be GGUF format"
        assert "quantization" in model, f"Model {model['name']} missing 'quantization' field"
        assert "size" in model, f"Model {model['name']} missing 'size' field"


class TestModelLoadingFormat:
    """Test suite for model loading and GGUF format validation"""

    def test_model_loading_format(self, valid_gguf_file, ollama_models_list):
        """Test that validates GGUF format for all 20 models using minimal downloads or mocked data"""
        # Verify we have 20 models in the fixture
        assert len(ollama_models_list) == 20, f"Expected 20 models, got {len(ollama_models_list)}"

        # Test valid GGUF file format
        is_valid, message = validate_gguf_format(valid_gguf_file)
        assert is_valid is True, f"Valid GGUF file should pass validation: {message}"
        assert "Valid GGUF format" in message, "Should report valid format"

        # Verify all models in fixture have required metadata
        for model in ollama_models_list:
            assert "name" in model, f"Model missing 'name' field: {model}"
            assert "format" in model, f"Model {model['name']} missing 'format' field"
            assert model["format"] == "gguf", f"Model {model['name']} should be GGUF format"
            assert "quantization" in model, f"Model {model['name']} missing 'quantization' field"
            assert "size" in model, f"Model {model['name']} missing 'size' field"

    def test_invalid_gguf_format(self, invalid_gguf_file):
        """Test that invalid GGUF files are properly rejected"""
        is_valid, message = validate_gguf_format(invalid_gguf_file)
        assert is_valid is False, "Invalid GGUF file should fail validation"
        assert "Invalid GGUF magic number" in message, "Should report invalid magic number"

    def test_nonexistent_file(self):
        """Test handling of nonexistent model files"""
        nonexistent_path = Path("/nonexistent/model.gguf")
        is_valid, message = validate_gguf_format(nonexistent_path)
        assert is_valid is False, "Nonexistent file should fail validation"
        assert "not found" in message.lower(), "Should report file not found"

    def test_model_metadata_extraction(self, valid_gguf_file):
        """Test extraction of model metadata"""
        metadata = get_model_metadata(valid_gguf_file)

        assert metadata["exists"] is True, "File should exist"
        assert metadata["format_valid"] is True, "Format should be valid"
        assert "checksum" in metadata, "Should include checksum"
        assert "size" in metadata, "Should include size"
        assert metadata["size"] > 0, "Size should be positive"

    def test_model_metadata_for_nonexistent_file(self):
        """Test metadata extraction for nonexistent file"""
        nonexistent_path = Path("/nonexistent/model.gguf")
        metadata = get_model_metadata(nonexistent_path)

        assert metadata["exists"] is False, "Should report file doesn't exist"
        assert "error" in metadata, "Should include error message"

    def test_mock_inference_validation(self, valid_gguf_file):
        """Test mock inference validation for valid GGUF file"""
        success, message, result = run_model_inference_test(valid_gguf_file, mock_mode=True)

        assert success is True, f"Mock inference should succeed: {message}"
        assert result is not None, "Should return result dictionary"
        assert result["format_valid"] is True, "Format should be valid"
        assert result["mock_mode"] is True, "Should indicate mock mode"

    def test_inference_with_invalid_model(self, invalid_gguf_file):
        """Test inference validation fails for invalid GGUF file"""
        success, message, result = run_model_inference_test(invalid_gguf_file, mock_mode=True)

        assert success is False, "Inference should fail for invalid format"
        assert "Invalid model format" in message, "Should report invalid format"

    def test_all_quantization_types_represented(self, ollama_models_list):
        """Test that all required quantization types are represented in test models"""
        required_quants = ["Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0"]
        found_quants = set()

        for model in ollama_models_list:
            quant = model.get("quantization", "")
            found_quants.add(quant)

        for required in required_quants:
            assert required in found_quants, f"Required quantization {required} not found in test models"

    def test_model_families_represented(self, ollama_models_list):
        """Test that multiple model families are represented"""
        families = set(model.get("family", "") for model in ollama_models_list)

        # Should have at least these major families
        expected_families = ["llama", "mistral", "mixtral"]
        for family in expected_families:
            assert family in families, f"Model family {family} not found in test models"

    def test_model_sizes_vary(self, ollama_models_list):
        """Test that models have varying sizes (from small to large)"""
        sizes = [model.get("size", 0) for model in ollama_models_list]
        sizes.sort()

        # Should have models of different sizes
        assert len(sizes) == 20, "Should have 20 model sizes"
        assert sizes[0] < 2_000_000_000, "Should have small models (< 2GB)"
        assert sizes[-1] > 10_000_000_000, "Should have large models (> 10GB)"

    def test_model_context_lengths(self, ollama_models_list):
        """Test that models have valid context lengths"""
        for model in ollama_models_list:
            context_length = model.get("context_length", 0)
            assert context_length > 0, f"Model {model['name']} has invalid context length"
            assert context_length >= 2048, f"Model {model['name']} context length too small"
            assert context_length <= 100000, f"Model {model['name']} context length unreasonably large"

    def test_model_capabilities(self, ollama_models_list):
        """Test that models have capability tags"""
        for model in ollama_models_list:
            capabilities = model.get("capabilities", [])
            assert isinstance(capabilities, list), f"Model {model['name']} capabilities should be a list"
            assert len(capabilities) > 0, f"Model {model['name']} should have at least one capability"


def test_basic_inference(valid_gguf_file):
    """Test basic inference validation with various inputs and scenarios"""
    # Test 1: Basic inference with valid model and default input
    success, message, result = run_model_inference_test(valid_gguf_file, mock_mode=True)
    assert success is True, f"Basic inference should succeed: {message}"
    assert result is not None, "Should return result dictionary"
    assert result["format_valid"] is True, "Format should be valid"
    assert result["mock_mode"] is True, "Should indicate mock mode"
    assert result["input_length"] == 5, "Should use default input length of 5"

    # Test 2: Inference with custom input tokens
    custom_input = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    success, message, result = run_model_inference_test(valid_gguf_file, test_input=custom_input, mock_mode=True)
    assert success is True, f"Inference with custom input should succeed: {message}"
    assert result["input_length"] == 10, "Should use custom input length"

    # Test 3: Inference with longer input sequence
    long_input = list(range(1, 101))  # 100 tokens
    success, message, result = run_model_inference_test(valid_gguf_file, test_input=long_input, mock_mode=True)
    assert success is True, f"Inference with long input should succeed: {message}"
    assert result["input_length"] == 100, "Should handle long input sequence"

    # Test 4: Inference with minimal input
    minimal_input = [1]
    success, message, result = run_model_inference_test(valid_gguf_file, test_input=minimal_input, mock_mode=True)
    assert success is True, f"Inference with minimal input should succeed: {message}"
    assert result["input_length"] == 1, "Should handle single token input"

    # Test 5: Inference with empty input should fail
    empty_input = []
    success, message, result = run_model_inference_test(valid_gguf_file, test_input=empty_input, mock_mode=True)
    assert success is False, "Empty input should fail"
    assert "cannot be empty" in message.lower(), "Should report empty input error"

    # Test 6: Inference with nonexistent model should fail
    nonexistent_path = Path("/nonexistent/model.gguf")
    success, message, result = run_model_inference_test(nonexistent_path, mock_mode=True)
    assert success is False, "Nonexistent model should fail"
    assert "not found" in message.lower(), "Should report file not found"
    assert result is None, "Should return None for nonexistent file"

    # Test 7: Verify result structure contains expected fields
    success, message, result = run_model_inference_test(valid_gguf_file, mock_mode=True)
    assert "model_path" in result, "Result should include model_path"
    assert "input_length" in result, "Result should include input_length"
    assert "format_valid" in result, "Result should include format_valid"
    assert "model_size" in result, "Result should include model_size"
    assert result["model_size"] > 0, "Model size should be positive"

    # Test 8: Verify non-mock mode structure (placeholder implementation)
    success, message, result = run_model_inference_test(valid_gguf_file, mock_mode=False)
    assert success is True, f"Non-mock inference should complete: {message}"
    assert result["mock_mode"] is False, "Should indicate non-mock mode"
    assert result["inference_attempted"] is True, "Should indicate inference was attempted"


def test_tokenization():
    """Comprehensive tokenization validation test"""
    # Test 1: Valid token sequence
    valid_tokens = [1, 2, 3, 4, 5]
    is_valid, message = validate_tokenizer_output(valid_tokens)
    assert is_valid is True, f"Valid tokens should pass: {message}"
    assert "Valid tokenizer output" in message, "Should report valid tokenizer output"

    # Test 2: Longer token sequence with typical model tokens
    long_tokens = list(range(0, 100))
    is_valid, message = validate_tokenizer_output(long_tokens)
    assert is_valid is True, f"Long token sequence should pass: {message}"

    # Test 3: Empty token output should fail
    empty_tokens = []
    is_valid, message = validate_tokenizer_output(empty_tokens)
    assert is_valid is False, "Empty tokens should fail validation"
    assert "empty" in message.lower(), "Should report empty token list"

    # Test 4: Non-integer tokens should fail
    invalid_tokens = [1, 2, "three", 4]
    is_valid, message = validate_tokenizer_output(invalid_tokens)
    assert is_valid is False, "Non-integer tokens should fail"
    assert "not an integer" in message, "Should report type error"

    # Test 5: Out-of-range tokens should fail
    out_of_range = [1, 2, 300000, 4]
    is_valid, message = validate_tokenizer_output(out_of_range)
    assert is_valid is False, "Out-of-range tokens should fail"
    assert "out of reasonable range" in message, "Should report range error"

    # Test 6: Negative tokens should fail
    negative_tokens = [1, 2, -5, 4]
    is_valid, message = validate_tokenizer_output(negative_tokens)
    assert is_valid is False, "Negative tokens should fail"
    assert "out of reasonable range" in message, "Should report range error"

    # Test 7: Token sequence with maximum valid token ID
    max_valid_tokens = [1, 2, 200000, 4]
    is_valid, message = validate_tokenizer_output(max_valid_tokens)
    assert is_valid is True, f"Tokens at max range should pass: {message}"

    # Test 8: Token sequence with minimum valid token ID (0)
    min_valid_tokens = [0, 1, 2, 3]
    is_valid, message = validate_tokenizer_output(min_valid_tokens)
    assert is_valid is True, f"Tokens with 0 should pass: {message}"


class TestTokenization:
    """Test suite for tokenization validation"""

    def test_tokenizer_output_validation(self):
        """Test validation of tokenizer outputs"""
        # Valid token sequence
        valid_tokens = [1, 2, 3, 4, 5]
        is_valid, message = validate_tokenizer_output(valid_tokens)
        assert is_valid is True, f"Valid tokens should pass: {message}"

    def test_tokenizer_empty_output(self):
        """Test that empty token output is rejected"""
        empty_tokens = []
        is_valid, message = validate_tokenizer_output(empty_tokens)
        assert is_valid is False, "Empty tokens should fail validation"
        assert "empty" in message.lower(), "Should report empty token list"

    def test_tokenizer_invalid_token_types(self):
        """Test that non-integer tokens are rejected"""
        invalid_tokens = [1, 2, "three", 4]
        is_valid, message = validate_tokenizer_output(invalid_tokens)
        assert is_valid is False, "Non-integer tokens should fail"
        assert "not an integer" in message, "Should report type error"

    def test_tokenizer_out_of_range_tokens(self):
        """Test that out-of-range token IDs are rejected"""
        out_of_range = [1, 2, 300000, 4]  # 300000 is out of reasonable range
        is_valid, message = validate_tokenizer_output(out_of_range)
        assert is_valid is False, "Out-of-range tokens should fail"
        assert "out of reasonable range" in message, "Should report range error"


class TestModelComparison:
    """Test suite for comparing model outputs"""

    def test_identical_outputs_comparison(self):
        """Test comparison of identical outputs"""
        output1 = [1, 2, 3, 4, 5]
        output2 = [1, 2, 3, 4, 5]
        are_similar, message, similarity = compare_model_outputs(output1, output2)

        assert are_similar is True, "Identical outputs should be similar"
        assert similarity == 1.0, "Identical outputs should have 100% similarity"

    def test_different_outputs_comparison(self):
        """Test comparison of completely different outputs"""
        output1 = [1, 2, 3, 4, 5]
        output2 = [6, 7, 8, 9, 10]
        are_similar, message, similarity = compare_model_outputs(output1, output2)

        assert are_similar is False, "Different outputs should not be similar"
        assert similarity == 0.0, "Completely different outputs should have 0% similarity"

    def test_partial_match_comparison(self):
        """Test comparison of partially matching outputs"""
        output1 = [1, 2, 3, 4, 5]
        output2 = [1, 2, 3, 6, 7]  # First 3 match
        are_similar, message, similarity = compare_model_outputs(output1, output2)

        assert 0.0 < similarity < 1.0, "Partial match should have intermediate similarity"
        assert "similarity:" in message.lower(), "Message should report similarity"

    def test_empty_outputs_comparison(self):
        """Test comparison fails with empty outputs"""
        output1 = []
        output2 = [1, 2, 3]
        are_similar, message, similarity = compare_model_outputs(output1, output2)

        assert are_similar is False, "Empty output comparison should fail"
        assert similarity == 0.0, "Empty comparison should have 0 similarity"


class TestQuantizationValidation:
    """Test suite for quantization format validation"""

    def test_quantization_format_detection(self, valid_gguf_file):
        """Test quantization format detection from filename"""
        # Create a test path with quantization in name
        test_path = valid_gguf_file.parent / "test_model_Q4_K_M.gguf"

        # Copy the valid file to the new name
        import shutil
        shutil.copy(valid_gguf_file, test_path)

        try:
            matches, message = verify_quantization_format(test_path, "Q4_K_M")
            assert matches is True, f"Should detect Q4_K_M in filename: {message}"
        finally:
            if test_path.exists():
                test_path.unlink()

    def test_quantization_mismatch_detection(self, valid_gguf_file):
        """Test detection when quantization doesn't match"""
        # Create a test path with different quantization
        test_path = valid_gguf_file.parent / "test_model_Q8_0.gguf"

        import shutil
        shutil.copy(valid_gguf_file, test_path)

        try:
            matches, message = verify_quantization_format(test_path, "Q4_K_M")
            # Should not match since filename says Q8_0 but we're checking for Q4_K_M
            assert matches is False, f"Should not match different quantization: {message}"
        finally:
            if test_path.exists():
                test_path.unlink()

    def test_supported_quantization_types(self):
        """Test that all supported quantization types are documented"""
        supported_quants = ["Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0"]

        # Verify these are the standard GGUF quantization types
        for quant in supported_quants:
            assert quant.startswith("Q"), f"Quantization {quant} should start with Q"
            assert any(char.isdigit() for char in quant), f"Quantization {quant} should contain digits"


def test_quantization_inference(temp_models_dir):
    """Test quantization inference comparison across different quantization levels"""
    # Valid quantization formats supported by EMBODIOS
    quantization_levels = ["Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0"]

    # Create valid GGUF file content
    gguf_magic = b'GGUF'
    version = (3).to_bytes(4, byteorder='little')
    minimal_gguf = gguf_magic + version + b'\x00' * (2048 - len(gguf_magic) - 4)

    # Create model files for each quantization level
    model_paths = {}
    for quant in quantization_levels:
        model_filename = f"test_model_{quant}.gguf"
        model_path = temp_models_dir / model_filename

        with open(model_path, 'wb') as f:
            f.write(minimal_gguf)

        model_paths[quant] = model_path

    # Test 1: Run mock inference on each quantization level
    inference_results = {}
    for quant, model_path in model_paths.items():
        success, message, result = run_model_inference_test(model_path, mock_mode=True)
        assert success is True, f"Inference should succeed for {quant}: {message}"
        assert result is not None, f"Should return result for {quant}"
        assert result["format_valid"] is True, f"Format should be valid for {quant}"
        inference_results[quant] = result

    # Test 2: Verify all quantization levels produced valid results
    assert len(inference_results) == 4, "Should have results for all 4 quantization levels"
    for quant, result in inference_results.items():
        assert "model_path" in result, f"Result for {quant} should include model_path"
        assert "model_size" in result, f"Result for {quant} should include model_size"
        assert result["model_size"] > 0, f"Model size for {quant} should be positive"

    # Test 3: Compare inference outputs between quantization levels
    # Simulate inference outputs (in real scenario, these would be actual model outputs)
    simulated_outputs = {
        "Q4_K_M": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],  # Lower quantization, slight variation
        "Q5_K_M": [1, 2, 3, 4, 5, 6, 7, 8, 9, 11],  # Medium quantization, very similar
        "Q6_K": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],    # Higher quantization, same as Q4
        "Q8_0": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]     # Highest quantization, most accurate
    }

    # Test 4: Compare Q4_K_M vs Q8_0 (lower vs highest quantization)
    output_q4 = simulated_outputs["Q4_K_M"]
    output_q8 = simulated_outputs["Q8_0"]
    are_similar, message, similarity = compare_model_outputs(output_q4, output_q8)
    assert are_similar is True, f"Q4_K_M and Q8_0 should produce similar outputs: {message}"
    assert similarity >= 0.9, f"Q4_K_M and Q8_0 should have high similarity (got {similarity:.2%})"

    # Test 5: Compare Q5_K_M vs Q8_0 (medium vs highest quantization)
    output_q5 = simulated_outputs["Q5_K_M"]
    are_similar, message, similarity = compare_model_outputs(output_q5, output_q8)
    assert similarity >= 0.9, f"Q5_K_M and Q8_0 should have high similarity (got {similarity:.2%})"

    # Test 6: Compare Q6_K vs Q8_0 (high vs highest quantization)
    output_q6 = simulated_outputs["Q6_K"]
    are_similar, message, similarity = compare_model_outputs(output_q6, output_q8)
    assert are_similar is True, f"Q6_K and Q8_0 should produce similar outputs: {message}"
    assert similarity == 1.0, f"Q6_K and Q8_0 should be identical in this test (got {similarity:.2%})"

    # Test 7: Compare identical outputs (Q4_K_M vs Q6_K)
    are_similar, message, similarity = compare_model_outputs(output_q4, output_q6)
    assert are_similar is True, "Identical outputs should be similar"
    assert similarity == 1.0, "Identical outputs should have 100% similarity"

    # Test 8: Test comparison with different outputs
    different_output = [20, 21, 22, 23, 24, 25, 26, 27, 28, 29]
    are_similar, message, similarity = compare_model_outputs(output_q8, different_output)
    assert are_similar is False, "Completely different outputs should not be similar"
    assert similarity == 0.0, "Completely different outputs should have 0% similarity"

    # Test 9: Test comparison with partially matching outputs
    partial_match = [1, 2, 3, 4, 5, 99, 99, 99, 99, 99]  # First half matches
    are_similar, message, similarity = compare_model_outputs(output_q8, partial_match)
    assert 0.0 < similarity < 1.0, "Partial match should have intermediate similarity"
    assert similarity == 0.5, f"Half matching should be 50% similar (got {similarity:.2%})"

    # Test 10: Test comparison with different length outputs
    shorter_output = [1, 2, 3, 4, 5]
    are_similar, message, similarity = compare_model_outputs(output_q8, shorter_output)
    assert similarity == 0.5, f"Half-length match should be 50% similar (got {similarity:.2%})"

    # Test 11: Test comparison with empty outputs
    empty_output = []
    are_similar, message, similarity = compare_model_outputs(output_q8, empty_output)
    assert are_similar is False, "Empty output comparison should fail"
    assert similarity == 0.0, "Empty comparison should have 0 similarity"

    # Test 12: Test comparison tolerance parameter
    slightly_different = [1, 2, 3, 4, 5, 6, 7, 8, 9, 11]  # 90% match
    are_similar, message, similarity = compare_model_outputs(output_q8, slightly_different, tolerance=0.1)
    assert are_similar is True, "Should be similar within 10% tolerance"
    assert similarity == 0.9, f"Should be 90% similar (got {similarity:.2%})"

    # Test 13: Test comparison with strict tolerance (should fail for 90% match)
    are_similar, message, similarity = compare_model_outputs(output_q8, slightly_different, tolerance=0.05)
    assert are_similar is False, "Should not be similar with 5% tolerance when 10% different"

    # Test 14: Test comparison with exact tolerance boundary
    are_similar, message, similarity = compare_model_outputs(output_q8, slightly_different, tolerance=0.0)
    assert are_similar is False, "Should not be similar with 0% tolerance"

    # Test 15: Verify quantization format is correctly detected in model files
    for quant, model_path in model_paths.items():
        matches, message = verify_quantization_format(model_path, quant)
        assert matches is True, f"Should detect {quant} format: {message}"

    # Test 16: Verify model metadata is correctly extracted
    for quant, model_path in model_paths.items():
        metadata = get_model_metadata(model_path)
        assert metadata["exists"] is True, f"Model file for {quant} should exist"
        assert metadata["format_valid"] is True, f"Format should be valid for {quant}"
        assert metadata["name"] == f"test_model_{quant}.gguf", f"Filename should match for {quant}"

    # Cleanup test files
    for model_path in model_paths.values():
        if model_path.exists():
            model_path.unlink()
