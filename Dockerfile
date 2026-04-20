FROM redhat/ubi9-minimal:latest

LABEL maintainer="Kasper Kanarek"
LABEL description="HFT Infrastructure Lab — low-latency trading systems (C++)"

# Install build tools (no Python needed)
# Zainstaluj narzędzia do kompilacji (Python niepotrzebny)
RUN microdnf install -y gcc-c++ make && \
    microdnf clean all

# Set working directory
WORKDIR /hft-infra-lab
COPY . .

# Build all 20 C++ binaries
RUN make build

# Default: run tests, then benchmarks, then simulator
CMD echo "=== HFT Infrastructure Lab (C++) ===" && \
    echo "" && \
    echo "[1/3] Running unit tests..." && \
    make test && \
    echo "" && \
    echo "[2/3] Running benchmarks..." && \
    make benchmark && \
    echo "" && \
    echo "[3/3] Running market simulator (all modes)..." && \
    make simulate
