# ==============================================================================
# Keva - Dockerfile
#
# Purpose:
#   Defines a multi-stage Docker build for Keva that produces a minimal,
#   portable Linux container image for deployment.
#
#   Stage 1: "builder"
#     - Ubuntu 22.04 with GCC-12, CMake 3.20+, and Ninja build system
#     - Compiles Keva in Release mode (-O3, LTO) from source
#     - This stage is discarded after the build; the compiler and headers
#       do NOT appear in the final image.
#
#   Stage 2: "runtime"
#     - Minimal Ubuntu 22.04 base with only the C++ runtime library
#     - Copies the compiled 'keva-server' binary from the builder stage
#     - Exposes port 6379 (the Keva/Redis standard port)
#     - Default CMD: start keva-server on 0.0.0.0:6379
#
#   Usage:
#     docker build -t keva:latest .
#     docker run -p 6379:6379 keva:latest
#
#   From Windows with WSL2/Docker Desktop, connect via:
#     redis-cli -p 6379 PING
# ==============================================================================

# ---- Stage 1: Builder --------------------------------------------------------
FROM ubuntu:22.04 AS builder

# Prevent tzdata and other packages from requesting interactive input
ENV DEBIAN_FRONTEND=noninteractive

# Install build tools: GCC-12 (C++20 support), CMake 3.25, Ninja, and Git
# Git is required by CMake FetchContent (Catch2 testing library)
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-12 \
    g++-12 \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Set GCC-12 as the default C++ compiler
ENV CC=gcc-12
ENV CXX=g++-12

# Copy the entire Keva source tree into the container
WORKDIR /keva
COPY . .

# Configure (Release mode: -O3, LTO) and build using Ninja
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++-12 \
    -G Ninja

RUN cmake --build build --target keva-server --parallel $(nproc)

# ---- Stage 2: Runtime --------------------------------------------------------
FROM ubuntu:22.04 AS runtime

# Install only the minimal C++ runtime shared libraries (libstdc++, libgcc)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# Create a non-root user for the database process (security best practice)
RUN groupadd -r keva && useradd -r -g keva keva

# Copy only the compiled binary from the builder stage
COPY --from=builder /keva/build/src/keva-server /usr/local/bin/keva-server

# Data directory for RDB snapshot files
RUN mkdir -p /var/lib/keva && chown keva:keva /var/lib/keva

WORKDIR /var/lib/keva
USER keva

# Keva listens on the same default port as Redis (6379)
EXPOSE 6379

# Health check: ping Keva using the redis-cli protocol PING command
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD bash -c 'echo -e "*1\r\n\$4\r\nPING\r\n" | nc -q 1 localhost 6379 | grep -q "+PONG"'

# Start the Keva server; bind to all interfaces inside the container
CMD ["keva-server", "--host", "0.0.0.0", "--port", "6379", "--loglevel", "info"]
