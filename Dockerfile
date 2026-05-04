FROM debian:bookworm-slim AS builder

RUN apt-get update \
  && apt-get install -y --no-install-recommends build-essential cmake zlib1g-dev ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmd ./cmd
COPY src ./src
RUN cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build-cmake -j"$(nproc)" --target fraud-server \
  && strip build-cmake/fraud-server

FROM debian:bookworm-slim

RUN apt-get update \
  && apt-get install -y --no-install-recommends libstdc++6 \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build-cmake/fraud-server /app/fraud-server
COPY build/index_k8192.ivfi16 /app/build/index_k8192.ivfi16

ENV PORT=8080
ENV INDEX_PATH=/app/build/index_k8192.ivfi16
ENV BASE_NPROBE=20
ENV AMBIG_NPROBE=40
ENV BBOX_MODE=ambiguous-only
ENV USE_EXACT_FALLBACK=0
ENV USE_FAST_PATH=0
ENV WARMUP=full
ENV WORKERS=1

EXPOSE 8080
CMD ["/app/fraud-server"]
