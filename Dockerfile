FROM debian:bookworm AS builder

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc make zlib1g-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY c ./c
COPY resources ./resources

RUN mkdir -p /app/out \
    && make -C c all \
    && /app/c/build/prepare_refs_c \
        /app/resources/references.json.gz \
        /app/out/references.bin \
        /app/out/labels.bin

FROM debian:bookworm-slim AS runtime

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/c/build/rinha-c /app/rinha-c
COPY --from=builder /app/out/references.bin /app/data/references.bin
COPY --from=builder /app/out/labels.bin /app/data/labels.bin

ENV REFERENCES_BIN_PATH=/app/data/references.bin
ENV LABELS_BIN_PATH=/app/data/labels.bin
ENV BIND_ADDR=0.0.0.0:3000

EXPOSE 3000

ENTRYPOINT ["/app/rinha-c"]
