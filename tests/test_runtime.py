"""Tests for EMBODIOS runtime functionality."""
import pytest
from pathlib import Path
from embodi.runtime.runtime import Runtime
from embodi.runtime.image import Image
from embodi.runtime.container import Container


class TestRuntime:
    """Test EMBODIOS runtime."""
    
    def test_runtime_initialization(self):
        """Test runtime initialization."""
        runtime = Runtime()
        assert runtime is not None
        assert hasattr(runtime, 'images_dir')
        assert hasattr(runtime, 'containers_dir')
    
    def test_list_images_empty(self):
        """Test listing images when none exist."""
        runtime = Runtime()
        images = runtime.list_images()
        assert isinstance(images, list)
    
    def test_list_containers_empty(self):
        """Test listing containers when none exist."""
        runtime = Runtime()
        containers = runtime.list_containers()
        assert isinstance(containers, list)


class TestImage:
    """Test Image functionality."""
    
    def test_image_creation(self):
        """Test image object creation."""
        image = Image(
            name="test",
            tag="latest",
            size=1024,
            created="2024-01-01T00:00:00Z"
        )
        assert image.name == "test"
        assert image.tag == "latest"
        assert image.size == 1024
    
    def test_image_full_name(self):
        """Test image full name generation."""
        image = Image(
            name="test",
            tag="latest",
            size=1024,
            created="2024-01-01T00:00:00Z"
        )
        assert image.full_name == "test:latest"


class TestContainer:
    """Test Container functionality."""
    
    def test_container_creation(self):
        """Test container object creation."""
        container = Container(
            id="abc123",
            image="test:latest",
            status="running",
            created="2024-01-01T00:00:00Z"
        )
        assert container.id == "abc123"
        assert container.image == "test:latest"
        assert container.status == "running"
    
    def test_container_short_id(self):
        """Test container short ID generation."""
        container = Container(
            id="abc123def456",
            image="test:latest",
            status="running",
            created="2024-01-01T00:00:00Z"
        )
        assert container.short_id == "abc123def456"[:12]