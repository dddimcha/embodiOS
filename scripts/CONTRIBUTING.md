# Contributing to EMBODIOS

Thank you for your interest in contributing to EMBODIOS! This document provides guidelines and instructions for contributing.

## Code of Conduct

By participating in this project, you agree to abide by our Code of Conduct. Please be respectful and constructive in all interactions.

## How to Contribute

### Reporting Issues

- Check existing issues first to avoid duplicates
- Use issue templates when available
- Provide clear reproduction steps
- Include system information (OS, Python version, etc.)

### Submitting Pull Requests

1. **Fork the repository**
   ```bash
   git clone https://github.com/yourusername/embodiOS.git
   cd embodi
   git remote add upstream https://github.com/dddimcha/embodiOS.git
   ```

2. **Create a feature branch**
   ```bash
   git checkout -b feature/your-feature-name
   ```

3. **Set up development environment**
   ```bash
   python -m venv venv
   source venv/bin/activate  # On Windows: venv\Scripts\activate
   pip install -r requirements.txt
   pip install -r requirements-dev.txt
   pip install -e .
   
   # Install pre-commit hooks
   ./scripts/setup-hooks.sh
   ```

4. **Make your changes**
   - Follow the coding style (PEP 8)
   - Add tests for new features
   - Update documentation as needed

5. **Run tests and linting**
   ```bash
   make lint
   make test
   ```

6. **Commit your changes**
   ```bash
   git add .
   git commit -m "feat: add new feature"
   ```
   
   Follow [Conventional Commits](https://www.conventionalcommits.org/):
   - `feat:` New feature
   - `fix:` Bug fix
   - `docs:` Documentation changes
   - `test:` Test changes
   - `refactor:` Code refactoring
   - `chore:` Maintenance tasks

7. **Push and create PR**
   ```bash
   git push origin feature/your-feature-name
   ```
   Then create a pull request on GitHub.

## Development Guidelines

### Pre-commit Hooks

We use pre-commit hooks to ensure code quality. They run automatically before each commit:

- **Python**: Syntax check, flake8, mypy
- **C/Kernel**: Compilation check, unsafe function detection
- **Security**: Scans for secrets and private keys
- **Files**: Checks for large files and merge conflicts

To skip hooks temporarily: `git commit --no-verify`

### Code Style

- Follow PEP 8
- Use type hints for function parameters and returns
- Maximum line length: 100 characters
- Use descriptive variable names

### Testing

- Write tests for all new features
- Maintain test coverage above 80%
- Use pytest for testing
- Mock external dependencies

### Documentation

- Update docstrings for new functions/classes
- Update README if adding major features
- Add examples for new functionality
- Keep documentation concise and clear

### Modelfile Format

When adding new Modelfile directives:
1. Update the parser in `src/embodi/builder/modelfile.py`
2. Add documentation in `docs/modelfile-reference.md`
3. Create an example in `examples/`
4. Add tests in `tests/test_modelfile.py`

## Project Structure

```
embodi/
├── src/embodi/          # Source code
│   ├── cli/          # CLI commands
│   ├── core/         # Core OS components
│   ├── builder/      # Build system
│   └── runtime/      # Container runtime
├── tests/            # Test files
├── docs/             # Documentation
├── examples/         # Example Modelfiles
└── docker/           # Docker files
```

## Review Process

1. All PRs require at least one review
2. CI must pass (tests, linting)
3. Maintainers may request changes
4. Be patient - reviews may take time

## Getting Help

- Open an issue on [GitHub](https://github.com/dddimcha/embodiOS/issues)
- Check the [documentation](https://github.com/dddimcha/embodiOS/tree/main/docs)
- Ask questions in GitHub Discussions

## Recognition

Contributors will be:
- Added to the AUTHORS file
- Mentioned in release notes
- Given credit in documentation

Thank you for contributing to EMBODIOS!