#!/usr/bin/env python3
"""
Workload generator for stability testing.

Generates realistic inference requests with varied prompts, token lengths,
and hardware operations to stress test EMBODIOS under diverse workloads.
"""

import random
from typing import List, Dict, Tuple
from dataclasses import dataclass


@dataclass
class WorkloadPattern:
    """
    Defines a workload pattern for inference testing.

    Attributes:
        name: Pattern name (e.g., "gpio_control", "sensor_read")
        token_range: Tuple of (min_tokens, max_tokens) for input length
        operations: List of hardware token ranges to include
        weight: Relative probability of selecting this pattern (0.0-1.0)
    """
    name: str
    token_range: Tuple[int, int]
    operations: List[int]
    weight: float = 1.0


class WorkloadGenerator:
    """
    Generates varied inference workloads for stability testing.

    Creates realistic token sequences with varying lengths and hardware
    operations to simulate production workload diversity. Supports both
    deterministic (seeded) and randomized generation.

    Example:
        generator = WorkloadGenerator(seed=42)  # Deterministic
        tokens = generator.generate()

        generator_random = WorkloadGenerator()  # Random
        tokens = generator_random.generate()
    """

    # Hardware token ranges (from EMBODIOSInferenceEngine)
    GPIO_TOKENS = [32000, 32001, 32002, 32003]
    MEMORY_TOKENS = [32010, 32011, 32012]
    I2C_TOKENS = [32020, 32021]
    SPI_TOKENS = [32022]
    UART_TOKENS = [32023, 32024]
    INTERRUPT_TOKENS = [32030, 32031]
    SYSTEM_TOKENS = [32032]

    def __init__(self, seed: int = None):
        """
        Initialize workload generator.

        Args:
            seed: Random seed for deterministic generation (None for random)
        """
        self.seed = seed
        self.rng = random.Random(seed)

        # Define workload patterns simulating real-world scenarios
        self.patterns = [
            WorkloadPattern(
                name="gpio_control",
                token_range=(5, 15),
                operations=self.GPIO_TOKENS,
                weight=0.3
            ),
            WorkloadPattern(
                name="sensor_read_i2c",
                token_range=(10, 25),
                operations=self.I2C_TOKENS + self.MEMORY_TOKENS,
                weight=0.25
            ),
            WorkloadPattern(
                name="uart_communication",
                token_range=(15, 40),
                operations=self.UART_TOKENS + self.MEMORY_TOKENS,
                weight=0.15
            ),
            WorkloadPattern(
                name="complex_control",
                token_range=(30, 60),
                operations=(self.GPIO_TOKENS + self.I2C_TOKENS +
                           self.MEMORY_TOKENS + self.INTERRUPT_TOKENS),
                weight=0.15
            ),
            WorkloadPattern(
                name="memory_operations",
                token_range=(8, 20),
                operations=self.MEMORY_TOKENS,
                weight=0.1
            ),
            WorkloadPattern(
                name="simple_query",
                token_range=(3, 8),
                operations=[],  # No special hardware tokens
                weight=0.05
            ),
        ]

    def generate(self, pattern_name: str = None) -> List[int]:
        """
        Generate a single inference workload.

        Args:
            pattern_name: Specific pattern to use (None for random selection)

        Returns:
            List of input tokens for inference
        """
        # Select pattern
        if pattern_name:
            pattern = next((p for p in self.patterns if p.name == pattern_name), None)
            if not pattern:
                raise ValueError(f"Unknown pattern: {pattern_name}")
        else:
            pattern = self._select_pattern()

        # Generate token sequence
        num_tokens = self.rng.randint(*pattern.token_range)
        tokens = []

        # Add regular tokens (1-1000 range for typical vocabulary)
        for _ in range(num_tokens):
            if pattern.operations and self.rng.random() < 0.3:
                # 30% chance to insert hardware operation token
                tokens.append(self.rng.choice(pattern.operations))
            else:
                # Regular vocabulary token
                tokens.append(self.rng.randint(1, 1000))

        return tokens

    def generate_batch(self, count: int, pattern_name: str = None) -> List[List[int]]:
        """
        Generate multiple workloads.

        Args:
            count: Number of workloads to generate
            pattern_name: Specific pattern to use (None for random)

        Returns:
            List of token sequences
        """
        return [self.generate(pattern_name) for _ in range(count)]

    def _select_pattern(self) -> WorkloadPattern:
        """
        Select a pattern based on weights.

        Returns:
            Selected WorkloadPattern
        """
        total_weight = sum(p.weight for p in self.patterns)
        r = self.rng.random() * total_weight

        cumulative = 0.0
        for pattern in self.patterns:
            cumulative += pattern.weight
            if r <= cumulative:
                return pattern

        # Fallback to last pattern
        return self.patterns[-1]

    def get_pattern_names(self) -> List[str]:
        """
        Get list of available pattern names.

        Returns:
            List of pattern names
        """
        return [p.name for p in self.patterns]

    def get_statistics(self, samples: int = 1000) -> Dict[str, any]:
        """
        Generate statistics about workload distribution.

        Args:
            samples: Number of samples to analyze

        Returns:
            Dictionary with statistics:
                - pattern_distribution: count per pattern
                - avg_token_length: average token sequence length
                - min_token_length: minimum token sequence length
                - max_token_length: maximum token sequence length
        """
        pattern_counts = {p.name: 0 for p in self.patterns}
        token_lengths = []

        # Track which pattern was selected
        original_seed = self.seed
        test_rng = random.Random(original_seed)

        for _ in range(samples):
            # Select pattern using same logic as generate()
            total_weight = sum(p.weight for p in self.patterns)
            r = test_rng.random() * total_weight

            cumulative = 0.0
            selected_pattern = None
            for pattern in self.patterns:
                cumulative += pattern.weight
                if r <= cumulative:
                    selected_pattern = pattern
                    break

            if selected_pattern is None:
                selected_pattern = self.patterns[-1]

            pattern_counts[selected_pattern.name] += 1

            # Generate token length
            token_length = test_rng.randint(*selected_pattern.token_range)
            token_lengths.append(token_length)

        return {
            'pattern_distribution': pattern_counts,
            'avg_token_length': sum(token_lengths) / len(token_lengths) if token_lengths else 0,
            'min_token_length': min(token_lengths) if token_lengths else 0,
            'max_token_length': max(token_lengths) if token_lengths else 0,
            'total_samples': samples
        }
