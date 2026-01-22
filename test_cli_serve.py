#!/usr/bin/env python3
"""Test the embodi serve CLI command"""
import sys
sys.path.insert(0, 'src')

from embodi.cli.main import cli
from click.testing import CliRunner

runner = CliRunner()

# Test --help
print("=" * 60)
print("Testing: embodi serve --help")
print("=" * 60)
result = runner.invoke(cli, ['serve', '--help'])
print(result.output)
print(f"Exit code: {result.exit_code}")

if result.exit_code == 0:
    print("\n✓ CLI command 'serve' is registered and accessible")

    # Check for expected help text
    if "Start API server" in result.output:
        print("✓ Help text contains 'Start API server'")
    if "--host" in result.output:
        print("✓ --host option available")
    if "--port" in result.output:
        print("✓ --port option available")
    if "--model" in result.output:
        print("✓ --model option available")
    if "--reload" in result.output:
        print("✓ --reload option available")
else:
    print("\n✗ CLI command 'serve' failed")
    sys.exit(1)
