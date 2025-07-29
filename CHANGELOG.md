# Changelog

All notable changes to EMBODIOS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2025-07-29

Initial release

### Added
- Core EMBODIOS kernel with AI-powered hardware control
- Hardware Abstraction Layer (HAL) supporting GPIO, I2C, SPI, UART
- Natural Language Processor for converting text commands to hardware operations
- AI Inference Engine for running language models on bare metal
- Docker-like CLI for building and running AI-OS images
- Support for multiple AI models (TinyLlama, Mistral, custom models)
- Modelfile format for defining AI-OS configurations
- Real-time hardware control with <2ms response times
- Memory-efficient operation (~16MB RAM for core OS)
- Comprehensive test suite with 34 tests
- Performance benchmarks showing 465+ commands/second throughput
- Example Modelfiles for different use cases
- Documentation for getting started, API reference, and hardware setup

### Features
- **Natural Language Control**: Control hardware using plain English commands
- **Bare Metal Support**: Direct hardware access without traditional OS overhead
- **Container-like Workflow**: Build, run, and deploy AI-OS images
- **Multi-Architecture**: Support for x86_64 and ARM platforms
- **Hardware Tokens**: Special tokens for efficient hardware control
- **Interrupt Handling**: Real-time response to hardware events
- **Memory Management**: Efficient memory-mapped model weights

### Supported Hardware
- Raspberry Pi 3/4/5
- x86_64 systems (with limited GPIO)
- Generic ARM boards
- I2C devices (sensors, displays)
- SPI devices
- UART communication

### Known Limitations
- Alpha release - API may change
- Voice input requires additional setup (text-based by default)
- Limited to models that fit in available RAM
- Some hardware features platform-dependent

[0.1.0]: https://github.com/dddimcha/embodiOS/releases/tag/v0.1.0