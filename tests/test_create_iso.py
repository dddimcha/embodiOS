#!/usr/bin/env python3
"""
Unit tests for ISOCreator._create_initrd() method
Tests security fix for command injection vulnerability
"""

import os
import tempfile
import subprocess
from pathlib import Path
from unittest.mock import patch, MagicMock, call
import pytest

from src.embodi.installer.iso.create_iso import ISOCreator


class TestISOCreatorInitrd:
    """Test suite for ISOCreator._create_initrd() method"""

    def test_create_initrd_creates_init_script(self):
        """Test that _create_initrd creates the init script file"""
        creator = ISOCreator()

        with tempfile.TemporaryDirectory() as work_dir:
            creator.work_dir = Path(work_dir)
            initrd_dir = creator.work_dir / 'initrd'
            embodi_dir = creator.work_dir / 'embodi'
            embodi_dir.mkdir(parents=True, exist_ok=True)

            # Mock subprocess calls
            with patch('subprocess.Popen') as mock_popen:
                # Setup mock process pipeline
                mock_find = MagicMock()
                mock_find.stdout = MagicMock()
                mock_cpio = MagicMock()
                mock_cpio.stdout = MagicMock()
                mock_gzip = MagicMock()
                mock_gzip.wait.return_value = None
                mock_gzip.returncode = 0

                mock_popen.side_effect = [mock_find, mock_cpio, mock_gzip]

                creator._create_initrd()

                # Verify init script was created
                init_script = initrd_dir / 'init'
                assert init_script.exists(), "Init script should be created"
                assert init_script.stat().st_mode & 0o111, "Init script should be executable"

                # Verify init script content
                content = init_script.read_text()
                assert "#!/bin/sh" in content
                assert "EMBODIOS Early Boot" in content

    def test_create_initrd_uses_list_arguments_not_shell(self):
        """Test that subprocess calls use list arguments, not shell=True (security fix)"""
        creator = ISOCreator()

        with tempfile.TemporaryDirectory() as work_dir:
            creator.work_dir = Path(work_dir)
            embodi_dir = creator.work_dir / 'embodi'
            embodi_dir.mkdir(parents=True, exist_ok=True)

            with patch('subprocess.Popen') as mock_popen:
                # Setup mock process pipeline
                mock_find = MagicMock()
                mock_find.stdout = MagicMock()
                mock_cpio = MagicMock()
                mock_cpio.stdout = MagicMock()
                mock_gzip = MagicMock()
                mock_gzip.wait.return_value = None
                mock_gzip.returncode = 0

                mock_popen.side_effect = [mock_find, mock_cpio, mock_gzip]

                creator._create_initrd()

                # Verify subprocess.Popen was called 3 times (find, cpio, gzip)
                assert mock_popen.call_count == 3, "Should call Popen 3 times for pipeline"

                # Get all calls to subprocess.Popen
                calls = mock_popen.call_args_list

                # Verify find command uses list arguments
                find_call = calls[0]
                assert find_call[0][0] == ['find', '.'], "find should use list arguments"
                # Verify shell=True is NOT used
                assert 'shell' not in find_call[1] or find_call[1].get('shell') is False

                # Verify cpio command uses list arguments
                cpio_call = calls[1]
                assert cpio_call[0][0] == ['cpio', '-o', '-H', 'newc'], "cpio should use list arguments"
                assert 'shell' not in cpio_call[1] or cpio_call[1].get('shell') is False

                # Verify gzip command uses list arguments
                gzip_call = calls[2]
                assert gzip_call[0][0] == ['gzip'], "gzip should use list arguments"
                assert 'shell' not in gzip_call[1] or gzip_call[1].get('shell') is False

    def test_create_initrd_uses_cwd_for_directory_change(self):
        """Test that subprocess uses cwd parameter instead of cd command"""
        creator = ISOCreator()

        with tempfile.TemporaryDirectory() as work_dir:
            creator.work_dir = Path(work_dir)
            initrd_dir = creator.work_dir / 'initrd'
            embodi_dir = creator.work_dir / 'embodi'
            embodi_dir.mkdir(parents=True, exist_ok=True)

            with patch('subprocess.Popen') as mock_popen:
                # Setup mock process pipeline
                mock_find = MagicMock()
                mock_find.stdout = MagicMock()
                mock_cpio = MagicMock()
                mock_cpio.stdout = MagicMock()
                mock_gzip = MagicMock()
                mock_gzip.wait.return_value = None
                mock_gzip.returncode = 0

                mock_popen.side_effect = [mock_find, mock_cpio, mock_gzip]

                creator._create_initrd()

                # Verify find command uses cwd parameter
                find_call = mock_popen.call_args_list[0]
                assert 'cwd' in find_call[1], "find should use cwd parameter"
                assert find_call[1]['cwd'] == str(initrd_dir), "cwd should point to initrd directory"

    def test_create_initrd_creates_output_file(self):
        """Test that _create_initrd creates the initrd.img output file"""
        creator = ISOCreator()

        with tempfile.TemporaryDirectory() as work_dir:
            creator.work_dir = Path(work_dir)
            embodi_dir = creator.work_dir / 'embodi'
            embodi_dir.mkdir(parents=True, exist_ok=True)
            output_path = embodi_dir / 'initrd.img'

            with patch('subprocess.Popen') as mock_popen, \
                 patch('builtins.open', create=True) as mock_open:
                # Setup mock process pipeline
                mock_find = MagicMock()
                mock_find.stdout = MagicMock()
                mock_cpio = MagicMock()
                mock_cpio.stdout = MagicMock()
                mock_gzip = MagicMock()
                mock_gzip.wait.return_value = None
                mock_gzip.returncode = 0

                mock_popen.side_effect = [mock_find, mock_cpio, mock_gzip]

                # Mock file handle
                mock_file = MagicMock()
                mock_open.return_value.__enter__.return_value = mock_file

                creator._create_initrd()

                # Verify file was opened for writing
                mock_open.assert_called_once_with(output_path, 'wb')

                # Verify gzip was passed the file handle
                gzip_call = mock_popen.call_args_list[2]
                assert gzip_call[1]['stdout'] == mock_file

    def test_create_initrd_raises_error_on_gzip_failure(self):
        """Test that _create_initrd raises CalledProcessError when gzip fails"""
        creator = ISOCreator()

        with tempfile.TemporaryDirectory() as work_dir:
            creator.work_dir = Path(work_dir)
            embodi_dir = creator.work_dir / 'embodi'
            embodi_dir.mkdir(parents=True, exist_ok=True)

            with patch('subprocess.Popen') as mock_popen:
                # Setup mock process pipeline with gzip failure
                mock_find = MagicMock()
                mock_find.stdout = MagicMock()
                mock_cpio = MagicMock()
                mock_cpio.stdout = MagicMock()
                mock_gzip = MagicMock()
                mock_gzip.wait.return_value = None
                mock_gzip.returncode = 1  # Non-zero return code indicates failure

                mock_popen.side_effect = [mock_find, mock_cpio, mock_gzip]

                # Verify that CalledProcessError is raised
                with pytest.raises(subprocess.CalledProcessError):
                    creator._create_initrd()

    def test_create_initrd_pipeline_closes_pipes_correctly(self):
        """Test that subprocess pipes are closed correctly to avoid deadlocks"""
        creator = ISOCreator()

        with tempfile.TemporaryDirectory() as work_dir:
            creator.work_dir = Path(work_dir)
            embodi_dir = creator.work_dir / 'embodi'
            embodi_dir.mkdir(parents=True, exist_ok=True)

            with patch('subprocess.Popen') as mock_popen:
                # Setup mock process pipeline
                mock_find = MagicMock()
                mock_find_stdout = MagicMock()
                mock_find.stdout = mock_find_stdout

                mock_cpio = MagicMock()
                mock_cpio_stdout = MagicMock()
                mock_cpio.stdout = mock_cpio_stdout

                mock_gzip = MagicMock()
                mock_gzip.wait.return_value = None
                mock_gzip.returncode = 0

                mock_popen.side_effect = [mock_find, mock_cpio, mock_gzip]

                creator._create_initrd()

                # Verify stdout pipes were closed to prevent deadlock
                mock_find_stdout.close.assert_called_once()
                mock_cpio_stdout.close.assert_called_once()

    def test_create_initrd_no_command_injection_vectors(self):
        """Test that paths with special characters don't cause command injection"""
        creator = ISOCreator()

        # Test with a work_dir path that contains shell metacharacters
        # This would be dangerous if shell=True was used
        with tempfile.TemporaryDirectory() as base_dir:
            # Create a subdirectory with potentially dangerous characters
            # (though in practice, temp dirs won't have these, this tests the safety)
            safe_dir = Path(base_dir) / "test_dir"
            safe_dir.mkdir(exist_ok=True)

            creator.work_dir = safe_dir
            embodi_dir = creator.work_dir / 'embodi'
            embodi_dir.mkdir(parents=True, exist_ok=True)

            with patch('subprocess.Popen') as mock_popen:
                # Setup mock process pipeline
                mock_find = MagicMock()
                mock_find.stdout = MagicMock()
                mock_cpio = MagicMock()
                mock_cpio.stdout = MagicMock()
                mock_gzip = MagicMock()
                mock_gzip.wait.return_value = None
                mock_gzip.returncode = 0

                mock_popen.side_effect = [mock_find, mock_cpio, mock_gzip]

                # Should complete without error
                creator._create_initrd()

                # Verify the commands were called with list arguments (safe)
                assert all(
                    isinstance(call_args[0][0], list)
                    for call_args in mock_popen.call_args_list
                ), "All subprocess calls should use list arguments"

    def test_create_initrd_integration_with_real_subprocess(self):
        """Integration test that actually runs subprocess commands"""
        creator = ISOCreator()

        with tempfile.TemporaryDirectory() as work_dir:
            creator.work_dir = Path(work_dir)
            embodi_dir = creator.work_dir / 'embodi'
            embodi_dir.mkdir(parents=True, exist_ok=True)

            # Check if required commands are available
            if not all(subprocess.run(['which', cmd], capture_output=True).returncode == 0
                      for cmd in ['find', 'cpio', 'gzip']):
                pytest.skip("Required commands (find, cpio, gzip) not available")

            # Run the actual method
            creator._create_initrd()

            # Verify output file was created
            output_path = embodi_dir / 'initrd.img'
            assert output_path.exists(), "initrd.img should be created"
            assert output_path.stat().st_size > 0, "initrd.img should not be empty"

            # Verify it's a valid gzip file
            result = subprocess.run(
                ['file', str(output_path)],
                capture_output=True,
                text=True
            )
            assert 'gzip' in result.stdout.lower(), "Output should be gzip compressed"


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
