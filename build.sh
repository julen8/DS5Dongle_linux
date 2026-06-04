#!/usr/bin/env bash

PACKAGES=(
    git
    cmake
    ninja-build
    python3
    gcc
    g++
    ca-certificates
    gcc-aarch64-linux-gnu
    g++-aarch64-linux-gnu
    binutils-aarch64-linux-gnu
    pkg-config
    wget
    curl
    libasound2-dev:arm64
)
MISSING=()
for pkg in "${PACKAGES[@]}"; do
    if ! dpkg -s "$pkg" &>/dev/null; then
        MISSING+=("$pkg")
    fi
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo "安装缺失的包: ${MISSING[*]}"
    export DEBIAN_FRONTEND=noninteractive
    export APT_LISTCHANGES_FRONTEND=none
    sudo dpkg --add-architecture arm64
    sudo apt-get update -qq
    sudo -E apt-get install -y -qq \
        -o Dpkg::Options::="--force-confdef" \
        -o Dpkg::Options::="--force-confold" \
        --no-install-recommends "${MISSING[@]}"

    echo "依赖安装完成"
else
    echo "所有依赖已安装，跳过"
fi

PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig \
  PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
  cmake -B build_arm64 -DCMAKE_BUILD_TYPE=Release .

cmake --build build_arm64 -j"$(nproc)"
