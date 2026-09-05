# Debian bookworm on purpose, as a deliberate compiler floor.
#
# It ships GCC 12, which is older than both ends of the real range: the
# development machine has GCC 15 and the Pi currently runs Debian 13 trixie with
# GCC 14.2. Building against the oldest of the three is what keeps the C++20
# decision honest - a feature that quietly needs a newer compiler fails here, in
# CI, rather than on whatever the measurement target happens to be running after
# its next upgrade.
#
# (Earlier revisions of this comment claimed the Pi ran bookworm/GCC 12. It does
# not; that was assumed rather than checked, and checking it is what turned this
# from a matching-compiler argument into a floor argument.)
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
COPY --from=builder /src/build/src/etg-view    /usr/local/bin/
COPY scripts/parse-capture.py /usr/local/bin/

WORKDIR /data
ENTRYPOINT []
CMD ["etg-gateway", "--help"]
