#!/usr/bin/env python3
"""
Ollama Model Registry for EMBODIOS
Maps Ollama-style model names to HuggingFace repository IDs
"""

from typing import Optional, Dict


class OllamaRegistry:
    """Registry mapping Ollama model names to HuggingFace repositories"""

    def __init__(self):
        # Registry of Ollama-style names to HuggingFace repo IDs
        # Format: {ollama_name: huggingface_repo_id}
        self.registry: Dict[str, str] = {
            # TinyLlama models
            "tinyllama": "TinyLlama/TinyLlama-1.1B-Chat-v1.0",
            "tinyllama-chat": "TinyLlama/TinyLlama-1.1B-Chat-v1.0",

            # Phi models
            "phi": "microsoft/phi-2",
            "phi2": "microsoft/phi-2",
            "phi-2": "microsoft/phi-2",

            # Mistral models
            "mistral": "mistralai/Mistral-7B-Instruct-v0.2",
            "mistral-7b": "mistralai/Mistral-7B-Instruct-v0.2",
            "mistral-instruct": "mistralai/Mistral-7B-Instruct-v0.2",

            # Llama 2 models
            "llama2": "meta-llama/Llama-2-7b-chat-hf",
            "llama2-7b": "meta-llama/Llama-2-7b-chat-hf",
            "llama2-13b": "meta-llama/Llama-2-13b-chat-hf",
            "llama2-70b": "meta-llama/Llama-2-70b-chat-hf",

            # Code Llama models
            "codellama": "codellama/CodeLlama-7b-hf",
            "codellama-7b": "codellama/CodeLlama-7b-hf",
            "codellama-13b": "codellama/CodeLlama-13b-hf",
            "codellama-34b": "codellama/CodeLlama-34b-hf",
            "codellama-python": "codellama/CodeLlama-7b-Python-hf",

            # Gemma models
            "gemma": "google/gemma-2b-it",
            "gemma-2b": "google/gemma-2b-it",
            "gemma-7b": "google/gemma-7b-it",

            # Falcon models
            "falcon": "tiiuae/falcon-7b-instruct",
            "falcon-7b": "tiiuae/falcon-7b-instruct",

            # Vicuna models
            "vicuna": "lmsys/vicuna-7b-v1.5",
            "vicuna-7b": "lmsys/vicuna-7b-v1.5",
            "vicuna-13b": "lmsys/vicuna-13b-v1.5",

            # Orca models
            "orca": "microsoft/Orca-2-7b",
            "orca-2": "microsoft/Orca-2-7b",

            # Zephyr models
            "zephyr": "HuggingFaceH4/zephyr-7b-beta",
            "zephyr-7b": "HuggingFaceH4/zephyr-7b-beta",

            # Neural Chat models
            "neural-chat": "Intel/neural-chat-7b-v3-1",

            # Starling models
            "starling": "berkeley-nest/Starling-LM-7B-alpha",
            "starling-7b": "berkeley-nest/Starling-LM-7B-alpha",
        }

    def resolve(self, model_name: str) -> Optional[str]:
        """
        Resolve an Ollama-style model name to a HuggingFace repository ID

        Args:
            model_name: Ollama model name (e.g., 'tinyllama', 'mistral')

        Returns:
            HuggingFace repo ID if found, None otherwise
        """
        # Normalize to lowercase for case-insensitive lookup
        normalized_name = model_name.lower().strip()

        # Check if it's in the registry
        if normalized_name in self.registry:
            return self.registry[normalized_name]

        # If not found, check if it's already a HuggingFace repo ID (contains '/')
        if '/' in model_name:
            return model_name

        return None

    def list_models(self) -> Dict[str, str]:
        """
        Get all available model mappings

        Returns:
            Dictionary of model name to repo ID mappings
        """
        return self.registry.copy()

    def add_model(self, name: str, repo_id: str) -> None:
        """
        Add a custom model mapping

        Args:
            name: Ollama-style model name
            repo_id: HuggingFace repository ID
        """
        self.registry[name.lower()] = repo_id
