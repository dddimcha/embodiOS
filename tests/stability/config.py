#!/usr/bin/env python3
"""
Configuration module for stability tests.

Defines test durations, thresholds, and monitoring intervals for
long-running stability testing of EMBODIOS.
"""

from dataclasses import dataclass
from typing import Optional


@dataclass
class StabilityConfig:
    """
    Configuration for stability test parameters and thresholds.

    This class defines the parameters for running stability tests including
    test durations, acceptance thresholds, and monitoring intervals.

    Attributes:
        duration_seconds: Total test duration in seconds
        monitoring_interval_seconds: How often to collect metrics
        memory_drift_threshold_pct: Maximum allowed memory growth percentage
        latency_degradation_threshold_pct: Maximum allowed latency increase percentage
        min_requests: Minimum number of requests to process during test
        warmup_requests: Number of warmup requests before baseline measurement
        checkpoint_interval_seconds: How often to save checkpoint (0 = disabled)
    """

    # Test duration
    duration_seconds: int = 3600  # 1 hour default

    # Monitoring settings
    monitoring_interval_seconds: int = 10  # Collect metrics every 10 seconds

    # Threshold settings (from acceptance criteria)
    memory_drift_threshold_pct: float = 5.0  # Max 5% memory growth
    latency_degradation_threshold_pct: float = 10.0  # Max 10% P99 latency increase

    # Request settings
    min_requests: int = 100  # Minimum requests to ensure meaningful test
    warmup_requests: int = 10  # Warmup before baseline

    # Checkpoint settings
    checkpoint_interval_seconds: int = 0  # Disabled by default

    @classmethod
    def short_test(cls) -> 'StabilityConfig':
        """
        Create configuration for short stability test (1 hour).

        Returns:
            StabilityConfig configured for 1-hour test
        """
        return cls(
            duration_seconds=3600,  # 1 hour
            monitoring_interval_seconds=10,
            min_requests=100,
        )

    @classmethod
    def medium_test(cls) -> 'StabilityConfig':
        """
        Create configuration for medium stability test (12 hours).

        Returns:
            StabilityConfig configured for 12-hour test
        """
        return cls(
            duration_seconds=43200,  # 12 hours
            monitoring_interval_seconds=30,  # Less frequent for longer tests
            min_requests=1000,
            checkpoint_interval_seconds=3600,  # Checkpoint every hour
        )

    @classmethod
    def long_test(cls) -> 'StabilityConfig':
        """
        Create configuration for long stability test (72 hours).

        Returns:
            StabilityConfig configured for 72-hour test (acceptance criteria)
        """
        return cls(
            duration_seconds=259200,  # 72 hours
            monitoring_interval_seconds=60,  # 1 minute intervals
            min_requests=10000,  # From acceptance criteria
            checkpoint_interval_seconds=3600,  # Checkpoint every hour
        )

    @classmethod
    def smoke_test(cls, duration_seconds: int = 60) -> 'StabilityConfig':
        """
        Create configuration for quick smoke test.

        Useful for development and CI verification. Uses more lenient
        thresholds than production tests since short tests are subject to
        higher variance from system scheduling and Python overhead.

        Args:
            duration_seconds: Test duration (default 60 seconds)

        Returns:
            StabilityConfig configured for quick smoke test
        """
        return cls(
            duration_seconds=duration_seconds,
            monitoring_interval_seconds=5,
            min_requests=10,
            warmup_requests=5,
            checkpoint_interval_seconds=0,
            # More lenient thresholds for short smoke tests
            # (system overhead and scheduling variance is proportionally higher)
            memory_drift_threshold_pct=10.0,  # 10% vs 5% for long tests
            latency_degradation_threshold_pct=20.0,  # 20% vs 10% for long tests
        )

    def validate(self) -> None:
        """
        Validate configuration parameters.

        Raises:
            ValueError: If configuration is invalid
        """
        if self.duration_seconds <= 0:
            raise ValueError("duration_seconds must be positive")

        if self.monitoring_interval_seconds <= 0:
            raise ValueError("monitoring_interval_seconds must be positive")

        if self.monitoring_interval_seconds > self.duration_seconds:
            raise ValueError("monitoring_interval_seconds cannot exceed duration_seconds")

        if self.memory_drift_threshold_pct <= 0:
            raise ValueError("memory_drift_threshold_pct must be positive")

        if self.latency_degradation_threshold_pct <= 0:
            raise ValueError("latency_degradation_threshold_pct must be positive")

        if self.min_requests <= 0:
            raise ValueError("min_requests must be positive")

        if self.warmup_requests < 0:
            raise ValueError("warmup_requests cannot be negative")

        if self.checkpoint_interval_seconds < 0:
            raise ValueError("checkpoint_interval_seconds cannot be negative")

        if self.checkpoint_interval_seconds > 0 and self.checkpoint_interval_seconds > self.duration_seconds:
            raise ValueError("checkpoint_interval_seconds cannot exceed duration_seconds")
