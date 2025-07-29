"""Tests for EMBODIOS runtime functionality."""
import pytest
from pathlib import Path
from embodi.runtime.runtime import EmbodiRuntime
from embodi.runtime.image import Image
from embodi.runtime.container import Container


class TestRuntime:
    """Test EMBODIOS runtime."""
    
    def test_runtime_initialization(self):
        """Test runtime initialization."""
        runtime = EmbodiRuntime()
        assert runtime is not None
        assert hasattr(runtime, 'images_dir')
        assert hasattr(runtime, 'containers')
    
    def test_list_images_empty(self):
        """Test listing images when none exist."""
        runtime = EmbodiRuntime()
        images = runtime.list_images()
        assert isinstance(images, list)
    
    def test_list_containers_empty(self):
        """Test listing containers when none exist."""
        runtime = EmbodiRuntime()
        containers = runtime.list_containers()
        assert isinstance(containers, list)


class TestImage:
    """Test Image functionality."""
    
    def test_image_creation(self):
        """Test image object creation."""
        image = Image(
            image_id="abc123",
            repository="test",
            tag="latest"
        )
        assert image.id == "abc123"
        assert image.repository == "test"
        assert image.tag == "latest"
    
    def test_image_full_name(self):
        """Test image full name generation."""
        image = Image(
            image_id="abc123",
            repository="test",
            tag="latest"
        )
        assert image.get_full_name() == "test:latest"


class TestContainer:
    """Test Container functionality."""
    
    def test_container_creation(self):
        """Test container object creation."""
        container = Container(
            container_id="abc123",
            image="test:latest",
            name="test-container"
        )
        assert container.id == "abc123"
        assert container.image == "test:latest"
        assert container.name == "test-container"
        assert container.status == "created"
    
    def test_container_short_id(self):
        """Test container short ID generation."""
        container = Container(
            container_id="abc123def456",
            image="test:latest"
        )
        assert container.id[:12] == "abc123def456"[:12]