# ════════════════════════════════════════════════════════════════════════
#  Containerfile — AcquisitionApp SDR Spectrum Scanner
#  Multi-stage: builder → test → runtime
#  Base: Ubuntu 24.04
#
#  Build (production):
#    podman build -t sdr-acquisition:1.0.0 .
#
#  Run unit tests only:
#    podman build --target test -t sdr-acquisition-test .
# ════════════════════════════════════════════════════════════════════════

# ── Stage 1: Builder ──────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        git \
        ca-certificates \
        libfftw3-dev \
        libtinyxml2-dev \
        libspdlog-dev \
        libfmt-dev \
        libsoapysdr-dev \
        soapysdr-module-remote \
        libqpid-proton-cpp12-dev \
        libpqxx-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone SdrTaskApi as sibling (required by CMakeLists sibling detection)
RUN git clone --depth 1 https://github.com/BMichaud7/SdrTaskApi.git /workspace/SdrTaskApi

WORKDIR /workspace/AcquisitionApp
COPY CMakeLists.txt .
COPY include/       include/
COPY src/           src/
COPY tests/         tests/
COPY config/        config/
COPY schema/        schema/

RUN cmake -B build \
        -S . \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/install \
    && cmake --build build --parallel "$(nproc)" \
    && cmake --install build


# ── Stage 2: Test runner ──────────────────────────────────────────────────
# podman build --target test .
FROM builder AS test
RUN ctest --test-dir build --output-on-failure -V


# ── Stage 3: Runtime image ────────────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
        libfftw3-single3 \
        libtinyxml2-10 \
        libspdlog1.12 \
        libfmt9 \
        libsoapysdr0.8 \
        soapysdr-module-remote \
        libqpid-proton-cpp12 \
        libpq5 \
        libpqxx-7.8t64 \
        tini \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd -r sdr && useradd -r -g sdr -s /sbin/nologin sdr
RUN mkdir -p /etc/sdr-acquisition && chown sdr:sdr /etc/sdr-acquisition

COPY --from=builder /install/bin/sdr_acquisition /usr/local/bin/sdr_acquisition
COPY --from=builder /install/etc/sdr-acquisition /etc/sdr-acquisition

USER sdr

ENV SDR_CONFIG_PATH=/etc/sdr-acquisition/scanner.xml
ENV SDR_LOG_LEVEL=info

ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["/usr/local/bin/sdr_acquisition"]
