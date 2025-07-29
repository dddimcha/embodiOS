"""Tests for EMBODIOS CLI commands."""
import pytest
from click.testing import CliRunner
from embodi.cli.main import cli


class TestCLI:
    """Test CLI commands."""
    
    def setup_method(self):
        """Set up test environment."""
        self.runner = CliRunner()
    
    def test_cli_version(self):
        """Test version command."""
        result = self.runner.invoke(cli, ['--version'])
        assert result.exit_code == 0
        assert 'embodi' in result.output
    
    def test_cli_help(self):
        """Test help command."""
        result = self.runner.invoke(cli, ['--help'])
        assert result.exit_code == 0
        assert 'Usage:' in result.output
    
    def test_init_command(self):
        """Test init command."""
        with self.runner.isolated_filesystem():
            result = self.runner.invoke(cli, ['init'])
            assert result.exit_code == 0
            assert 'Modelfile' in result.output
    
    def test_build_command_no_modelfile(self):
        """Test build command without Modelfile."""
        with self.runner.isolated_filesystem():
            result = self.runner.invoke(cli, ['build', '-t', 'test:latest'])
            assert result.exit_code != 0
    
    def test_images_command(self):
        """Test images command."""
        result = self.runner.invoke(cli, ['images'])
        assert result.exit_code == 0
    
    def test_ps_command(self):
        """Test ps command."""
        result = self.runner.invoke(cli, ['ps'])
        assert result.exit_code == 0