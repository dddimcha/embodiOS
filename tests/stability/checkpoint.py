#!/usr/bin/env python3
"""
Checkpoint and resume support for long-running stability tests.

Provides graceful shutdown handling and state persistence to allow
72-hour tests to be paused and resumed without losing progress.
"""

import json
import signal
import time
from pathlib import Path
from dataclasses import dataclass, asdict
from typing import Optional, Dict, Any, List, Callable
from datetime import datetime


@dataclass
class CheckpointData:
    """
    Checkpoint state for resumable stability test.

    Attributes:
        test_name: Name of the test being run
        config_dict: Serialized StabilityConfig parameters
        start_time: Original test start time (Unix timestamp)
        elapsed_time: Total elapsed time so far (seconds)
        requests_processed: Number of inference requests completed
        warmup_completed: Whether warmup phase is complete
        baseline_snapshot_dict: Serialized baseline ResourceSnapshot
        latencies: List of recorded latencies (last 1000 kept)
        metrics_data: Serialized metrics from MetricsStorage
        checkpoint_time: When this checkpoint was created
        checkpoint_count: Number of checkpoints created so far
        resume_count: Number of times test has been resumed
    """

    test_name: str
    config_dict: Dict[str, Any]
    start_time: float
    elapsed_time: float
    requests_processed: int
    warmup_completed: bool
    baseline_snapshot_dict: Optional[Dict[str, Any]]
    latencies: List[float]
    metrics_data: List[Dict[str, Any]]
    checkpoint_time: float
    checkpoint_count: int
    resume_count: int

    def to_dict(self) -> Dict[str, Any]:
        """Convert checkpoint to dictionary"""
        return asdict(self)

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> 'CheckpointData':
        """Create checkpoint from dictionary"""
        return cls(**data)


class CheckpointManager:
    """
    Manages checkpoint creation, saving, and loading for stability tests.

    Handles graceful shutdown on SIGTERM/SIGINT and periodic checkpoint
    saves for long-running tests. Checkpoint files are saved in JSON format
    with metadata for resumption.

    Example:
        manager = CheckpointManager(checkpoint_dir='/tmp/stability')
        manager.setup_signal_handlers(on_shutdown_callback)

        # During test
        manager.save_checkpoint(checkpoint_data)

        # On resume
        checkpoint = manager.load_latest_checkpoint('test_72h')
        if checkpoint:
            # Resume test from checkpoint state
            ...

    Attributes:
        checkpoint_dir: Directory for storing checkpoint files
        shutdown_requested: Flag indicating if graceful shutdown was requested
        last_checkpoint_time: Timestamp of last checkpoint save
    """

    def __init__(self, checkpoint_dir: Optional[Path] = None):
        """
        Initialize checkpoint manager.

        Args:
            checkpoint_dir: Directory for checkpoint files (default: ./checkpoints)
        """
        self.checkpoint_dir = checkpoint_dir or Path('./checkpoints')
        self.checkpoint_dir.mkdir(parents=True, exist_ok=True)

        self.shutdown_requested = False
        self.last_checkpoint_time: Optional[float] = None
        self._shutdown_callback: Optional[Callable] = None
        self._original_sigterm_handler = None
        self._original_sigint_handler = None

    def setup_signal_handlers(self, on_shutdown: Optional[Callable] = None) -> None:
        """
        Setup signal handlers for graceful shutdown.

        Registers handlers for SIGTERM and SIGINT (Ctrl+C) to enable
        graceful shutdown with checkpoint save before exit.

        Args:
            on_shutdown: Optional callback to invoke on shutdown signal
        """
        self._shutdown_callback = on_shutdown

        # Store original handlers
        self._original_sigterm_handler = signal.signal(signal.SIGTERM, self._handle_shutdown_signal)
        self._original_sigint_handler = signal.signal(signal.SIGINT, self._handle_shutdown_signal)

    def restore_signal_handlers(self) -> None:
        """Restore original signal handlers"""
        if self._original_sigterm_handler is not None:
            signal.signal(signal.SIGTERM, self._original_sigterm_handler)
        if self._original_sigint_handler is not None:
            signal.signal(signal.SIGINT, self._original_sigint_handler)

    def _handle_shutdown_signal(self, signum: int, frame) -> None:
        """
        Handle shutdown signal (SIGTERM/SIGINT).

        Sets shutdown flag and invokes callback if configured.

        Args:
            signum: Signal number
            frame: Current stack frame
        """
        signal_name = 'SIGTERM' if signum == signal.SIGTERM else 'SIGINT'
        print(f"\n[Checkpoint] Received {signal_name}, initiating graceful shutdown...")

        self.shutdown_requested = True

        if self._shutdown_callback:
            try:
                self._shutdown_callback()
            except Exception as e:
                print(f"[Checkpoint] Error in shutdown callback: {e}")

    def save_checkpoint(
        self,
        checkpoint_data: CheckpointData,
        filename: Optional[str] = None
    ) -> Path:
        """
        Save checkpoint to disk.

        Args:
            checkpoint_data: Checkpoint state to save
            filename: Optional custom filename (default: auto-generated)

        Returns:
            Path to saved checkpoint file
        """
        if filename is None:
            # Generate filename: test_name_YYYYMMDD_HHMMSS.json
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            test_name_safe = checkpoint_data.test_name.replace(' ', '_').replace('/', '_')
            filename = f'{test_name_safe}_{timestamp}.json'

        filepath = self.checkpoint_dir / filename

        # Convert to dictionary and add metadata
        data = checkpoint_data.to_dict()
        data['_checkpoint_metadata'] = {
            'version': '1.0',
            'saved_at': time.time(),
            'saved_at_iso': datetime.now().isoformat(),
        }

        # Save to file
        with open(filepath, 'w') as f:
            json.dump(data, f, indent=2)

        self.last_checkpoint_time = time.time()

        print(f"[Checkpoint] Saved to {filepath}")
        return filepath

    def load_checkpoint(self, filepath: Path) -> CheckpointData:
        """
        Load checkpoint from file.

        Args:
            filepath: Path to checkpoint file

        Returns:
            CheckpointData loaded from file

        Raises:
            FileNotFoundError: If checkpoint file doesn't exist
            ValueError: If checkpoint file is invalid
        """
        filepath = Path(filepath)

        if not filepath.exists():
            raise FileNotFoundError(f"Checkpoint file not found: {filepath}")

        with open(filepath, 'r') as f:
            data = json.load(f)

        # Remove metadata before creating CheckpointData
        data.pop('_checkpoint_metadata', None)

        try:
            checkpoint = CheckpointData.from_dict(data)
            print(f"[Checkpoint] Loaded from {filepath}")
            print(f"[Checkpoint] Test: {checkpoint.test_name}, "
                  f"Elapsed: {checkpoint.elapsed_time:.1f}s, "
                  f"Requests: {checkpoint.requests_processed}")
            return checkpoint
        except Exception as e:
            raise ValueError(f"Invalid checkpoint file: {e}")

    def load_latest_checkpoint(self, test_name: str) -> Optional[CheckpointData]:
        """
        Load the most recent checkpoint for a test.

        Args:
            test_name: Name of the test to load checkpoint for

        Returns:
            CheckpointData for the latest checkpoint, or None if not found
        """
        # Find all checkpoints for this test
        test_name_safe = test_name.replace(' ', '_').replace('/', '_')
        pattern = f'{test_name_safe}_*.json'

        checkpoints = list(self.checkpoint_dir.glob(pattern))

        if not checkpoints:
            return None

        # Sort by modification time and get the latest
        latest = max(checkpoints, key=lambda p: p.stat().st_mtime)

        try:
            return self.load_checkpoint(latest)
        except Exception as e:
            print(f"[Checkpoint] Failed to load checkpoint {latest}: {e}")
            return None

    def list_checkpoints(self, test_name: Optional[str] = None) -> List[Path]:
        """
        List available checkpoint files.

        Args:
            test_name: Optional test name to filter by

        Returns:
            List of checkpoint file paths, sorted by modification time (newest first)
        """
        if test_name:
            test_name_safe = test_name.replace(' ', '_').replace('/', '_')
            pattern = f'{test_name_safe}_*.json'
        else:
            pattern = '*.json'

        checkpoints = list(self.checkpoint_dir.glob(pattern))
        checkpoints.sort(key=lambda p: p.stat().st_mtime, reverse=True)

        return checkpoints

    def delete_checkpoint(self, filepath: Path) -> bool:
        """
        Delete a checkpoint file.

        Args:
            filepath: Path to checkpoint file to delete

        Returns:
            True if deleted successfully, False otherwise
        """
        try:
            filepath = Path(filepath)
            if filepath.exists():
                filepath.unlink()
                print(f"[Checkpoint] Deleted {filepath}")
                return True
            return False
        except Exception as e:
            print(f"[Checkpoint] Failed to delete {filepath}: {e}")
            return False

    def cleanup_old_checkpoints(
        self,
        test_name: str,
        keep_count: int = 5
    ) -> int:
        """
        Delete old checkpoints, keeping only the most recent ones.

        Args:
            test_name: Test name to clean up checkpoints for
            keep_count: Number of recent checkpoints to keep

        Returns:
            Number of checkpoints deleted
        """
        checkpoints = self.list_checkpoints(test_name)

        if len(checkpoints) <= keep_count:
            return 0

        # Delete oldest checkpoints
        to_delete = checkpoints[keep_count:]
        deleted_count = 0

        for checkpoint_path in to_delete:
            if self.delete_checkpoint(checkpoint_path):
                deleted_count += 1

        return deleted_count

    def should_save_checkpoint(
        self,
        interval_seconds: int,
        force: bool = False
    ) -> bool:
        """
        Check if it's time to save a checkpoint.

        Args:
            interval_seconds: Minimum time between checkpoints
            force: Force checkpoint regardless of interval

        Returns:
            True if checkpoint should be saved
        """
        if force or self.shutdown_requested:
            return True

        if interval_seconds <= 0:
            return False

        if self.last_checkpoint_time is None:
            return True

        elapsed = time.time() - self.last_checkpoint_time
        return elapsed >= interval_seconds
