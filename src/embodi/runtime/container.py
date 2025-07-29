"""
EMBODIOS Container Management
"""

class Container:
    """EMBODIOS Container representation"""
    
    def __init__(self, container_id, image, name=None):
        self.id = container_id
        self.image = image
        self.name = name or f"embodios-{container_id[:12]}"
        self.status = "created"
        
    def start(self):
        """Start the container"""
        self.status = "running"
        print(f"Starting container {self.name}")
        
    def stop(self):
        """Stop the container"""
        self.status = "stopped"
        print(f"Stopping container {self.name}")
        
    def get_info(self):
        """Get container information"""
        return {
            'id': self.id,
            'image': self.image,
            'name': self.name,
            'status': self.status
        }