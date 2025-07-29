"""
EMBODIOS Hardware Abstraction Layer
"""

class HardwareAbstractionLayer:
    """Hardware Abstraction Layer for EMBODIOS"""
    
    def __init__(self):
        self.devices = {}
        
    def initialize(self):
        """Initialize the HAL"""
        print("EMBODIOS HAL initialized")
        
    def register_device(self, name, device):
        """Register a hardware device"""
        self.devices[name] = device
        
    def get_device(self, name):
        """Get a registered device"""
        return self.devices.get(name)