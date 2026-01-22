#!/usr/bin/env python3
"""
Direct test of the API server - bypass CLI and test server directly
"""

import sys
import time
import subprocess
import signal

def start_server():
    """Start the server directly using Python"""
    # Start server in background
    cmd = [
        sys.executable,
        "-c",
        """
import sys
sys.path.insert(0, 'src')
from embodi.api.server import create_app
import uvicorn

app = create_app(model_path=None, debug=False)
uvicorn.run(app, host='0.0.0.0', port=8000, log_level='info')
"""
    ]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    return proc

def wait_for_server(timeout=15):
    """Wait for server to be ready"""
    for i in range(timeout):
        try:
            result = subprocess.run(
                ["curl", "-s", "http://localhost:8000/health"],
                capture_output=True,
                timeout=2
            )
            if result.returncode == 0:
                print(f"✓ Server ready after {i+1} seconds")
                return True
        except:
            pass
        time.sleep(1)
    return False

def run_test(name, cmd):
    """Run a single test command"""
    print(f"\n=== {name} ===")
    try:
        result = subprocess.run(
            cmd,
            shell=True,
            capture_output=True,
            text=True,
            timeout=10
        )

        if result.returncode == 0 and result.stdout:
            print(f"✓ Success")
            print(f"Response: {result.stdout[:200]}")
            return True
        else:
            print(f"✗ Failed: {result.stderr}")
            return False
    except Exception as e:
        print(f"✗ Exception: {e}")
        return False

def main():
    """Main test runner"""
    print("=" * 60)
    print("EMBODIOS API Server Direct Test")
    print("=" * 60)

    # Start server
    print("\nStarting server...")
    server_proc = start_server()

    try:
        # Wait for server
        print("Waiting for server to be ready...")
        if not wait_for_server():
            print("✗ Server failed to start")
            return 1

        # Run tests
        results = []

        results.append(run_test(
            "Test /health endpoint",
            "curl -s http://localhost:8000/health"
        ))

        results.append(run_test(
            "Test / root endpoint",
            "curl -s http://localhost:8000/"
        ))

        results.append(run_test(
            "Test /v1/completions (non-streaming)",
            "curl -s -X POST http://localhost:8000/v1/completions -H 'Content-Type: application/json' -d '{\"model\":\"test\",\"prompt\":\"Hello\",\"max_tokens\":10}'"
        ))

        results.append(run_test(
            "Test /v1/completions (streaming)",
            "curl -s -X POST http://localhost:8000/v1/completions -H 'Content-Type: application/json' -d '{\"model\":\"test\",\"prompt\":\"Hello\",\"max_tokens\":10,\"stream\":true}'"
        ))

        # Print summary
        print("\n" + "=" * 60)
        print("Test Summary")
        print("=" * 60)
        passed = sum(results)
        total = len(results)
        print(f"{passed}/{total} tests passed")

        return 0 if passed == total else 1

    finally:
        # Cleanup
        print("\nStopping server...")
        server_proc.terminate()
        try:
            server_proc.wait(timeout=5)
        except:
            server_proc.kill()

if __name__ == "__main__":
    sys.exit(main())
