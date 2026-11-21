# syntax=docker/dockerfile:1
# Multi-stage build for emberfall-server. Build context is the repository root:
#   docker build -f deploy/Dockerfile -t emberfall-server .

FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ninja-build pkg-config libsodium-dev libssl-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY server server
COPY tools/bot tools/bot
COPY tools/balance_sim tools/balance_sim
COPY content content
RUN cmake --preset release \
 && cmake --build --preset release \
 && ctest --preset release

FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends libsodium23 libssl3t64 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --create-home --home-dir /var/lib/emberfall emberfall
COPY --from=build /src/build/release/server/emberfall-server /usr/local/bin/emberfall-server
COPY --from=build /src/build/release/tools/bot/emberfall-bot /usr/local/bin/emberfall-bot
COPY content /opt/emberfall/content
COPY deploy/config/server.json /etc/emberfall/server.json
ENV EMBERFALL_CONFIG=/etc/emberfall/server.json
USER emberfall
WORKDIR /var/lib/emberfall
VOLUME /var/lib/emberfall
EXPOSE 7777/udp 7778/tcp 7779/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 \
  CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/7778 && printf 'GET /health HTTP/1.0\r\n\r\n' >&3 && grep -q '\"status\":\"ok\"' <&3"]
ENTRYPOINT ["/usr/local/bin/emberfall-server"]
