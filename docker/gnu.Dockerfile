# Builds the glibc runtime + chi wrapper for shipping to glibc Linux distros.
#
# Output artifacts:
#   /src/build/libchrt.a      - glibc-linked runtime (Ubuntu 22.04 baseline)
#   /src/build/libchrt_debug.a
#   /src/build/bin/chi        - glibc-linked chi wrapper
#
# Ubuntu 22.04 is chosen as the oldest still-supported glibc (2.35) so the
# built runtime is forward-compatible with newer glibc distros (Ubuntu 24.04,
# Fedora, Debian testing, Arch). Building on newer glibc would pull in symbol
# versions that older distros lack.
#
# RUNTIME_ONLY=ON skips the compiler/LLVM build path - this image doesn't
# build chic itself, but reuses the statically-linked chic from the musl
# builder image to compile the chi wrapper against glibc libchrt.a.

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
ENV CHI_ROOT=/src
RUN cd build && cmake -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DRUNTIME_ONLY=ON \
    .. && make -j$(nproc) chrt_bundling_target chrt_debug_bundling_target

# Reuse the static chic from the musl builder (musl chic runs on any Linux),
# then compile the chi wrapper against the just-built glibc libchrt.a.
COPY --from=chi-musl-builder /src/build/bin/chic /usr/local/bin/chic
RUN mkdir -p build/bin && chic -c /src/src/chi/main.xs -o /src/build/bin/chi -r -w /tmp/chi-wrapper-build
