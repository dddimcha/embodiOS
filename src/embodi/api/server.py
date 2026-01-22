"""
FastAPI Server - EMBODIOS API server with inference engine integration
"""

from typing import Optional
from pathlib import Path
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
import logging

from ..core.inference import EMBODIOSInferenceEngine
from .routes import router, set_inference_engine
from .middleware.metrics_middleware import MetricsMiddleware

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


def create_app(model_path: Optional[str] = None, debug: bool = False) -> FastAPI:
    """
    Create and configure FastAPI application with inference engine

    Args:
        model_path: Optional path to .aios model file to load on startup
        debug: Enable debug mode with verbose logging

    Returns:
        Configured FastAPI application
    """
    # Initialize FastAPI app
    app = FastAPI(
        title="EMBODIOS Inference API",
        description="OpenAI-compatible REST API for EMBODIOS inference",
        version="0.1.0",
        docs_url="/docs",
        redoc_url="/redoc"
    )

    # Configure CORS for browser access
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],  # Allow all origins for development
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # Add metrics middleware for request tracking
    app.add_middleware(MetricsMiddleware)

    # Initialize inference engine
    engine = EMBODIOSInferenceEngine()

    # Load model if path provided
    if model_path:
        try:
            model_path_obj = Path(model_path)
            if not model_path_obj.exists():
                logger.warning(f"Model file not found: {model_path}")
            else:
                logger.info(f"Loading model from {model_path}")
                engine.load_model(str(model_path_obj))
                logger.info("Model loaded successfully")
        except Exception as e:
            logger.error(f"Failed to load model: {e}")
            if debug:
                raise

    # Set global inference engine for routes
    set_inference_engine(engine)

    # Include API routes
    app.include_router(router)

    # Health check endpoint
    @app.get("/health")
    async def health_check():
        """Health check endpoint"""
        return {
            "status": "healthy",
            "model_loaded": engine.model_loaded,
            "version": "0.1.0"
        }

    # Root endpoint
    @app.get("/")
    async def root():
        """Root endpoint with API information"""
        return {
            "name": "EMBODIOS Inference API",
            "version": "0.1.0",
            "endpoints": {
                "health": "/health",
                "completions": "/v1/completions",
                "docs": "/docs"
            }
        }

    if debug:
        logger.setLevel(logging.DEBUG)
        logger.debug("Debug mode enabled")

    return app
