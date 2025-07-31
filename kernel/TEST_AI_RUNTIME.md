# Testing AI Runtime Implementation

## Build Instructions

```bash
# Build for x86_64
make clean && make ARCH=x86_64

# Build for ARM64
make clean && make ARCH=aarch64
```

## Testing with QEMU

### x86_64
```bash
qemu-system-x86_64 -kernel embodios.elf -m 512M -nographic -serial mon:stdio
```

### ARM64
```bash
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -kernel embodios.elf -nographic -serial mon:stdio
```

## Commands to Test

Once the kernel boots, try these commands:

1. **Check memory status**
   ```
   > mem
   ```
   This shows both PMM and heap statistics.

2. **Check heap allocator**
   ```
   > heap
   ```
   Shows heap usage for AI workloads.

3. **Check model status**
   ```
   > model
   ```
   Shows if any AI model is loaded.

4. **Test inference (echo mode)**
   ```
   > infer hello world
   ```
   This will tokenize the input and echo it back (no real inference yet).

5. **Get help**
   ```
   > help
   ```
   Shows all available commands including new AI commands.

## Expected Output

1. During boot, you should see:
   - "Initializing heap allocator..."
   - "Initializing task scheduler..."
   - "Initializing AI runtime..."
   - "Heap: Initialized 256 MB at 0x10000000"

2. The `mem` command should show:
   - Physical Memory Manager stats
   - Heap statistics (256MB total)

3. The `model` command should show:
   - "No model loaded" (unless example_model is linked)

4. The `infer` command should:
   - Show tokenized input
   - Echo back the tokens as characters

## Debugging

If the kernel doesn't boot:
1. Check CI build status on GitHub
2. Ensure all files compiled successfully
3. Check for undefined symbols in linking

## Next Steps

To add a real AI model:
1. Compile your model to a binary format
2. Embed it in the kernel or load from ramdisk
3. Implement proper tokenizer
4. Add inference engine (TVM/ONNX Runtime)