FROM debian:bullseye-slim AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG CMAKE_BUILD_TYPE=Release
ARG ENABLE_TESTS=OFF
ARG TARGETPLATFORM

WORKDIR /src

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        ccache \
        cmake \
        libb64-dev \
        libcurl4-openssl-dev \
        libdeflate-dev \
        libevent-dev \
        libminiupnpc-dev \
        libnatpmp-dev \
        libpsl-dev \
        libssl-dev \
        ninja-build \
        pkg-config \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src

RUN --mount=type=cache,id=${TARGETPLATFORM}-ccache,target=/root/.ccache \
    cmake -S /src -B /src/build -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DENABLE_DAEMON=ON \
        -DENABLE_UTILS=ON \
        -DENABLE_CLI=OFF \
        -DENABLE_GTK=OFF \
        -DENABLE_QT=OFF \
        -DENABLE_MAC=OFF \
        -DINSTALL_WEB=ON \
        -DENABLE_TESTS="${ENABLE_TESTS}"

RUN --mount=type=cache,id=${TARGETPLATFORM}-ccache,target=/root/.ccache \
    --mount=type=cache,id=${TARGETPLATFORM}-ninja-build,target=/src/build/.ninja \
    cmake --build /src/build --parallel \
    && strip /src/build/daemon/transmission-daemon /src/build/utils/transmission-remote

FROM debian:bullseye-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
ARG COMMIT_HASH=unknown

ENV TRANSMISSION_HOME=/config
ENV TRANSMISSION_DOWNLOAD_DIR=/downloads
ENV TRANSMISSION_INCOMPLETE_DIR=/downloads/incomplete
ENV TRANSMISSION_INCOMPLETE_DIR_ENABLED=false
ENV TRANSMISSION_WATCH_DIR=/watch
ENV TRANSMISSION_WATCH_DIR_ENABLED=false
ENV TRANSMISSION_RPC_ENABLED=true
ENV TRANSMISSION_RPC_BIND_ADDRESS=0.0.0.0
ENV TRANSMISSION_RPC_PORT=9091
ENV TRANSMISSION_RPC_WHITELIST=127.0.0.1,::1
ENV TRANSMISSION_RPC_WHITELIST_ENABLED=false
ENV TRANSMISSION_PEER_PORT=51413
ENV TRANSMISSION_WEB_HOME=/opt/transmission/web/public_html
ENV TRANSMISSION_QUICK_VERIFY_ENABLED=false
ENV TRANSMISSION_QUICK_VERIFY_FALLBACK_ENABLED=false
ENV TRANSMISSION_LOG_LEVEL=error
ENV TRANSMISSION_LOG_FILE=""

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        jq \
        libb64-0d \
        libcurl4 \
        libdeflate0 \
        libevent-2.1-7 \
        libevent-core-2.1-7 \
        libevent-extra-2.1-7 \
        libevent-openssl-2.1-7 \
        libevent-pthreads-2.1-7 \
        libminiupnpc17 \
        libnatpmp1 \
        libpsl5 \
        libssl1.1 \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/daemon/transmission-daemon /usr/local/bin/transmission-daemon
COPY --from=builder /src/build/utils/transmission-remote /usr/local/bin/transmission-remote
COPY --from=builder /src/web/public_html /opt/transmission/web/public_html
COPY docker/entrypoint.sh /usr/local/bin/transmission-entrypoint.sh

RUN chmod +x /usr/local/bin/transmission-entrypoint.sh \
    && mkdir -p /config /downloads /watch \
    && echo "${COMMIT_HASH}" > /commit.txt

EXPOSE 9091 51413/tcp 51413/udp

VOLUME ["/config", "/downloads", "/watch"]

HEALTHCHECK --interval=30s --timeout=10s --start-period=20s --retries=3 \
    CMD sh -ec 'transmission-remote 127.0.0.1:${TRANSMISSION_RPC_PORT} --session-info >/dev/null'

ENTRYPOINT ["/usr/local/bin/transmission-entrypoint.sh"]
CMD ["transmission-daemon", "--foreground", "--config-dir", "/config", "--download-dir", "/downloads"]
