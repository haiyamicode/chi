# Builds the glibc runtime for shipping to glibc Linux distros.
#
# Output artifacts:
#   /src/build/libchrt.a      - glibc-linked runtime (Ubuntu 22.04 baseline)
#   /src/build/libchrt_debug.a
#
# Ubuntu 22.04 is chosen as the oldest still-supported glibc (2.35) so the
# built runtime is forward-compatible with newer glibc distros (Ubuntu 24.04,
# Fedora, Debian testing, Arch). Building on newer glibc would pull in symbol
# versions that older distros lack.
#
# RUNTIME_ONLY=ON skips the compiler/LLVM build path - this image only
# produces libchrt.a, not chic.

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake python3 python3-pip git ca-certificates \
    && pip3 install conan \
    && conan profile detect

WORKDIR /src
COPY conanfile.txt .
RUN mkdir build && cd build && conan install /src --build=missing --output-folder=. -s build_type=Release

COPY . .
RUN cd build && cmake -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DRUNTIME_ONLY=ON \
    .. && make -j$(nproc) chrt_bundling_target chrt_debug_bundling_target
