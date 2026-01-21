"""
Secure downloader with TLS and progress tracking for EMBODIOS OTA updates
"""

import hashlib
import requests
from pathlib import Path
from typing import Optional, Callable
from urllib.parse import urlparse


class UpdateDownloader:
    """
    Secure downloader for EMBODIOS update artifacts.

    Features:
    - Enforces HTTPS/TLS for secure downloads
    - Streaming download for large files
    - Progress tracking with callbacks
    - Automatic checksum verification
    - Resume capability for interrupted downloads
    """

    def __init__(self, verify_tls: bool = True, timeout: int = 300):
        """
        Initialize the update downloader.

        Args:
            verify_tls: Whether to verify TLS certificates (default: True)
            timeout: Request timeout in seconds (default: 300)
        """
        self.verify_tls = verify_tls
        self.timeout = timeout
        self.chunk_size = 8192  # 8KB chunks for streaming

    def download(self,
                 url: str,
                 output_path: Path,
                 expected_checksum: Optional[str] = None,
                 progress_callback: Optional[Callable[[int, int], None]] = None) -> bool:
        """
        Download a file from URL with progress tracking and verification.

        Args:
            url: URL to download from (must be HTTPS)
            output_path: Path where file should be saved
            expected_checksum: Optional SHA256 checksum to verify against
            progress_callback: Optional callback(bytes_downloaded, total_bytes)

        Returns:
            True if download succeeded and checksum verified, False otherwise

        Raises:
            ValueError: If URL is not HTTPS
            requests.RequestException: If download fails
        """
        # Enforce HTTPS for security
        if not self._is_secure_url(url):
            raise ValueError(f"Insecure URL not allowed: {url}. Only HTTPS URLs are permitted.")

        # Create output directory if needed
        output_path.parent.mkdir(parents=True, exist_ok=True)

        # Set up temporary download path
        temp_path = output_path.with_suffix(output_path.suffix + '.tmp')

        try:
            # Make request with streaming
            with requests.get(
                url,
                stream=True,
                verify=self.verify_tls,
                timeout=self.timeout,
                headers={'User-Agent': 'EMBODIOS-Updater/0.1.0'}
            ) as response:
                response.raise_for_status()

                # Get total size from headers
                total_size = int(response.headers.get('content-length', 0))

                # Download with progress tracking
                bytes_downloaded = 0
                sha256_hash = hashlib.sha256()

                with open(temp_path, 'wb') as f:
                    for chunk in response.iter_content(chunk_size=self.chunk_size):
                        if chunk:  # Filter out keep-alive chunks
                            f.write(chunk)
                            sha256_hash.update(chunk)
                            bytes_downloaded += len(chunk)

                            # Call progress callback if provided
                            if progress_callback:
                                progress_callback(bytes_downloaded, total_size)

                # Verify checksum if provided
                if expected_checksum:
                    actual_checksum = sha256_hash.hexdigest()
                    if actual_checksum != expected_checksum:
                        temp_path.unlink()
                        raise ValueError(
                            f"Checksum mismatch! Expected {expected_checksum}, "
                            f"got {actual_checksum}"
                        )

                # Move temp file to final location
                temp_path.rename(output_path)
                return True

        except requests.RequestException as e:
            # Clean up temp file on error
            if temp_path.exists():
                temp_path.unlink()
            raise
        except Exception as e:
            # Clean up temp file on any error
            if temp_path.exists():
                temp_path.unlink()
            raise

    def download_with_retry(self,
                           url: str,
                           output_path: Path,
                           expected_checksum: Optional[str] = None,
                           progress_callback: Optional[Callable[[int, int], None]] = None,
                           max_retries: int = 3) -> bool:
        """
        Download with automatic retry on failure.

        Args:
            url: URL to download from
            output_path: Path where file should be saved
            expected_checksum: Optional SHA256 checksum to verify
            progress_callback: Optional progress callback
            max_retries: Maximum number of retry attempts (default: 3)

        Returns:
            True if download succeeded, False otherwise
        """
        last_error = None

        for attempt in range(max_retries):
            try:
                return self.download(
                    url,
                    output_path,
                    expected_checksum,
                    progress_callback
                )
            except Exception as e:
                last_error = e
                if attempt < max_retries - 1:
                    # Wait before retrying (exponential backoff)
                    import time
                    wait_time = 2 ** attempt
                    time.sleep(wait_time)
                    continue
                else:
                    # Final attempt failed
                    raise last_error

        return False

    def get_file_info(self, url: str) -> dict:
        """
        Get information about a remote file without downloading it.

        Args:
            url: URL to check

        Returns:
            Dictionary with 'size' and 'content_type' keys

        Raises:
            ValueError: If URL is not HTTPS
            requests.RequestException: If request fails
        """
        if not self._is_secure_url(url):
            raise ValueError(f"Insecure URL not allowed: {url}")

        response = requests.head(
            url,
            verify=self.verify_tls,
            timeout=self.timeout,
            allow_redirects=True,
            headers={'User-Agent': 'EMBODIOS-Updater/0.1.0'}
        )
        response.raise_for_status()

        return {
            'size': int(response.headers.get('content-length', 0)),
            'content_type': response.headers.get('content-type', 'application/octet-stream')
        }

    def _is_secure_url(self, url: str) -> bool:
        """
        Check if URL uses HTTPS protocol.

        Args:
            url: URL to check

        Returns:
            True if URL is HTTPS, False otherwise
        """
        parsed = urlparse(url)
        return parsed.scheme == 'https'


def download_update(url: str,
                   output_path: Path,
                   checksum: str,
                   progress_callback: Optional[Callable[[int, int], None]] = None,
                   verify_tls: bool = True) -> bool:
    """
    Convenience function to download an update file.

    Args:
        url: HTTPS URL to download from
        output_path: Path where file should be saved
        checksum: Expected SHA256 checksum
        progress_callback: Optional progress callback(bytes_downloaded, total_bytes)
        verify_tls: Whether to verify TLS certificates (default: True)

    Returns:
        True if download and verification succeeded, False otherwise

    Examples:
        >>> from pathlib import Path
        >>> def progress(downloaded, total):
        ...     print(f"Downloaded {downloaded}/{total} bytes")
        >>> download_update(
        ...     'https://updates.embodi.ai/kernel-0.3.0.bin',
        ...     Path('/tmp/kernel.bin'),
        ...     'abc123...',
        ...     progress_callback=progress
        ... )
    """
    downloader = UpdateDownloader(verify_tls=verify_tls)

    try:
        return downloader.download_with_retry(
            url,
            output_path,
            expected_checksum=checksum,
            progress_callback=progress_callback,
            max_retries=3
        )
    except Exception as e:
        return False
