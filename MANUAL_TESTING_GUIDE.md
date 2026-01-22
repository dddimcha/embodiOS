# Manual Testing Guide - EMBODIOS REST API

This guide provides step-by-step instructions for manually testing the EMBODIOS REST API server as specified in subtask-5-1.

## Prerequisites

1. EMBODIOS package installed (either via `pip install -e .` or system-wide)
2. `curl` command available (default on macOS/Linux)
3. Python 3.8+ with required dependencies

## Step 1: Start the Server

### Without a model (basic functionality test):

```bash
python3 -c "import sys; sys.path.insert(0, 'src'); from embodi.cli.main import main; sys.argv = ['embodi', 'serve', '--port', '8000']; main()"
```

Or if you have embodi installed globally:

```bash
embodi serve --port 8000
```

### With a model (full inference test):

```bash
embodi serve --model path/to/your-model.aios --port 8000
```

**Expected output:**
```
Starting EMBODIOS API server...
Host: 0.0.0.0
Port: 8000
✓ Server starting...
API docs available at: http://0.0.0.0:8000/docs
INFO:     Started server process [xxxxx]
INFO:     Waiting for application startup.
INFO:     Application startup complete.
INFO:     Uvicorn running on http://0.0.0.0:8000
```

## Step 2: Test Health Endpoint

Open a **new terminal** and run:

```bash
curl http://localhost:8000/health
```

**Expected response:**
```json
{
  "status": "healthy",
  "model_loaded": false,
  "version": "0.1.0"
}
```

**Verification:**
- ✓ Returns HTTP 200 OK
- ✓ JSON format
- ✓ Contains "status": "healthy"
- ✓ Shows model_loaded status
- ✓ Includes version number

## Step 3: Test Non-Streaming Completions

```bash
curl -X POST http://localhost:8000/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"test","prompt":"Hello","max_tokens":10}'
```

**Expected response (without model):**
```json
{
  "detail": "Inference failed: No model loaded"
}
```

**Expected response (with model):**
```json
{
  "id": "cmpl-1234567890",
  "object": "text_completion",
  "created": 1234567890,
  "model": "test",
  "choices": [
    {
      "text": " world! How can I help you today?",
      "index": 0,
      "logprobs": null,
      "finish_reason": "length"
    }
  ],
  "usage": {
    "prompt_tokens": 5,
    "completion_tokens": 10,
    "total_tokens": 15
  }
}
```

**Verification:**
- ✓ Returns HTTP 200 OK
- ✓ JSON format
- ✓ Contains required OpenAI fields: id, object, created, model, choices
- ✓ choices array has text, index, finish_reason
- ✓ usage object has prompt_tokens, completion_tokens, total_tokens
- ✓ object field is "text_completion"

## Step 4: Test Streaming Completions

```bash
curl -X POST http://localhost:8000/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"test","prompt":"Hello","max_tokens":10,"stream":true}'
```

**Expected response (without model):**
```
data: {"error": {"message": "Inference failed: No model loaded", "type": "inference_error"}}
```

**Expected response (with model):**
```
data: {"id":"cmpl-1234567890","object":"text_completion","created":1234567890,"model":"test","choices":[{"text":"Hello","index":0,"finish_reason":null}]}

data: {"id":"cmpl-1234567890","object":"text_completion","created":1234567890,"model":"test","choices":[{"text":" world","index":0,"finish_reason":null}]}

data: {"id":"cmpl-1234567890","object":"text_completion","created":1234567890,"model":"test","choices":[{"text":"!","index":0,"finish_reason":"length"}]}

data: [DONE]
```

**Verification:**
- ✓ Returns HTTP 200 OK
- ✓ Content-Type: text/event-stream
- ✓ Server-Sent Events format with "data:" prefix
- ✓ Each line is valid JSON (except [DONE])
- ✓ Streaming chunks contain partial completions
- ✓ Final message is "data: [DONE]"

## Step 5: Test Additional Endpoints

### Root endpoint:
```bash
curl http://localhost:8000/
```

**Expected:**
```json
{
  "name": "EMBODIOS Inference API",
  "version": "0.1.0",
  "endpoints": {
    "health": "/health",
    "completions": "/v1/completions",
    "docs": "/docs"
  }
}
```

### API Documentation (in browser):
```bash
open http://localhost:8000/docs
```

This should open the Swagger UI with interactive API documentation.

## Step 6: Test with Different Parameters

### With temperature:
```bash
curl -X POST http://localhost:8000/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"test","prompt":"Tell me a story","max_tokens":50,"temperature":0.8}'
```

### With multiple prompts:
```bash
curl -X POST http://localhost:8000/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"test","prompt":["Hello","Goodbye"],"max_tokens":10}'
```

### With stop sequences:
```bash
curl -X POST http://localhost:8000/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"test","prompt":"Count: 1, 2, 3,","max_tokens":20,"stop":[","]}'
```

## OpenAI Compatibility Test

If you have the OpenAI Python library installed, you can test compatibility:

```python
import openai

# Point to local server
openai.api_base = "http://localhost:8000/v1"
openai.api_key = "dummy"  # Not used but required by client

# Test completion
response = openai.Completion.create(
    model="test",
    prompt="Hello, world!",
    max_tokens=20
)

print(response)
```

## Automated Test Scripts

We've provided automated test scripts for convenience:

### Direct server test (bypasses CLI):
```bash
python3 test_server_direct.py
```

### CLI test:
```bash
python3 test_cli_serve.py
```

## Troubleshooting

### Server won't start:
- Check if port 8000 is already in use: `lsof -i :8000`
- Try a different port: `embodi serve --port 8080`
- Check dependencies: `pip install -r requirements.txt`

### Model not loading:
- Verify model path exists: `ls -l path/to/model.aios`
- Check server logs for error messages
- Try starting with `--debug` flag (in main repo)

### Connection refused:
- Ensure server is running: `curl http://localhost:8000/health`
- Check firewall settings
- Verify correct hostname (use 127.0.0.1 instead of localhost)

### JSON parsing errors:
- Ensure Content-Type header is set: `-H 'Content-Type: application/json'`
- Check JSON is valid: use a JSON validator
- Properly escape quotes in shell commands

## Success Criteria

The subtask is complete when ALL of the following are verified:

- [x] Server starts successfully with `embodi serve` command
- [x] Health endpoint returns valid JSON with status
- [x] POST /v1/completions endpoint accepts requests
- [x] Non-streaming responses match OpenAI format
- [x] Streaming responses use SSE format with `data:` prefix
- [x] Error handling works (e.g., no model loaded)
- [x] All required response fields present (id, object, created, model, choices)
- [x] CORS headers configured (can test from browser)
- [x] API documentation accessible at /docs
- [x] CLI command has proper help text and options

## Results

See `API_TESTING_RESULTS.md` for detailed test results.

**Status: ✓ ALL TESTS PASSED**

All endpoints are working correctly and returning OpenAI-compatible responses.
