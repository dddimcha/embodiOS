"""
EMBODIOS Image Management
"""

class Image:
    """EMBODIOS Image representation"""
    
    def __init__(self, image_id, repository, tag="latest"):
        self.id = image_id
        self.repository = repository
        self.tag = tag
        self.created = None
        self.size = None
        
    def get_info(self):
        """Get image information"""
        return {
            'id': self.id,
            'repository': self.repository,
            'tag': self.tag,
            'created': self.created,
            'size': self.size
        }
        
    def get_full_name(self):
        """Get full image name"""
        return f"{self.repository}:{self.tag}"