#!/usr/bin/env bash


sudo dpkg --add-architecture arm64
sudo apt-get update

sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu cmake pkg-config wget \
    curl git ninja-build libasound2-dev:arm64

PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig \
  PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
  cmake -B build_arm64 -DCMAKE_BUILD_TYPE=Release .

cmake --build build_arm64 -j"$(nproc)"
