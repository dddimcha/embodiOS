#!/usr/bin/env python3
"""
Report generation for stability test results.

Generates HTML and Markdown reports with time-series visualizations, pass/fail
verdicts, and detailed drift analysis. Reports include interactive charts showing
metrics evolution over time and comprehensive summary tables.
"""

import json
import statistics
from pathlib import Path
from typing import List, Dict, Any, Optional, Union
from datetime import datetime
from dataclasses import dataclass

from tests.stability.storage import MetricPoint, MetricsStorage
from tests.stability.analysis import DriftAnalyzer, DriftResult, DriftStatus


@dataclass
class ReportConfig:
    """
    Configuration for report generation.

    Attributes:
        title: Report title
        format: Output format ('html', 'markdown', or 'both')
        include_charts: Whether to include time-series charts
        chart_width: Width of charts in pixels
        chart_height: Height of charts in pixels
    """

    title: str = "Stability Test Report"
    format: str = "html"
    include_charts: bool = True
    chart_width: int = 800
    chart_height: int = 400


class StabilityReport:
    """
    Generator for stability test reports with visualizations.

    Creates comprehensive reports showing test results, drift analysis, and
    time-series metrics. Supports HTML and Markdown output formats with
    optional inline charts.

    Example:
        report = StabilityReport()
        storage = MetricsStorage()
        analyzer = DriftAnalyzer()

        # Add analysis results
        report.add_drift_result('memory', memory_drift_result)
        report.add_drift_result('latency', latency_drift_result)

        # Generate report
        html_path = report.generate_html(storage, 'report.html')
        print(f"Report saved to {html_path}")
    """

    def __init__(self, config: Optional[ReportConfig] = None):
        """
        Initialize stability report generator.

        Args:
            config: Report configuration (default: ReportConfig())
        """
        self.config = config or ReportConfig()
        self.drift_results: Dict[str, DriftResult] = {}
        self.test_metadata: Dict[str, Any] = {}

    def add_drift_result(self, metric_name: str, result: DriftResult) -> None:
        """
        Add drift analysis result to report.

        Args:
            metric_name: Name of the metric
            result: DriftResult from analysis
        """
        self.drift_results[metric_name] = result

    def set_metadata(self, metadata: Dict[str, Any]) -> None:
        """
        Set test metadata for report header.

        Args:
            metadata: Dictionary with test details (duration, start_time, etc.)
        """
        self.test_metadata = metadata

    def generate_html(
        self,
        storage: MetricsStorage,
        output_path: Union[str, Path],
        test_summary: Optional[Dict[str, Any]] = None
    ) -> Path:
        """
        Generate HTML report with charts.

        Args:
            storage: MetricsStorage with test metrics
            output_path: Path to save HTML report
            test_summary: Optional summary data from test run

        Returns:
            Path to generated HTML file
        """
        output_path = Path(output_path)

        # Build HTML content
        html = self._build_html_header()
        html += self._build_summary_section(test_summary or {})
        html += self._build_drift_analysis_section()
        html += self._build_metrics_section(storage)

        if self.config.include_charts:
            html += self._build_charts_section(storage)

        html += self._build_html_footer()

        # Write to file
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(html)

        return output_path

    def generate_markdown(
        self,
        storage: MetricsStorage,
        output_path: Union[str, Path],
        test_summary: Optional[Dict[str, Any]] = None
    ) -> Path:
        """
        Generate Markdown report.

        Args:
            storage: MetricsStorage with test metrics
            output_path: Path to save Markdown report
            test_summary: Optional summary data from test run

        Returns:
            Path to generated Markdown file
        """
        output_path = Path(output_path)

        # Build Markdown content
        md = self._build_markdown_header()
        md += self._build_summary_section_md(test_summary or {})
        md += self._build_drift_analysis_section_md()
        md += self._build_metrics_section_md(storage)

        # Write to file
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(md)

        return output_path

    def _build_html_header(self) -> str:
        """Build HTML document header with styles"""
        return f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>{self.config.title}</title>
    <style>
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            line-height: 1.6;
            max-width: 1200px;
            margin: 0 auto;
            padding: 20px;
            background: #f5f5f5;
        }}
        .header {{
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 30px;
            border-radius: 10px;
            margin-bottom: 30px;
        }}
        .section {{
            background: white;
            padding: 25px;
            margin-bottom: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        .status-badge {{
            display: inline-block;
            padding: 5px 15px;
            border-radius: 20px;
            font-weight: bold;
            font-size: 14px;
        }}
        .status-pass {{ background: #10b981; color: white; }}
        .status-fail {{ background: #ef4444; color: white; }}
        .status-warning {{ background: #f59e0b; color: white; }}
        .metric-table {{
            width: 100%;
            border-collapse: collapse;
            margin: 15px 0;
        }}
        .metric-table th {{
            background: #f3f4f6;
            padding: 12px;
            text-align: left;
            font-weight: 600;
            border-bottom: 2px solid #e5e7eb;
        }}
        .metric-table td {{
            padding: 10px 12px;
            border-bottom: 1px solid #e5e7eb;
        }}
        .metric-table tr:hover {{
            background: #f9fafb;
        }}
        .chart-container {{
            margin: 20px 0;
            padding: 15px;
            background: #f9fafb;
            border-radius: 5px;
        }}
        .drift-critical {{ color: #ef4444; font-weight: bold; }}
        .drift-warning {{ color: #f59e0b; font-weight: bold; }}
        .drift-stable {{ color: #10b981; font-weight: bold; }}
        h1 {{ margin: 0 0 10px 0; }}
        h2 {{ color: #1f2937; margin-top: 0; }}
        .metadata {{ opacity: 0.9; font-size: 14px; }}
    </style>
</head>
<body>
"""

    def _build_html_footer(self) -> str:
        """Build HTML document footer"""
        return """</body>
</html>
"""

    def _build_summary_section(self, test_summary: Dict[str, Any]) -> str:
        """Build HTML summary section"""
        passed = test_summary.get('passed', False)
        status_class = 'status-pass' if passed else 'status-fail'
        status_text = 'PASS ✓' if passed else 'FAIL ✗'

        duration = test_summary.get('duration_seconds', 0)
        duration_str = self._format_duration(duration)

        html = f"""
<div class="header">
    <h1>{self.config.title}</h1>
    <div class="metadata">
        <p>Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>
        <p>Test Duration: {duration_str}</p>
        <p>Status: <span class="{status_class}">{status_text}</span></p>
    </div>
</div>
"""
        return html

    def _build_drift_analysis_section(self) -> str:
        """Build HTML drift analysis section"""
        if not self.drift_results:
            return ""

        html = '<div class="section"><h2>📊 Drift Analysis</h2>'
        html += '<table class="metric-table">'
        html += '<tr><th>Metric</th><th>Status</th><th>Drift</th><th>Threshold</th><th>Confidence</th><th>Message</th></tr>'

        for metric_name, result in self.drift_results.items():
            status_class = f"drift-{result.status.value}"
            status_icon = {
                DriftStatus.STABLE: "✓",
                DriftStatus.WARNING: "⚠",
                DriftStatus.CRITICAL: "✗"
            }.get(result.status, "?")

            html += f"""
<tr>
    <td><strong>{metric_name}</strong></td>
    <td class="{status_class}">{result.status.value.upper()} {status_icon}</td>
    <td>{result.drift_percentage:.1f}%</td>
    <td>{result.threshold_percentage:.1f}%</td>
    <td>{result.confidence:.2f}</td>
    <td>{result.message}</td>
</tr>
"""

        html += '</table></div>'
        return html

    def _build_metrics_section(self, storage: MetricsStorage) -> str:
        """Build HTML metrics section"""
        summary = storage.get_summary()

        html = '<div class="section"><h2>📈 Metrics Summary</h2>'
        html += f'<p><strong>Total Data Points:</strong> {summary["count"]}</p>'
        html += f'<p><strong>Metric Types:</strong> {", ".join(summary["metric_names"])}</p>'

        if summary['time_range']:
            duration = summary['time_range']['duration']
            html += f'<p><strong>Collection Duration:</strong> {self._format_duration(duration)}</p>'

        # Per-metric statistics
        html += '<h3>Metric Statistics</h3>'
        html += '<table class="metric-table">'
        html += '<tr><th>Metric</th><th>Count</th><th>Min</th><th>Max</th><th>Mean</th><th>Median</th></tr>'

        for metric_name in summary['metric_names']:
            metrics = storage.get_metrics(metric_name)
            if metrics:
                values = [m.value for m in metrics]
                html += f"""
<tr>
    <td><strong>{metric_name}</strong></td>
    <td>{len(values)}</td>
    <td>{min(values):.2f}</td>
    <td>{max(values):.2f}</td>
    <td>{statistics.mean(values):.2f}</td>
    <td>{statistics.median(values):.2f}</td>
</tr>
"""

        html += '</table></div>'
        return html

    def _build_charts_section(self, storage: MetricsStorage) -> str:
        """Build HTML charts section with ASCII charts"""
        html = '<div class="section"><h2>📉 Time-Series Visualizations</h2>'

        metric_names = storage.get_metric_names()
        for metric_name in metric_names:
            metrics = storage.get_metrics(metric_name)
            if metrics:
                html += f'<div class="chart-container">'
                html += f'<h3>{metric_name}</h3>'
                html += self._generate_ascii_chart(metrics)
                html += '</div>'

        html += '</div>'
        return html

    def _generate_ascii_chart(self, metrics: List[MetricPoint]) -> str:
        """Generate ASCII chart for metrics"""
        if not metrics or len(metrics) < 2:
            return '<p>Insufficient data for chart</p>'

        values = [m.value for m in metrics]
        timestamps = [m.timestamp for m in metrics]

        # Normalize values for display
        min_val = min(values)
        max_val = max(values)
        value_range = max_val - min_val if max_val > min_val else 1

        # Build ASCII chart (10 rows)
        chart_rows = []
        chart_height = 10
        chart_width = min(80, len(values))

        # Sample data points if too many
        if len(values) > chart_width:
            step = len(values) // chart_width
            sampled_values = [values[i] for i in range(0, len(values), step)][:chart_width]
        else:
            sampled_values = values

        # Build chart rows (top to bottom)
        for row in range(chart_height):
            threshold = max_val - (row / chart_height) * value_range
            line = ""
            for val in sampled_values:
                if val >= threshold:
                    line += "█"
                else:
                    line += " "
            chart_rows.append(line)

        # Format chart
        chart_html = '<pre style="font-family: monospace; line-height: 1.2;">\n'
        chart_html += f'Max: {max_val:.2f} ┤\n'
        for row in chart_rows:
            chart_html += f'          │{row}\n'
        chart_html += f'Min: {min_val:.2f} ┤\n'
        chart_html += f'          └{"─" * len(sampled_values)}\n'
        chart_html += f'           {len(metrics)} data points\n'
        chart_html += '</pre>'

        return chart_html

    def _build_markdown_header(self) -> str:
        """Build Markdown document header"""
        return f"# {self.config.title}\n\n"

    def _build_summary_section_md(self, test_summary: Dict[str, Any]) -> str:
        """Build Markdown summary section"""
        passed = test_summary.get('passed', False)
        status_text = 'PASS ✓' if passed else 'FAIL ✗'

        duration = test_summary.get('duration_seconds', 0)
        duration_str = self._format_duration(duration)

        md = "## Summary\n\n"
        md += f"- **Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n"
        md += f"- **Test Duration:** {duration_str}\n"
        md += f"- **Status:** {status_text}\n\n"

        return md

    def _build_drift_analysis_section_md(self) -> str:
        """Build Markdown drift analysis section"""
        if not self.drift_results:
            return ""

        md = "## Drift Analysis\n\n"
        md += "| Metric | Status | Drift | Threshold | Confidence | Message |\n"
        md += "|--------|--------|-------|-----------|------------|--------|\n"

        for metric_name, result in self.drift_results.items():
            status_icon = {
                DriftStatus.STABLE: "✓",
                DriftStatus.WARNING: "⚠",
                DriftStatus.CRITICAL: "✗"
            }.get(result.status, "?")

            md += f"| {metric_name} | {result.status.value.upper()} {status_icon} | "
            md += f"{result.drift_percentage:.1f}% | {result.threshold_percentage:.1f}% | "
            md += f"{result.confidence:.2f} | {result.message} |\n"

        md += "\n"
        return md

    def _build_metrics_section_md(self, storage: MetricsStorage) -> str:
        """Build Markdown metrics section"""
        summary = storage.get_summary()

        md = "## Metrics Summary\n\n"
        md += f"- **Total Data Points:** {summary['count']}\n"
        md += f"- **Metric Types:** {', '.join(summary['metric_names'])}\n"

        if summary['time_range']:
            duration = summary['time_range']['duration']
            md += f"- **Collection Duration:** {self._format_duration(duration)}\n"

        md += "\n### Metric Statistics\n\n"
        md += "| Metric | Count | Min | Max | Mean | Median |\n"
        md += "|--------|-------|-----|-----|------|--------|\n"

        for metric_name in summary['metric_names']:
            metrics = storage.get_metrics(metric_name)
            if metrics:
                values = [m.value for m in metrics]
                md += f"| {metric_name} | {len(values)} | "
                md += f"{min(values):.2f} | {max(values):.2f} | "
                md += f"{statistics.mean(values):.2f} | {statistics.median(values):.2f} |\n"

        md += "\n"
        return md

    def _format_duration(self, seconds: float) -> str:
        """Format duration in human-readable format"""
        if seconds < 60:
            return f"{seconds:.1f}s"
        elif seconds < 3600:
            minutes = seconds / 60
            return f"{minutes:.1f}m"
        else:
            hours = seconds / 3600
            return f"{hours:.2f}h"

    def get_pass_fail_verdict(self) -> Dict[str, Any]:
        """
        Get overall pass/fail verdict based on drift results.

        Returns:
            Dictionary with 'passed' boolean and 'message' string
        """
        if not self.drift_results:
            return {
                'passed': False,
                'message': 'No drift analysis results available'
            }

        critical_count = sum(1 for r in self.drift_results.values()
                             if r.status == DriftStatus.CRITICAL)

        if critical_count > 0:
            return {
                'passed': False,
                'message': f'Test FAILED: {critical_count} critical issue(s) detected'
            }

        return {
            'passed': True,
            'message': 'Test PASSED: All metrics within acceptable thresholds'
        }
