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
COPY dxfeed.system.properties ./
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
    && cmake --build /build --parallel 4 \
    && cmake --install /build

# Keep the C++ runtime in lockstep with the compiler used above. Debian Bookworm
# ships GCC 12's libstdc++, which cannot load binaries produced by GCC 14.
RUN mkdir -p /opt/gcc-runtime \
    && cp --dereference "$(c++ -print-file-name=libstdc++.so.6)" /opt/gcc-runtime/libstdc++.so.6 \
    && cp --dereference "$(gcc -print-file-name=libgcc_s.so.1)" /opt/gcc-runtime/libgcc_s.so.1

FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates libstdc++6 zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/latency/bin /opt/latency/bin
COPY --from=build /opt/gcc-runtime /opt/gcc-runtime
RUN printf '%s\n' '/opt/gcc-runtime' > /etc/ld.so.conf.d/gcc-runtime.conf \
    && ldconfig

ENV LD_LIBRARY_PATH="/opt/gcc-runtime"
ENV PATH="/opt/latency/bin:${PATH}"
ENV DXFEED_dxfeed.system.properties="/opt/latency/bin/dxfeed.system.properties"
WORKDIR /work

RUN latency_server --help > /dev/null \
    && latency_client --help > /dev/null \
    && latency_analyzer --help > /dev/null

EXPOSE 7400
CMD ["latency_client", "--help"]
