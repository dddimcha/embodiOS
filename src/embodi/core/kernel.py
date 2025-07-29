"""
EMBODIOS Kernel
"""

class Kernel:
    """EMBODIOS Kernel implementation"""
    
    def __init__(self):
        self.running = False
        
    def start(self):
        """Start the kernel"""
        self.running = True
        print("EMBODIOS Kernel started")
        
    def stop(self):
        """Stop the kernel"""
        self.running = False
        print("EMBODIOS Kernel stopped")
        
    def is_running(self):
        """Check if kernel is running"""
        return self.running