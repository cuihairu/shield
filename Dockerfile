# Multi-stage production Dockerfile for Shield
# Stage 1: Build with vcpkg dependencies
# Stage 2: Minimal runtime image

# === Build Stage ===
FROM ubuntu:24.04 AS builder
ARG SHIELD_GIT_COMMIT_HASH=Unknown

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    curl \
    ca-certificates \
    zip \
    unzip \
    tar \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Install vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="${VCPKG_ROOT}:${PATH}"

WORKDIR /build

# Copy manifests first for dependency caching
COPY vcpkg.json ./
COPY vcpkg-configuration.json ./

# Install dependencies (cached layer)
RUN vcpkg install --triplet x64-linux

# Copy source
COPY . .

# Build
RUN cmake -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DSHIELD_GIT_COMMIT_HASH="${SHIELD_GIT_COMMIT_HASH}" \
    -DSHIELD_BUILD_TESTS=OFF \
    -DSHIELD_BUILD_EXAMPLES=OFF \
    && cmake --build build --config Release

# === Runtime Stage ===
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libstdc++6 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN groupadd -r shield && useradd -r -g shield -d /app -s /sbin/nologin shield

WORKDIR /app

# Copy binary and runtime files
COPY --from=builder /build/build/bin/shield /app/shield
COPY config/ /app/config/
COPY scripts/ /app/scripts/

RUN chown -R shield:shield /app
USER shield

# Default ports: TCP 8080, UDP 8081, HTTP 8082, WS 8083
EXPOSE 8080 8081 8082 8083

ENTRYPOINT ["/app/shield"]
CMD ["server", "--config", "/app/config/app.yaml"]
