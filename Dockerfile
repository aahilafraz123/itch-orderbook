# Multi-stage build.
#   docker build -t itch-orderbook .                    # build + run all tests
#   docker build --target bench -t itch-bench --build-arg OB_NATIVE=ON .
#   docker run --rm -v "$PWD/data:/data:ro" itch-bench bench /data/12302019.NASDAQ_ITCH50
#
# OB_NATIVE=OFF by default so the image runs on any CPU of the same architecture;
# turn it on when building and benchmarking on the same machine.

FROM ubuntu:24.04 AS build
ARG OB_NATIVE=OFF
RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake ninja-build g++ git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY tests tests
COPY scripts scripts
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_NATIVE=${OB_NATIVE} \
 && cmake --build build

# Tests run as part of the image build: if any fail, the build fails.
FROM build AS test
RUN ctest --test-dir build --output-on-failure

# Small image with just the tools, for running benchmarks against mounted data.
FROM ubuntu:24.04 AS bench
RUN apt-get update \
 && apt-get install -y --no-install-recommends linux-tools-generic util-linux binutils \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/itch_stats /src/build/differential /src/build/bench /src/build/pipeline /usr/local/bin/
COPY --from=build /src/scripts /opt/itch-orderbook/scripts
WORKDIR /data

# Default target: the tested build.
FROM test
