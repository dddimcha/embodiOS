#!/usr/bin/env python3
"""
EMBODIOS CLI - Main entry point
"""

import click
import sys
from pathlib import Path
from rich.console import Console
from rich.table import Table
from rich.progress import Progress, SpinnerColumn, TextColumn

from embodi import __version__
from embodi.builder import EmbodiBuilder
from embodi.runtime import EmbodiRuntime
# Commands are implemented directly in this file

console = Console()

@click.group()
@click.version_option(version=__version__, prog_name="EMBODIOS")
@click.option('--debug', is_flag=True, help='Enable debug output')
@click.pass_context
def cli(ctx, debug):
    """EMBODIOS - Natural Operating System with Voice AI
    
    Build and run AI-powered operating systems using natural language.
    """
    ctx.ensure_object(dict)
    ctx.obj['debug'] = debug
    ctx.obj['console'] = console

@cli.command()
@click.argument('model', required=True)
@click.option('--output', '-o', help='Output directory (default: models/)')
@click.option('--format', default='gguf', help='Model format (gguf, aios)')
@click.option('--quantize', '-q', type=int, help='Quantization bits (4, 8)')
@click.pass_context
def pull(ctx, model, output, format, quantize):
    """Pull a model from HuggingFace or other sources
    
    Examples:
        embodi pull TinyLlama/TinyLlama-1.1B-Chat-v1.0
        embodi pull microsoft/phi-2 --quantize 4
        embodi pull https://huggingface.co/.../model.gguf
    """
    from embodi.models.huggingface import HuggingFaceDownloader
    
    output_dir = Path(output) if output else Path('models')
    output_dir.mkdir(parents=True, exist_ok=True)
    
    console.print(f"[bold blue]Pulling model: {model}[/bold blue]")
    
    # Direct URL download
    if model.startswith('http://') or model.startswith('https://'):
        import urllib.request
        filename = model.split('/')[-1]
        output_path = output_dir / filename
        
        with Progress(console=console) as progress:
            task = progress.add_task(f"Downloading {filename}...", total=None)
            urllib.request.urlretrieve(model, output_path)
            progress.update(task, completed=True)
        
        console.print(f"[green]✓ Downloaded to {output_path}[/green]")
        return
    
    # HuggingFace model
    downloader = HuggingFaceDownloader()
    model_name = model.split('/')[-1].lower()
    output_path = output_dir / f"{model_name}.{format}"
    
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task("Downloading from HuggingFace...", total=None)
        
        success = downloader.download_and_convert(
            model, 
            output_path,
            quantization=quantize
        )
        
        if success:
            console.print(f"[green]✓ Model saved to {output_path}[/green]")
        else:
            console.print(f"[red]✗ Failed to download model[/red]")
            sys.exit(1)

@cli.command()
@click.option('-f', '--file', required=True, help='Modelfile path')
@click.option('-t', '--tag', help='Image tag (name:version)')
@click.option('--no-cache', is_flag=True, help='Build without cache')
@click.option('--platform', default='linux/amd64', help='Target platform')
@click.pass_context
def build(ctx, file, tag, no_cache, platform):
    """Build EMBODIOS image from Modelfile"""
    console.print(f"[bold blue]Building EMBODIOS image from {file}[/bold blue]")
    
    builder = EmbodiBuilder(debug=ctx.obj['debug'])
    
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task("Building...", total=None)
        
        success = builder.build(
            modelfile=file,
            tag=tag,
            no_cache=no_cache,
            platform=platform,
            progress_callback=lambda msg: progress.update(task, description=msg)
        )
    
    if success:
        console.print("[bold green]✓ Build successful![/bold green]")
    else:
        console.print("[bold red]✗ Build failed[/bold red]")
        sys.exit(1)

@cli.command()
@click.argument('image')
@click.option('-d', '--detach', is_flag=True, help='Run in background')
@click.option('-n', '--name', help='Container name')
@click.option('-m', '--memory', default='2G', help='Memory limit')
@click.option('-c', '--cpus', default=2, type=int, help='Number of CPUs')
@click.option('--rm', is_flag=True, help='Remove container after exit')
@click.option('--hardware', multiple=True, default=['gpio'], help='Hardware to enable')
@click.option('--bare-metal', is_flag=True, help='Run in bare metal mode (simulated)')
@click.pass_context
def run(ctx, image, detach, name, memory, cpus, rm, hardware, bare_metal):
    """Run EMBODIOS image"""
    console.print(f"[bold blue]Starting EMBODIOS: {image}[/bold blue]")
    
    # Check if it's a .aios model file or an image name
    if image.endswith('.aios') or Path(image).exists():
        # Direct model file - use runtime kernel
        from embodi.core.runtime_kernel import EMBODIOSRunner
        
        runner = EMBODIOSRunner()
        
        if bare_metal:
            runner.run_bare_metal(image)
        else:
            hardware_config = {
                'enabled': list(hardware),
                'memory_limit': memory,
                'cpus': cpus
            }
            runner.run_interactive(image, hardware_config)
    else:
        # Container image - use regular runtime
        runtime = EmbodiRuntime(debug=ctx.obj['debug'])
        
        container_id = runtime.run(
            image=image,
            detach=detach,
            name=name,
            memory=memory,
            cpus=cpus,
            remove=rm
        )
        
        if container_id:
            if detach:
                console.print(f"[bold green]✓ Container started: {container_id[:12]}[/bold green]")
            else:
                console.print("[bold green]✓ Container exited[/bold green]")
        else:
            console.print("[bold red]✗ Failed to start container[/bold red]")
            sys.exit(1)

@cli.command()
@click.option('-a', '--all', is_flag=True, help='Show all images')
@click.pass_context
def images(ctx, all):
    """List EMBODIOS images"""
    runtime = EmbodiRuntime(debug=ctx.obj['debug'])
    images_list = runtime.list_images(show_all=all)
    
    if not images_list:
        console.print("No EMBODIOS images found")
        return
    
    table = Table(title="EMBODIOS Images")
    table.add_column("Repository", style="cyan")
    table.add_column("Tag", style="green")
    table.add_column("Image ID", style="yellow")
    table.add_column("Created", style="blue")
    table.add_column("Size", style="magenta")
    
    for img in images_list:
        table.add_row(
            img['repository'],
            img['tag'],
            img['id'][:12],
            img['created'],
            img['size']
        )
    
    console.print(table)

@cli.command()
@click.pass_context
def ps(ctx):
    """List running EMBODIOS containers"""
    runtime = EmbodiRuntime(debug=ctx.obj['debug'])
    containers = runtime.list_containers()
    
    if not containers:
        console.print("No EMBODIOS containers running")
        return
    
    table = Table(title="EMBODIOS Containers")
    table.add_column("Container ID", style="yellow")
    table.add_column("Image", style="cyan")
    table.add_column("Name", style="green")
    table.add_column("Status", style="blue")
    table.add_column("Ports", style="magenta")
    
    for container in containers:
        table.add_row(
            container['id'][:12],
            container['image'],
            container['name'],
            container['status'],
            container['ports']
        )
    
    console.print(table)

@cli.command()
@click.argument('container')
@click.pass_context
def stop(ctx, container):
    """Stop EMBODIOS container"""
    runtime = EmbodiRuntime(debug=ctx.obj['debug'])
    
    console.print(f"[bold blue]Stopping {container}...[/bold blue]")
    
    if runtime.stop(container):
        console.print("[bold green]✓ Container stopped[/bold green]")
    else:
        console.print("[bold red]✗ Failed to stop container[/bold red]")
        sys.exit(1)

@cli.command()
@click.argument('container')
@click.option('-f', '--follow', is_flag=True, help='Follow log output')
@click.pass_context
def logs(ctx, container, follow):
    """View container logs"""
    runtime = EmbodiRuntime(debug=ctx.obj['debug'])
    runtime.show_logs(container, follow=follow)

@cli.command()
@click.pass_context
def init(ctx):
    """Initialize EMBODIOS in current directory"""
    console.print("[bold blue]Initializing EMBODIOS project...[/bold blue]")
    
    # Create example Modelfile
    modelfile_content = """# EMBODIOS Modelfile
FROM scratch

# Select AI model
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit

# System configuration
MEMORY 2G
CPU 2

# Hardware capabilities
HARDWARE gpio:enabled
HARDWARE uart:enabled

# OS capabilities
CAPABILITY hardware_control
CAPABILITY sensor_reading

# Environment
ENV EMBODIOS_PROMPT "> "
ENV EMBODIOS_DEBUG 0
"""
    
    Path("Modelfile").write_text(modelfile_content)
    
    # Create .embodi directory
    Path(".embodi").mkdir(exist_ok=True)
    
    console.print("[bold green]✓ Created Modelfile[/bold green]")
    console.print("\nNext steps:")
    console.print("  1. Edit Modelfile to customize your AI-OS")
    console.print("  2. Run: [bold]embodi build -f Modelfile -t my-os:latest[/bold]")
    console.print("  3. Run: [bold]embodi run my-os:latest[/bold]")

@cli.group()
@click.pass_context
def bundle(ctx):
    """Manage EMBODIOS bundles for bare metal deployment"""
    pass

@bundle.command('create')
@click.option('--model', required=True, help='Model name or path')
@click.option('--output', required=True, help='Output ISO file')
@click.option('--target', default='bare-metal', type=click.Choice(['bare-metal', 'qemu', 'docker']))
@click.option('--arch', default='x86_64', help='Target architecture')
@click.option('--memory', default='2G', help='Memory allocation')
@click.option('--features', multiple=True, help='Hardware features to enable')
@click.option('--compress', is_flag=True, help='Compress output')
@click.pass_context
def bundle_create(ctx, model, output, target, arch, memory, features, compress):
    """Create bootable EMBODIOS bundle"""
    console.print(f"[bold blue]Creating {target} bundle for {model}[/bold blue]")
    
    from embodi.installer.bundle.bundler import EMBODIOSBundler
    
    bundler = EMBODIOSBundler()
    
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task("Creating bundle...", total=None)
        
        success = bundler.create_bundle(
            model=model,
            output=Path(output),
            target=target,
            arch=arch,
            memory=memory,
            features=list(features) if features else None,
            compress=compress
        )
        
    if success:
        console.print(f"[bold green]✓ Bundle created: {output}[/bold green]")
        if target == 'bare-metal':
            console.print("\nNext steps:")
            console.print(f"  1. Write to USB: [bold]embodi bundle write {output} /dev/sdX[/bold]")
            console.print("  2. Boot from USB on target hardware")
    else:
        console.print("[bold red]✗ Bundle creation failed[/bold red]")
        sys.exit(1)

@bundle.command('write')
@click.argument('bundle')
@click.argument('device')
@click.option('--verify', is_flag=True, help='Verify after writing')
@click.pass_context
def bundle_write(ctx, bundle, device, verify):
    """Write bundle to USB device"""
    from embodi.installer.bundle.bundler import EMBODIOSBundler

    bundler = EMBODIOSBundler()

    if bundler.write_bundle(bundle, device, verify):
        console.print("[bold green]✓ Bundle written successfully[/bold green]")
    else:
        console.print("[bold red]✗ Failed to write bundle[/bold red]")
        sys.exit(1)

@cli.group()
@click.pass_context
def update(ctx):
    """Manage EMBODIOS updates"""
    pass

@update.command('check')
@click.pass_context
def update_check(ctx):
    """Check for EMBODIOS updates

    Examples:
        embodi update check
    """
    console.print("[bold blue]Checking for updates...[/bold blue]")

    # Placeholder implementation - will be implemented in later subtasks
    console.print("[yellow]Update check not yet implemented[/yellow]")

@update.command('apply')
@click.option('--version', help='Specific version to apply')
@click.option('--force', is_flag=True, help='Force apply update')
@click.pass_context
def update_apply(ctx, version, force):
    """Apply EMBODIOS updates

    Examples:
        embodi update apply
        embodi update apply --version 1.2.3
        embodi update apply --force
    """
    console.print("[bold blue]Applying updates...[/bold blue]")

    if version:
        console.print(f"[blue]Target version: {version}[/blue]")
    if force:
        console.print("[yellow]Force mode enabled[/yellow]")

    # Placeholder implementation - will be implemented in later subtasks
    console.print("[yellow]Update apply not yet implemented[/yellow]")

def main():
    """Main entry point"""
    try:
        cli()
    except KeyboardInterrupt:
        console.print("\n[yellow]Interrupted by user[/yellow]")
        sys.exit(130)
    except Exception as e:
        console.print(f"[bold red]Error: {e}[/bold red]")
        sys.exit(1)

if __name__ == "__main__":
    main()