FROM gcc:14-bookworm AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        cmake \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src

RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/latency \
        -DBUILD_TESTING=OFF \
        -DDXFCXX_BUILD_DOC=OFF \
        -DDXFCXX_BUILD_SAMPLES=OFF \
        -DDXFCXX_BUILD_TOOLS=OFF \
        -DDXFCXX_INSTALL=OFF \
    && cmake --build /build --parallel \
    && cmake --install /build

FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/latency/bin /opt/latency/bin
ENV PATH="/opt/latency/bin:${PATH}"
WORKDIR /work

EXPOSE 7400
CMD ["latency_client", "--help"]
