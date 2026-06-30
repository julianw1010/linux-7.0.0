#!/usr/bin/env bash

set -euo pipefail

echo ">>> Fixing ownership of source tree..."
sudo chown -R "$USER:$(id -gn)" .

PACKAGES=(
    build-essential
    fakeroot
    libncurses-dev
    bison
    flex
    libssl-dev
    libelf-dev
    dwarves
    pahole
    bc
    zstd
    kmod
    cpio
    rsync
    git
)

echo ">>> Updating package lists..."
sudo apt-get update -y

echo ">>> Installing kernel build dependencies..."
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${PACKAGES[@]}"

echo ">>> Copying current kernel config..."
cp "/boot/config-$(uname -r)" .config

echo ">>> Clearing Debian/Canonical trusted and revocation keys..."
./scripts/config --set-str CONFIG_SYSTEM_TRUSTED_KEYS ""
./scripts/config --set-str CONFIG_SYSTEM_REVOCATION_KEYS ""

echo ">>> Running make olddefconfig..."
make olddefconfig

echo ">>> Running make localmodconfig..."
yes "" | make localmodconfig

echo ">>> Running final make olddefconfig..."
make olddefconfig

echo ">>> Done. Your system is ready to build the Linux kernel."
