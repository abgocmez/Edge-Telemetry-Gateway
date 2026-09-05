# Debian bookworm on purpose: it ships GCC 12, which is what Raspberry Pi OS
# bookworm ships. Building here therefore checks the decision to stay on C++20
# rather than C++23 - the compiler on the measurement target is the one that
# defines the floor, and a build that only works on the development machine's
# GCC 15 would be discovered on the Pi rather than in CI.
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src

# Tests are off here so the image build needs no network beyond apt: Catch2
# arrives through FetchContent, and CI already runs the suite natively.
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DETG_BUILD_TESTS=OFF \
      -DETG_WERROR=ON \
 && cmake --build build

FROM debian:bookworm-slim

# can-utils is not needed by the binaries; it is here so the container can be
# used to inspect the bus by hand when something looks wrong.
RUN apt-get update && apt-get install -y --no-install-recommends \
      can-utils iproute2 python3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/src/etg-gateway /usr/local/bin/
COPY --from=builder /src/build/src/etg-gen     /usr/local/bin/
COPY --from=builder /src/build/src/etg-probe   /usr/local/bin/
COPY --from=builder /src/build/src/etg-record  /usr/local/bin/
COPY scripts/parse-capture.py /usr/local/bin/

WORKDIR /data
ENTRYPOINT []
CMD ["etg-gateway", "--help"]
