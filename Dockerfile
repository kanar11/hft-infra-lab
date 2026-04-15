FROM redhat/ubi9-minimal:latest

LABEL maintainer="Kasper Kanarek"
LABEL description="HFT Infrastructure Lab — low-latency trading systems"

# Install build tools and Python
RUN microdnf install -y gcc-c++ make python3 python3-pip && \
    microdnf clean all

# Set working directory
WORKDIR /hft-infra-lab
COPY . .

# Build all C++ modules
RUN make build

# Default: run tests, then benchmarks, then simulator
CMD echo "=== HFT Infrastructure Lab ===" && \
    echo "" && \
    echo "[1/3] Running tests (44/44)..." && \
    make test && \
    echo "" && \
    echo "[2/3] Running benchmarks..." && \
    make benchmark && \
    echo "" && \
    echo "[3/3] Running market data simulator (10K messages)..." && \
    make simulate
