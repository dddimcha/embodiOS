#!/usr/bin/env python3
import sys
sys.path.insert(0, 'src')

from embodi.cli.main import cli

print("Registered CLI commands:")
for cmd_name, cmd_obj in cli.commands.items():
    print(f"  - {cmd_name}")
