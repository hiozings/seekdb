# syntax=docker/dockerfile:1

# ---- build stage -----------------------------------------------------------
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      g++ \
      cmake \
      make \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY src ./src

RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DMVDB_STATIC_RUNTIME=ON \
 && cmake --build build -j"$(nproc)"

# ---- runtime stage ---------------------------------------------------------
FROM ubuntu:24.04

RUN mkdir -p /data && chmod 777 /data

COPY --from=build /src/build/minivectordb /usr/local/bin/minivectordb

WORKDIR /app

VOLUME ["/data"]

# Foreground process, no network listener: the evaluation driver talks to it
# over stdin/stdout only.
ENTRYPOINT ["/usr/local/bin/minivectordb"]
CMD ["--data-dir", "/data"]
