#!/usr/bin/env python3
"""
NOVA CLI - Main entry point
"""

import click
import sys
from pathlib import Path
from rich.console import Console
from rich.table import Table
from rich.progress import Progress, SpinnerColumn, TextColumn

from nova import __version__
from nova.builder import NovaBuilder
from nova.runtime import NovaRuntime
from nova.cli.commands import build, run, images, pull, push

console = Console()

@click.group()
@click.version_option(version=__version__, prog_name="NOVA")
@click.option('--debug', is_flag=True, help='Enable debug output')
@click.pass_context
def cli(ctx, debug):
    """NOVA - Natural Operating System with Voice AI
    
    Build and run AI-powered operating systems using natural language.
    """
    ctx.ensure_object(dict)
    ctx.obj['debug'] = debug
    ctx.obj['console'] = console

@cli.command()
@click.option('-f', '--file', required=True, help='Modelfile path')
@click.option('-t', '--tag', help='Image tag (name:version)')
@click.option('--no-cache', is_flag=True, help='Build without cache')
@click.option('--platform', default='linux/amd64', help='Target platform')
@click.pass_context
def build(ctx, file, tag, no_cache, platform):
    """Build NOVA image from Modelfile"""
    console.print(f"[bold blue]Building NOVA image from {file}[/bold blue]")
    
    builder = NovaBuilder(debug=ctx.obj['debug'])
    
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
@click.pass_context
def run(ctx, image, detach, name, memory, cpus, rm):
    """Run NOVA image"""
    console.print(f"[bold blue]Starting NOVA: {image}[/bold blue]")
    
    runtime = NovaRuntime(debug=ctx.obj['debug'])
    
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
    """List NOVA images"""
    runtime = NovaRuntime(debug=ctx.obj['debug'])
    images_list = runtime.list_images(show_all=all)
    
    if not images_list:
        console.print("No NOVA images found")
        return
    
    table = Table(title="NOVA Images")
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
@click.argument('model')
@click.option('--source', default='huggingface', help='Model source')
@click.option('--quantize', type=click.Choice(['4', '8']), help='Quantization bits')
@click.option('--force', is_flag=True, help='Force re-download')
@click.pass_context
def pull(ctx, model, source, quantize, force):
    """Pull AI model"""
    console.print(f"[bold blue]Pulling {model} from {source}[/bold blue]")
    
    # Import model puller
    from nova.models.huggingface import pull_model
    
    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task("Downloading...", total=None)
        
        # Pull model
        quantization = int(quantize) if quantize else None
        model_path = pull_model(f"{source}:{model}", quantization, force)
        
        progress.update(task, description="Converting to NOVA format...")
        
    if model_path:
        console.print(f"[bold green]✓ Model pulled successfully: {model_path.name}[/bold green]")
    else:
        console.print("[bold red]✗ Failed to pull model[/bold red]")
        sys.exit(1)

@cli.command()
@click.pass_context
def ps(ctx):
    """List running NOVA containers"""
    runtime = NovaRuntime(debug=ctx.obj['debug'])
    containers = runtime.list_containers()
    
    if not containers:
        console.print("No NOVA containers running")
        return
    
    table = Table(title="NOVA Containers")
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
    """Stop NOVA container"""
    runtime = NovaRuntime(debug=ctx.obj['debug'])
    
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
    runtime = NovaRuntime(debug=ctx.obj['debug'])
    runtime.show_logs(container, follow=follow)

@cli.command()
@click.pass_context
def init(ctx):
    """Initialize NOVA in current directory"""
    console.print("[bold blue]Initializing NOVA project...[/bold blue]")
    
    # Create example Modelfile
    modelfile_content = """# NOVA Modelfile
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
ENV NOVA_PROMPT "> "
ENV NOVA_DEBUG 0
"""
    
    Path("Modelfile").write_text(modelfile_content)
    
    # Create .nova directory
    Path(".nova").mkdir(exist_ok=True)
    
    console.print("[bold green]✓ Created Modelfile[/bold green]")
    console.print("\nNext steps:")
    console.print("  1. Edit Modelfile to customize your AI-OS")
    console.print("  2. Run: [bold]nova build -f Modelfile -t my-os:latest[/bold]")
    console.print("  3. Run: [bold]nova run my-os:latest[/bold]")

@cli.group()
@click.pass_context
def bundle(ctx):
    """Manage NOVA bundles for bare metal deployment"""
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
    """Create bootable NOVA bundle"""
    console.print(f"[bold blue]Creating {target} bundle for {model}[/bold blue]")
    
    from nova.nova_installer.bundle.bundler import NOVABundler
    
    bundler = NOVABundler()
    
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
            console.print(f"  1. Write to USB: [bold]nova bundle write {output} /dev/sdX[/bold]")
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
    from nova.nova_installer.bundle.bundler import NOVABundler
    
    bundler = NOVABundler()
    
    if bundler.write_bundle(bundle, device, verify):
        console.print("[bold green]✓ Bundle written successfully[/bold green]")
    else:
        console.print("[bold red]✗ Failed to write bundle[/bold red]")
        sys.exit(1)

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