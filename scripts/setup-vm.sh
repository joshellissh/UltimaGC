#!/bin/bash
set -euo pipefail

echo "=== Ultima RPi5 Build Environment Setup ==="

# Detect Ubuntu version
source /etc/os-release 2>/dev/null || true
echo "Running on: ${PRETTY_NAME:-Unknown}"

# Install Buildroot build dependencies
echo "Installing build dependencies..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    libncurses-dev \
    unzip \
    rsync \
    bc \
    git \
    wget \
    cpio \
    python3 \
    file \
    libssl-dev \
    python3-setuptools \
    perl \
    patch \
    gzip \
    bzip2 \
    xz-utils \
    tar \
    device-tree-compiler

# Clone Buildroot if not already present
BUILDROOT_VERSION="2024.11.1"
BUILDROOT_DIR="$HOME/ultima/buildroot"

if [ ! -d "$BUILDROOT_DIR" ]; then
    echo "Cloning Buildroot ${BUILDROOT_VERSION}..."
    mkdir -p "$(dirname "$BUILDROOT_DIR")"
    git clone --depth 1 --branch "${BUILDROOT_VERSION}" \
        https://gitlab.com/buildroot.org/buildroot.git "$BUILDROOT_DIR"
else
    echo "Buildroot already cloned at $BUILDROOT_DIR"
fi

echo "=== Setup complete ==="
