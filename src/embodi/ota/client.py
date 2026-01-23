"""OTA Client - Query model status from remote EMBODIOS instances"""

import requests
from typing import Optional, List, Dict
import os


class OTAClient:
    def __init__(self, debug: bool = False):
        self.debug = debug

    def list_remote_models(
        self,
        target: Optional[str] = None,
        show_all: bool = False
    ) -> List[Dict]:
        """
        List models on remote device(s)

        Args:
            target: Target device (IP:port or hostname)
            show_all: Show all models including inactive

        Returns:
            List of model information dictionaries
        """
        if not target:
            if self.debug:
                print("Error: target device required")
            return []

        # Prepare status URL
        if not target.startswith('http'):
            target = f"http://{target}"
        status_url = f"{target}/v1/models/status"

        try:
            response = requests.get(status_url, timeout=10)

            if response.status_code == 200:
                data = response.json()

                # Format response for display
                models_list = []
                for model in data.get('loaded_models', []):
                    # Skip inactive models unless show_all is True
                    if not show_all and not model.get('loaded', False):
                        continue

                    models_list.append({
                        'device': target,
                        'name': model.get('model_name', 'unknown'),
                        'version': model.get('version', 'unknown'),
                        'status': 'active' if model.get('loaded') else 'inactive',
                        'size': self._format_size(model.get('size_bytes', 0)),
                        'updated': model.get('last_update', 'unknown')
                    })

                return models_list
            else:
                if self.debug:
                    print(f"Status request failed: {response.status_code}")
                return []

        except Exception as e:
            if self.debug:
                print(f"Error querying device: {e}")
            return []

    def _format_size(self, size_bytes: int) -> str:
        """Format size in bytes to human-readable string"""
        if size_bytes < 1024:
            return f"{size_bytes}B"
        elif size_bytes < 1024 * 1024:
            return f"{size_bytes / 1024:.1f}KB"
        elif size_bytes < 1024 * 1024 * 1024:
            return f"{size_bytes / (1024 * 1024):.1f}MB"
        else:
            return f"{size_bytes / (1024 * 1024 * 1024):.2f}GB"
