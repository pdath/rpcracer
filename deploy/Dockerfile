# rpcrace Dockerfile
# Multi-stage build: compile in build stage, minimal runtime image
#
# Build:
#   docker build -t rpcrace -f deploy/Dockerfile .
#
# Run:
#   docker run -d --restart=on-failure \
#     -v /path/to/rpcrace.conf:/etc/rpcrace/rpcrace.conf:ro \
#     -p 8332:8332 -p 37152:37152 \
#     rpcrace
#
# The container expects a config file mounted at /etc/rpcrace/rpcrace.conf

# =============================================================================
# Build stage
# =============================================================================
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    make \
    pkg-config \
    libzmq3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy source tree
COPY Makefile configure ./
COPY src/ src/
COPY include/ include/

# Build the binary
RUN chmod +x configure && ./configure && make

# =============================================================================
# Runtime stage
# =============================================================================
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libzmq5 \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -r -s /usr/sbin/nologin rpcrace

# Copy binary from build stage
COPY --from=build /build/rpcrace /usr/local/bin/rpcrace

# Config mount point
RUN mkdir -p /etc/rpcrace
VOLUME ["/etc/rpcrace"]

# Switch to non-root user
USER rpcrace

# Expose RPC listener and HTTP listener (notify + stats) ports
EXPOSE 8332 37152

# Health check: poll the /stats endpoint to detect event loop hangs
# Interval 10s, timeout 5s, start period 5s, retries 3
HEALTHCHECK --interval=10s --timeout=5s --start-period=5s --retries=3 \
    CMD wget -q -O /dev/null http://localhost:37152/stats || exit 1

# Restart-compatible: container orchestrators (Docker, Kubernetes) will
# restart the container when the health check fails. The on-failure restart
# policy (set at docker run) combined with HEALTHCHECK provides automatic
# recovery from event loop hangs.

ENTRYPOINT ["/usr/local/bin/rpcrace"]
CMD ["/etc/rpcrace/rpcrace.conf"]
