# Builds the distributable chi compiler + musl runtime + chi wrapper.
#
# Output artifacts:
#   /src/build/bin/chic       - fully static chic (works on any Linux host)
#   /src/build/bin/chi        - musl-linked chi wrapper (for musl targets)
#   /src/build/libchrt.a      - musl-linked runtime for Alpine/musl targets
#   /src/build/libchrt_debug.a
#
# Alpine is the host because musl + static LLVM/clang produce a chic binary
# with zero dynamic dependencies, so a single binary ships everywhere.

FROM alpine:3.19 AS builder

RUN apk update && apk add --no-cache \
    build-base cmake python3 py3-pip git \
    llvm17-dev llvm17-static llvm17-gtest llvm17-test-utils \
    clang17-dev clang17-static \
    zlib-dev zstd-dev ncurses-dev libxml2-dev \
    zlib-static zstd-static ncurses-static libxml2-static \
    pipx

# Install Conan 2
RUN pipx install conan && ln -s /root/.local/bin/conan /usr/bin/conan \
    && conan profile detect

WORKDIR /src
COPY conanfile.txt .
RUN mkdir build && cd build && conan install /src --build=missing --output-folder=. -s build_type=Release

COPY . .
ENV CHI_ROOT=/src
RUN cd build && cmake -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_STATIC_LINK=ON \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    .. && make -j$(nproc) chic chrt_bundling_target chrt_debug_bundling_target

# Build the chi wrapper against the just-built musl libchrt.a.
RUN cd build && ./bin/chic -c /src/src/chi/main.xs -o bin/chi -r -w /tmp/chi-wrapper-build
