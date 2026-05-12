FROM debian:trixie AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        build-essential \
        cmake \
        curl \
        ninja-build \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY cpp ./cpp

ARG RINHA_DATA_REF=d501ddc1e941b24014c3ce5a6b41ccc3054ec1a0
RUN mkdir -p /app/resources \
    && curl -fsSL \
        "https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/${RINHA_DATA_REF}/resources/references.json.gz" \
        -o /app/resources/references.json.gz

RUN cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build cpp/build --target rinha-backend-2026-cpp-manual prepare-ivf-cpp -j"$(nproc)"
RUN mkdir -p /app/out \
    && /app/cpp/build/prepare-ivf-cpp \
        /app/resources/references.json.gz \
        /app/out/index.bin \
        1280 \
        65536 \
        6

FROM debian:trixie-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends zlib1g \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/cpp/build/rinha-backend-2026-cpp-manual /app/rinha-backend-2026-cpp-manual
COPY --from=builder /app/out/index.bin /app/data/index.bin

ENV IVF_INDEX_PATH=/app/data/index.bin
ENV BIND_ADDR=0.0.0.0:3000

EXPOSE 3000

ENTRYPOINT ["/app/rinha-backend-2026-cpp-manual"]
