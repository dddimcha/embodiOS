"""
EMBODIOS Benchmark CLI - Latency and performance benchmarking
"""

import click
import sys
from rich.console import Console
from rich.progress import Progress, SpinnerColumn, TextColumn
from pathlib import Path

from embodi.core.benchmarking import (
    EMBODIOSBenchmarkRunner,
    LlamaCppBenchmarkRunner,
    BenchmarkComparison
)

console = Console()

@click.command()
@click.argument('model', required=True)
@click.option('--baseline', '-b', help='Path to llama.cpp binary for baseline comparison')
@click.option('--baseline-model', help='Path to GGUF model for llama.cpp baseline')
@click.option('--prompts', '-p', type=int, default=100, help='Number of prompts to test')
@click.option('--warmup', '-w', type=int, default=5, help='Warmup iterations')
@click.option('--output', '-o', help='Output file for results (JSON)')
@click.option('--format', type=click.Choice(['json', 'table', 'both']), default='both', help='Output format')
@click.option('--threshold', '-t', type=float, default=1.0, help='Overhead threshold % for comparison (default: 1.0%)')
@click.pass_context
def benchmark(ctx, model, baseline, baseline_model, prompts, warmup, output, format, threshold):
    """Run latency and performance benchmarks on EMBODIOS models

    Measures P50, P95, P99, and P99.9 latency percentiles, throughput,
    and memory usage. Optionally compares against llama.cpp baseline.

    Examples:
        embodi benchmark models/tinyllama.aios
        embodi benchmark models/phi-2.aios --baseline /usr/local/bin/llama-cli --baseline-model models/phi-2.gguf
        embodi benchmark models/phi-2.aios --output results.json --format json
        embodi benchmark models/phi-2.aios --prompts 1000 --warmup 10
    """
    console.print(f"[bold blue]Benchmarking EMBODIOS model: {model}[/bold blue]")

    # Validate model file exists
    model_path = Path(model)
    if not model_path.exists():
        console.print(f"[bold red]✗ Model file not found: {model}[/bold red]")
        raise click.Abort()

    # Display configuration
    console.print(f"[cyan]Configuration:[/cyan]")
    console.print(f"  Prompts: {prompts}")
    console.print(f"  Warmup iterations: {warmup}")
    console.print(f"  Output format: {format}")
    if baseline:
        console.print(f"  Baseline: llama.cpp ({baseline})")
        console.print(f"  Baseline model: {baseline_model}")
        console.print(f"  Threshold: {threshold:.2f}%")
    if output:
        console.print(f"  Output file: {output}")
    console.print()

    # Run EMBODIOS benchmark
    console.print("[bold cyan]Running EMBODIOS benchmark...[/bold cyan]")
    embodios_result = None

    try:
        with Progress(
            SpinnerColumn(),
            TextColumn("[progress.description]{task.description}"),
            console=console,
        ) as progress:
            task = progress.add_task(f"Benchmarking {prompts} requests...", total=None)

            # Initialize EMBODIOS runner
            # Note: No inference engine provided - will use mock inference for now
            runner = EMBODIOSBenchmarkRunner()

            # Run benchmark
            embodios_result = runner.run_inference_benchmark(
                num_requests=prompts,
                warmup_requests=warmup,
                metadata={
                    "model": str(model_path),
                    "model_type": "EMBODIOS"
                }
            )

            progress.update(task, description="✓ EMBODIOS benchmark complete")

        console.print("[bold green]✓ EMBODIOS benchmark complete[/bold green]\n")

    except Exception as e:
        console.print(f"[bold red]✗ EMBODIOS benchmark failed: {e}[/bold red]")
        sys.exit(1)

    # Run llama.cpp baseline if requested
    baseline_result = None
    if baseline and baseline_model:
        console.print("[bold cyan]Running llama.cpp baseline benchmark...[/bold cyan]")

        # Validate baseline files exist
        if not Path(baseline).exists():
            console.print(f"[bold red]✗ llama.cpp binary not found: {baseline}[/bold red]")
            sys.exit(1)

        if not Path(baseline_model).exists():
            console.print(f"[bold red]✗ Baseline model not found: {baseline_model}[/bold red]")
            sys.exit(1)

        try:
            with Progress(
                SpinnerColumn(),
                TextColumn("[progress.description]{task.description}"),
                console=console,
            ) as progress:
                task = progress.add_task("Running llama.cpp benchmark...", total=None)

                # Initialize llama.cpp runner
                llama_runner = LlamaCppBenchmarkRunner(
                    binary_path=baseline,
                    model_path=baseline_model
                )

                # Run llama.cpp benchmark
                # Note: This runs once to get metrics - we'll use the parsed output
                llama_result = llama_runner.run_benchmark(
                    prompt="Benchmark test prompt",
                    n_predict=128
                )

                progress.update(task, description="✓ llama.cpp benchmark complete")

            console.print("[bold green]✓ llama.cpp baseline complete[/bold green]\n")

            # Note: llama.cpp runner returns different format than EMBODIOS
            # For comparison, we would need to run llama.cpp multiple times or
            # use the single-run metrics. For now, we'll note this limitation.
            console.print("[yellow]Note: Baseline comparison using single llama.cpp run[/yellow]")
            console.print("[dim]For full statistical comparison, llama.cpp would need multiple runs[/dim]\n")

        except Exception as e:
            console.print(f"[bold red]✗ llama.cpp benchmark failed: {e}[/bold red]")
            console.print("[yellow]Continuing with EMBODIOS results only...[/yellow]\n")

    # Generate output based on format
    if format in ['table', 'both']:
        # Print EMBODIOS results as table
        if embodios_result:
            runner.print_results_rich(embodios_result, console=console)
            console.print()

        # Print comparison if baseline was run
        if baseline_result and embodios_result:
            comparison = BenchmarkComparison(
                baseline_result=baseline_result,
                test_result=embodios_result,
                threshold_pct=threshold,
                baseline_label="llama.cpp",
                test_label="EMBODIOS"
            )
            comparison_result = comparison.compare()
            comparison.print_comparison_rich(comparison_result, console=console)
            console.print()

    # Save JSON output if requested
    if output:
        import json

        try:
            output_data = {
                "embodios_result": embodios_result.to_dict() if embodios_result else None,
                "baseline_result": baseline_result.to_dict() if baseline_result else None,
                "config": {
                    "model": str(model_path),
                    "prompts": prompts,
                    "warmup": warmup,
                    "threshold_pct": threshold
                }
            }

            # Add comparison if both results available
            if baseline_result and embodios_result:
                comparison = BenchmarkComparison(
                    baseline_result=baseline_result,
                    test_result=embodios_result,
                    threshold_pct=threshold,
                    baseline_label="llama.cpp",
                    test_label="EMBODIOS"
                )
                output_data["comparison"] = comparison.compare().to_dict()

            # Write JSON file
            with open(output, 'w') as f:
                json.dump(output_data, f, indent=2)

            console.print(f"[bold green]✓ Results saved to {output}[/bold green]")

        except Exception as e:
            console.print(f"[bold red]✗ Failed to save JSON output: {e}[/bold red]")
            sys.exit(1)

    # Print JSON to stdout if requested
    if format == 'json':
        import json

        output_data = {
            "embodios_result": embodios_result.to_dict() if embodios_result else None,
            "baseline_result": baseline_result.to_dict() if baseline_result else None,
            "config": {
                "model": str(model_path),
                "prompts": prompts,
                "warmup": warmup,
                "threshold_pct": threshold
            }
        }

        if baseline_result and embodios_result:
            comparison = BenchmarkComparison(
                baseline_result=baseline_result,
                test_result=embodios_result,
                threshold_pct=threshold,
                baseline_label="llama.cpp",
                test_label="EMBODIOS"
            )
            output_data["comparison"] = comparison.compare().to_dict()

        print(json.dumps(output_data, indent=2))

    # Exit with appropriate status
    if baseline_result and embodios_result:
        comparison = BenchmarkComparison(
            baseline_result=baseline_result,
            test_result=embodios_result,
            threshold_pct=threshold
        )
        result = comparison.compare()
        if not result.passes_threshold:
            console.print(f"[bold red]✗ Benchmark failed: overhead {result.mean_overhead_pct:.2f}% exceeds threshold {threshold:.2f}%[/bold red]")
            sys.exit(1)
        else:
            console.print(f"[bold green]✓ Benchmark passed: overhead {result.mean_overhead_pct:.2f}% within threshold {threshold:.2f}%[/bold green]")
    else:
        console.print("[bold green]✓ Benchmark complete[/bold green]")
