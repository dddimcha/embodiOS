#!/usr/bin/env python3
"""
EMBODIOS Model Inference Comparison Test
Compares TinyLlama inference between embodi and llama.cpp
"""

import time
import json
from pathlib import Path
from datetime import datetime

try:
    from llama_cpp import Llama
    LLAMA_CPP_AVAILABLE = True
except ImportError:
    LLAMA_CPP_AVAILABLE = False
    print("WARNING: llama-cpp-python not available")


class InferenceComparison:
    """Compare inference between different implementations"""

    def __init__(self, model_path):
        self.model_path = model_path
        self.results = []

    def test_llama_cpp(self, prompt, max_tokens=100):
        """Test inference with llama.cpp"""
        if not LLAMA_CPP_AVAILABLE:
            return None

        print(f"\n{'='*70}")
        print("Testing with llama.cpp (llama-cpp-python)")
        print(f"{'='*70}")

        # Load model
        load_start = time.time()
        llm = Llama(
            model_path=self.model_path,
            n_ctx=512,
            n_threads=4,
            verbose=False
        )
        load_time = time.time() - load_start
        print(f"✓ Model loaded in {load_time:.2f}s")

        # Run inference
        print(f"\nPrompt: '{prompt}'")
        infer_start = time.time()
        response = llm(prompt, max_tokens=max_tokens, temperature=0.7)
        infer_time = time.time() - infer_start

        output_text = response['choices'][0]['text'].strip()
        tokens_generated = len(output_text.split())  # Approximate

        result = {
            'implementation': 'llama.cpp',
            'prompt': prompt,
            'output': output_text,
            'load_time_seconds': load_time,
            'inference_time_seconds': infer_time,
            'tokens_generated': tokens_generated,
            'tokens_per_second': tokens_generated / infer_time if infer_time > 0 else 0,
            'timestamp': datetime.now().isoformat()
        }

        print(f"\nOutput: {output_text}")
        print(f"Inference time: {infer_time:.2f}s")
        print(f"Speed: {result['tokens_per_second']:.2f} tokens/sec")

        return result

    def test_embodios_simulation(self, prompt, max_tokens=100):
        """Simulate EMBODIOS inference (would run on actual hardware)"""
        print(f"\n{'='*70}")
        print("EMBODIOS Inference (Simulated - would run on kernel)")
        print(f"{'='*70}")

        # This would actually run the EMBODIOS kernel inference
        # For now, we simulate what would happen
        print(f"\nPrompt: '{prompt}'")
        print("\n[Note: In production, this runs on bare metal with:")
        print("  - Integer-only Q16.16 fixed-point arithmetic")
        print("  - No floating-point operations")
        print("  - Direct hardware execution (no OS overhead)")
        print("  - ARM NEON SIMD optimizations")
        print("  - TinyLlama 1.1B Q4_K_M quantized model]")

        # Simulate response (in real deployment, this comes from kernel)
        simulated_output = "[EMBODIOS kernel would generate response here using quantized inference]"

        result = {
            'implementation': 'EMBODIOS',
            'prompt': prompt,
            'output': simulated_output,
            'note': 'Runs on bare metal kernel with integer-only inference',
            'architecture': 'Q16.16 fixed-point, ARM NEON SIMD',
            'timestamp': datetime.now().isoformat()
        }

        print(f"\nNote: To test actual EMBODIOS inference, boot the kernel image")
        print(f"      on real hardware (Raspberry Pi, QEMU, etc.)")

        return result

    def run_comparison(self, prompts):
        """Run comparison tests with multiple prompts"""
        print("\n" + "="*70)
        print("EMBODIOS vs llama.cpp Inference Comparison")
        print("="*70)
        print(f"Model: {self.model_path}")
        print(f"Test started: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

        for i, prompt in enumerate(prompts, 1):
            print(f"\n\n{'#'*70}")
            print(f"Test {i}/{len(prompts)}")
            print(f"{'#'*70}")

            # Test with llama.cpp
            llama_result = self.test_llama_cpp(prompt)
            if llama_result:
                self.results.append(llama_result)

            # Test with EMBODIOS (simulated)
            embodios_result = self.test_embodios_simulation(prompt)
            self.results.append(embodios_result)

        return self.results

    def save_results(self, output_file='inference_comparison_results.json'):
        """Save comparison results to file"""
        with open(output_file, 'w') as f:
            json.dump(self.results, f, indent=2)
        print(f"\n✓ Results saved to {output_file}")

    def print_summary(self):
        """Print summary of results"""
        print("\n" + "="*70)
        print("COMPARISON SUMMARY")
        print("="*70)

        llama_results = [r for r in self.results if r['implementation'] == 'llama.cpp']

        if llama_results:
            avg_load = sum(r['load_time_seconds'] for r in llama_results) / len(llama_results)
            avg_infer = sum(r['inference_time_seconds'] for r in llama_results) / len(llama_results)
            avg_speed = sum(r['tokens_per_second'] for r in llama_results) / len(llama_results)

            print(f"\nllama.cpp Performance:")
            print(f"  Average load time:      {avg_load:.2f}s")
            print(f"  Average inference time: {avg_infer:.2f}s")
            print(f"  Average speed:          {avg_speed:.2f} tokens/sec")

        print(f"\nEMBODIOS Performance:")
        print(f"  Deployment:   Bare metal kernel (no OS)")
        print(f"  Arithmetic:   Integer-only Q16.16 fixed-point")
        print(f"  Optimization: ARM NEON SIMD vectorization")
        print(f"  Model:        TinyLlama 1.1B Q4_K_M")
        print(f"  Memory:       256MB heap allocator")

        print("\n" + "="*70)


def main():
    """Main comparison test"""

    # Check for TinyLlama model
    model_path = "models/tinyllama/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
    if not Path(model_path).exists():
        print(f"ERROR: Model not found at {model_path}")
        print("Please download the TinyLlama model first:")
        print("  embodi pull TinyLlama/TinyLlama-1.1B-Chat-v1.0")
        return 1

    # Test prompts - varied to show different capabilities
    prompts = [
        "Hello, what is your name?",
        "What is 15 plus 27?",
        "Tell me about artificial intelligence",
        "How does a computer work?",
    ]

    # Run comparison
    comparison = InferenceComparison(model_path)
    results = comparison.run_comparison(prompts)

    # Save and display results
    comparison.save_results()
    comparison.print_summary()

    print("\n✓ Comparison test completed successfully")
    return 0


if __name__ == "__main__":
    exit(main())
