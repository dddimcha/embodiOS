"""
EMBODIOS Modelfile Parser
"""

from pathlib import Path
from typing import Dict, List, Any
import yaml  # type: ignore

class ModelfileParser:
    """Parse EMBODIOS Modelfiles"""
    
    def __init__(self, path: str):
        self.path = Path(path)
        
    def parse(self) -> Dict[str, Any]:
        """Parse Modelfile and return specification"""
        
        if self.path.suffix in ['.yaml', '.yml']:
            return self._parse_yaml()
        else:
            return self._parse_modelfile()
            
    def _parse_yaml(self) -> Dict[str, Any]:
        """Parse YAML format"""
        with open(self.path) as f:
            return yaml.safe_load(f)
            
    def _parse_modelfile(self) -> Dict[str, Any]:
        """Parse Dockerfile-style Modelfile"""
        spec = {
            'name': 'embodi-custom',
            'from': 'scratch',
            'model': {},
            'system': {},
            'hardware': {},
            'capabilities': [],
            'env': {},
            'commands': []
        }
        
        with open(self.path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                    
                parts = line.split(None, 1)
                if len(parts) < 2:
                    continue
                    
                directive, args = parts[0].upper(), parts[1]
                
                if directive == 'FROM':
                    spec['from'] = args
                elif directive == 'MODEL':
                    if ':' in args:
                        source, name = args.split(':', 1)
                        spec['model'] = {'source': source, 'name': name}
                    else:
                        spec['model'] = {'name': args}
                elif directive == 'QUANTIZE':
                    if isinstance(spec['model'], dict):
                        spec['model']['quantization'] = args
                elif directive == 'MEMORY':
                    if isinstance(spec['system'], dict):
                        spec['system']['memory'] = args
                elif directive == 'CPU':
                    if isinstance(spec['system'], dict):
                        spec['system']['cpus'] = int(args)
                elif directive == 'HARDWARE':
                    if ':' in args:
                        hw, state = args.split(':', 1)
                        if 'hardware' not in spec:
                            spec['hardware'] = {}
                        if isinstance(spec['hardware'], dict):
                            spec['hardware'][hw] = state
                elif directive == 'CAPABILITY':
                    if isinstance(spec['capabilities'], list):
                        spec['capabilities'].append(args)
                elif directive == 'ENV':
                    if '=' in args:
                        key, val = args.split('=', 1)
                        if isinstance(spec['env'], dict):
                            spec['env'][key] = val
                elif directive == 'RUN':
                    if isinstance(spec['commands'], list):
                        spec['commands'].append(args)
                    
        return spec