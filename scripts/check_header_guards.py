#!/usr/bin/env python3
"""Check C header files for proper include guards."""

import os
import sys
import re


def check_header_guard(filepath):
    """Check if a header file has proper include guards."""
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Get expected guard name from file path
    # e.g., kernel/include/embodios/types.h -> EMBODIOS_TYPES_H
    relative_path = os.path.relpath(filepath)
    guard_name = relative_path.upper()
    guard_name = re.sub(r'[./\\]', '_', guard_name)
    guard_name = re.sub(r'^.*INCLUDE_', '', guard_name)
    guard_name = re.sub(r'\.H$', '_H', guard_name)
    
    # Check for ifndef/define pattern
    ifndef_pattern = rf'#ifndef\s+{guard_name}'
    define_pattern = rf'#define\s+{guard_name}'
    endif_pattern = rf'#endif\s*/\*\s*{guard_name}\s*\*/|#endif\s*$'
    
    has_ifndef = re.search(ifndef_pattern, content)
    has_define = re.search(define_pattern, content)
    has_endif = re.search(endif_pattern, content)
    
    errors = []
    if not has_ifndef:
        errors.append(f"Missing #ifndef {guard_name}")
    if not has_define:
        errors.append(f"Missing #define {guard_name}")
    if not has_endif:
        errors.append(f"Missing or incorrect #endif for {guard_name}")
    
    return errors


def main():
    """Check all header files passed as arguments."""
    if len(sys.argv) < 2:
        print("No files to check")
        return 0
    
    failed = False
    for filepath in sys.argv[1:]:
        if filepath.endswith('.h'):
            errors = check_header_guard(filepath)
            if errors:
                print(f"\n{filepath}:")
                for error in errors:
                    print(f"  - {error}")
                failed = True
    
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())