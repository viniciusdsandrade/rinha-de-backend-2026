FROM rust:1.95-bookworm AS builder

WORKDIR /app

COPY Cargo.toml Cargo.lock ./
COPY src ./src
COPY tools ./tools
COPY resources ./resources

RUN cargo build --release --locked --bin prepare_refs --bin rinha-backend-2026
RUN mkdir -p /app/out \
    && /app/target/release/prepare_refs \
        /app/resources/references.json.gz \
        /app/out/references.bin \
        /app/out/labels.bin

FROM debian:bookworm-slim AS runtime

WORKDIR /app

COPY --from=builder /app/target/release/rinha-backend-2026 /app/rinha-backend-2026
COPY --from=builder /app/out/references.bin /app/data/references.bin
COPY --from=builder /app/out/labels.bin /app/data/labels.bin

ENV REFERENCES_BIN_PATH=/app/data/references.bin
ENV LABELS_BIN_PATH=/app/data/labels.bin
ENV BIND_ADDR=0.0.0.0:3000

EXPOSE 3000

ENTRYPOINT ["/app/rinha-backend-2026"]

