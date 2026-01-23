#!/usr/bin/env python3
"""
Data persistence module for time-series metrics.

Provides storage and retrieval of metrics data for long-running stability tests.
Supports JSON and CSV formats for post-run analysis and reporting.
"""

import json
import csv
import time
from pathlib import Path
from dataclasses import dataclass, asdict
from typing import List, Optional, Dict, Any, Union
from datetime import datetime


@dataclass
class MetricPoint:
    """
    Single metric data point with timestamp.

    Attributes:
        timestamp: Time of measurement (Unix timestamp)
        metric_name: Name of the metric (e.g., 'memory_rss', 'latency_p99')
        value: Numeric value of the metric
        metadata: Optional additional context (tags, labels, etc.)
    """

    timestamp: float
    metric_name: str
    value: float
    metadata: Optional[Dict[str, Any]] = None

    def to_dict(self) -> Dict[str, Any]:
        """Convert metric point to dictionary"""
        return asdict(self)

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> 'MetricPoint':
        """Create metric point from dictionary"""
        return cls(**data)


class MetricsStorage:
    """
    Time-series metrics storage for stability test data.

    Stores metrics to disk in JSON or CSV format for post-run analysis.
    Supports appending new metrics and bulk retrieval for analysis.

    Example:
        storage = MetricsStorage(output_dir='/tmp/stability')
        storage.record('memory_rss', 123.45, metadata={'unit': 'MB'})
        storage.save_json('metrics.json')

    Attributes:
        output_dir: Directory for storing metrics files
        metrics: List of recorded metric points
    """

    def __init__(self, output_dir: Optional[Union[str, Path]] = None):
        """
        Initialize metrics storage.

        Args:
            output_dir: Directory for output files (default: current directory)
        """
        self.output_dir = Path(output_dir) if output_dir else Path.cwd()
        self.metrics: List[MetricPoint] = []

        # Create output directory if it doesn't exist
        if output_dir:
            self.output_dir.mkdir(parents=True, exist_ok=True)

    def record(
        self,
        metric_name: str,
        value: float,
        timestamp: Optional[float] = None,
        metadata: Optional[Dict[str, Any]] = None
    ) -> MetricPoint:
        """
        Record a single metric point.

        Args:
            metric_name: Name of the metric
            value: Numeric value
            timestamp: Time of measurement (default: current time)
            metadata: Optional additional context

        Returns:
            The recorded MetricPoint
        """
        if timestamp is None:
            timestamp = time.time()

        point = MetricPoint(
            timestamp=timestamp,
            metric_name=metric_name,
            value=value,
            metadata=metadata
        )

        self.metrics.append(point)
        return point

    def record_batch(self, metrics: List[Dict[str, Any]]) -> int:
        """
        Record multiple metrics at once.

        Args:
            metrics: List of metric dictionaries with 'name', 'value', optional 'timestamp', 'metadata'

        Returns:
            Number of metrics recorded
        """
        count = 0
        for metric in metrics:
            self.record(
                metric_name=metric['name'],
                value=metric['value'],
                timestamp=metric.get('timestamp'),
                metadata=metric.get('metadata')
            )
            count += 1

        return count

    def get_metrics(
        self,
        metric_name: Optional[str] = None,
        start_time: Optional[float] = None,
        end_time: Optional[float] = None
    ) -> List[MetricPoint]:
        """
        Retrieve metrics with optional filtering.

        Args:
            metric_name: Filter by metric name (default: all metrics)
            start_time: Filter by start timestamp (inclusive)
            end_time: Filter by end timestamp (inclusive)

        Returns:
            List of matching MetricPoint objects
        """
        filtered = self.metrics

        if metric_name:
            filtered = [m for m in filtered if m.metric_name == metric_name]

        if start_time is not None:
            filtered = [m for m in filtered if m.timestamp >= start_time]

        if end_time is not None:
            filtered = [m for m in filtered if m.timestamp <= end_time]

        return filtered

    def get_metric_names(self) -> List[str]:
        """
        Get list of unique metric names in storage.

        Returns:
            Sorted list of metric names
        """
        names = set(m.metric_name for m in self.metrics)
        return sorted(names)

    def clear(self) -> None:
        """Clear all stored metrics"""
        self.metrics.clear()

    def count(self) -> int:
        """
        Get total number of stored metrics.

        Returns:
            Number of metric points
        """
        return len(self.metrics)

    def save_json(self, filename: Optional[Union[str, Path]] = None) -> Path:
        """
        Save metrics to JSON file.

        Args:
            filename: Output filename (default: 'metrics_{timestamp}.json')

        Returns:
            Path to saved file
        """
        if filename is None:
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            filename = f'metrics_{timestamp}.json'

        filepath = self.output_dir / filename

        data = {
            'metadata': {
                'num_metrics': len(self.metrics),
                'metric_names': self.get_metric_names(),
                'time_range': self._get_time_range(),
                'saved_at': time.time()
            },
            'metrics': [m.to_dict() for m in self.metrics]
        }

        with open(filepath, 'w') as f:
            json.dump(data, f, indent=2)

        return filepath

    def save_csv(self, filename: Optional[Union[str, Path]] = None) -> Path:
        """
        Save metrics to CSV file.

        CSV format: timestamp, metric_name, value, metadata_json

        Args:
            filename: Output filename (default: 'metrics_{timestamp}.csv')

        Returns:
            Path to saved file
        """
        if filename is None:
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            filename = f'metrics_{timestamp}.csv'

        filepath = self.output_dir / filename

        with open(filepath, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['timestamp', 'metric_name', 'value', 'metadata'])

            for metric in self.metrics:
                metadata_str = json.dumps(metric.metadata) if metric.metadata else ''
                writer.writerow([
                    metric.timestamp,
                    metric.metric_name,
                    metric.value,
                    metadata_str
                ])

        return filepath

    def load_json(self, filepath: Union[str, Path]) -> int:
        """
        Load metrics from JSON file.

        Args:
            filepath: Path to JSON file to load

        Returns:
            Number of metrics loaded
        """
        filepath = Path(filepath)

        with open(filepath, 'r') as f:
            data = json.load(f)

        # Clear existing metrics before loading
        self.metrics.clear()

        # Load metrics from file
        for metric_data in data.get('metrics', []):
            point = MetricPoint.from_dict(metric_data)
            self.metrics.append(point)

        return len(self.metrics)

    def load_csv(self, filepath: Union[str, Path]) -> int:
        """
        Load metrics from CSV file.

        Args:
            filepath: Path to CSV file to load

        Returns:
            Number of metrics loaded
        """
        filepath = Path(filepath)

        # Clear existing metrics before loading
        self.metrics.clear()

        with open(filepath, 'r', newline='') as f:
            reader = csv.DictReader(f)

            for row in reader:
                metadata = None
                if row['metadata']:
                    try:
                        metadata = json.loads(row['metadata'])
                    except json.JSONDecodeError:
                        pass

                point = MetricPoint(
                    timestamp=float(row['timestamp']),
                    metric_name=row['metric_name'],
                    value=float(row['value']),
                    metadata=metadata
                )
                self.metrics.append(point)

        return len(self.metrics)

    def _get_time_range(self) -> Optional[Dict[str, float]]:
        """
        Get time range of stored metrics.

        Returns:
            Dictionary with 'start' and 'end' timestamps, or None if no metrics
        """
        if not self.metrics:
            return None

        timestamps = [m.timestamp for m in self.metrics]
        return {
            'start': min(timestamps),
            'end': max(timestamps),
            'duration': max(timestamps) - min(timestamps)
        }

    def get_summary(self) -> Dict[str, Any]:
        """
        Get summary statistics of stored metrics.

        Returns:
            Dictionary with count, metric names, and time range
        """
        return {
            'count': len(self.metrics),
            'metric_names': self.get_metric_names(),
            'time_range': self._get_time_range(),
            'output_dir': str(self.output_dir)
        }
