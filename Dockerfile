# NOVA Docker Image
FROM python:3.11-slim

# Install system dependencies
RUN apt-get update && apt-get install -y \
    qemu-system-x86 \
    qemu-utils \
    git \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy requirements first for better caching
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy source code
COPY src/ src/
COPY setup.py .
COPY README.md .

# Install NOVA
RUN pip install --no-cache-dir -e .

# Create NOVA home directory
RUN mkdir -p /root/.nova

# Set entrypoint
ENTRYPOINT ["nova"]
CMD ["--help"]