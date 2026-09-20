FROM node:20-bookworm-slim AS web-builder

WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY web ./web
COPY vite.config.js ./
RUN npm run build

FROM debian:bookworm-slim AS native-builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY src ./src
COPY Makefile ./
RUN mkdir -p bin && make -B server

FROM debian:bookworm-slim

LABEL org.opencontainers.image.title="Blob Arena"
LABEL org.opencontainers.image.version="1.0.0-rc1"

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=native-builder /app/bin/blob_server /app/blob_server
COPY --from=web-builder /app/web/dist /app/web

RUN useradd --system --uid 10001 blob \
    && mkdir -p /app/data \
    && chown -R blob:blob /app

USER blob
EXPOSE 8080
VOLUME ["/app/data"]

HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8080/health || exit 1

CMD ["./blob_server", "--port", "8080", "--web", "web", "--data-dir", "data", "--io-threads", "2"]
