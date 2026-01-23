#!/usr/bin/env python3
"""Direct test of metrics endpoint"""
import sys
sys.path.insert(0, 'src')

from embodi.api.server import create_app
from fastapi.testclient import TestClient

# Create app
app = create_app()

# Create test client
client = TestClient(app)

# Test health endpoint
print("Testing /health endpoint...")
response = client.get("/health")
print(f"Status: {response.status_code}")
print(f"Response: {response.json()}")
print()

# Test metrics endpoint
print("Testing /metrics endpoint...")
response = client.get("/metrics")
print(f"Status: {response.status_code}")
print(f"Content-Type: {response.headers.get('content-type')}")
if response.status_code == 200:
    print("First 500 chars of metrics:")
    print(response.text[:500])
else:
    print(f"Response: {response.text}")
print()

# Make a request to generate metrics
print("Making inference request...")
response = client.post("/v1/completions", json={
    "model": "test",
    "prompt": "Hello",
    "max_tokens": 10
})
print(f"Status: {response.status_code}")
print()

# Check metrics again
print("Checking /metrics after request...")
response = client.get("/metrics")
print(f"Status: {response.status_code}")
if response.status_code == 200:
    # Check for key metrics
    text = response.text
    has_requests_total = 'inference_requests_total' in text
    has_latency = 'inference_latency_seconds' in text
    has_memory = 'memory_usage_bytes' in text
    has_uptime = 'uptime_seconds' in text

    print(f"✓ Has inference_requests_total: {has_requests_total}")
    print(f"✓ Has inference_latency_seconds: {has_latency}")
    print(f"✓ Has memory_usage_bytes: {has_memory}")
    print(f"✓ Has uptime_seconds: {has_uptime}")

    if all([has_requests_total, has_latency, has_memory, has_uptime]):
        print("\n✓ ALL METRICS PRESENT!")
        sys.exit(0)
    else:
        print("\n✗ Some metrics missing")
        sys.exit(1)
else:
    print(f"✗ Metrics endpoint failed: {response.text}")
    sys.exit(1)
