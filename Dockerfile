# EMBODIOS One-Click ISO Builder
# Provides a reproducible build environment with all dependencies

FROM ubuntu:22.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install system dependencies
RUN apt-get update && apt-get install -y \
    # Build essentials
    build-essential \
    make \
    nasm \
    # ISO creation tools (required by make_iso.sh)
    grub-pc-bin \
    grub-common \
    xorriso \
    mtools \
    # Development tools
    gcc \
    g++ \
    binutils \
    # Cross-compiler dependencies
    wget \
    libgmp-dev \
    libmpfr-dev \
    libmpc-dev \
    texinfo \
    bison \
    flex \
    # Utilities
    coreutils \
    bash \
    && rm -rf /var/lib/apt/lists/*

# Build x86_64-elf cross-compiler from source
# This is required for bare-metal kernel compilation
ENV CROSS_PREFIX=/opt/cross
ENV PATH="${CROSS_PREFIX}/bin:${PATH}"

# Download and build binutils
WORKDIR /tmp/cross
RUN wget https://ftp.gnu.org/gnu/binutils/binutils-2.40.tar.gz && \
    tar -xzf binutils-2.40.tar.gz && \
    mkdir -p binutils-build && \
    cd binutils-build && \
    ../binutils-2.40/configure \
        --target=x86_64-elf \
        --prefix="${CROSS_PREFIX}" \
        --with-sysroot \
        --disable-nls \
        --disable-werror && \
    make -j$(nproc) && \
    make install && \
    cd /tmp/cross && \
    rm -rf binutils-2.40 binutils-2.40.tar.gz binutils-build

# Download and build GCC
RUN wget https://ftp.gnu.org/gnu/gcc/gcc-12.2.0/gcc-12.2.0.tar.gz && \
    tar -xzf gcc-12.2.0.tar.gz && \
    mkdir -p gcc-build && \
    cd gcc-build && \
    ../gcc-12.2.0/configure \
        --target=x86_64-elf \
        --prefix="${CROSS_PREFIX}" \
        --disable-nls \
        --enable-languages=c,c++ \
        --without-headers && \
    make -j$(nproc) all-gcc && \
    make -j$(nproc) all-target-libgcc && \
    make install-gcc && \
    make install-target-libgcc && \
    cd /tmp && \
    rm -rf /tmp/cross

# Set working directory
WORKDIR /workspace

# Verify installation
RUN x86_64-elf-gcc --version && \
    grub-mkrescue --version && \
    xorriso --version

# Default command: show help
CMD ["bash", "-c", "echo 'EMBODIOS Builder Ready' && echo '' && echo 'Available commands:' && echo '  make               - Build kernel' && echo '  make iso-prod      - Build production ISO' && echo '  make iso-full      - Build ISO with models' && bash"]
