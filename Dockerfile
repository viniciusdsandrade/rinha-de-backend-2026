FROM debian:bookworm AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY cpp ./cpp
COPY resources ./resources

RUN cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build cpp/build -j"$(nproc)"
RUN mkdir -p /app/out \
    && /app/cpp/build/prepare-refs-cpp \
        /app/resources/references.json.gz \
        /app/out/references.bin \
        /app/out/labels.bin

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends zlib1g \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/cpp/build/rinha-backend-2026-cpp /app/rinha-backend-2026-cpp
COPY --from=builder /app/out/references.bin /app/data/references.bin
COPY --from=builder /app/out/labels.bin /app/data/labels.bin

ENV REFERENCES_BIN_PATH=/app/data/references.bin
ENV LABELS_BIN_PATH=/app/data/labels.bin
ENV BIND_ADDR=0.0.0.0:3000

EXPOSE 3000

ENTRYPOINT ["/app/rinha-backend-2026-cpp"]
