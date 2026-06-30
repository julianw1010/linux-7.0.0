#!/usr/bin/env bash

set -euo pipefail

export LOCALVERSION=

git pull

echo ">>> Fixing ownership of source tree..."
sudo chown -R "$USER:$(id -gn)" .

KVER=$(make -s kernelrelease)
echo ">>> Target kernel version: $KVER"

echo ">>> Removing old /boot files for $KVER..."
sudo rm -f \
    "/boot/vmlinuz-$KVER" \
    "/boot/initrd.img-$KVER" \
    "/boot/initramfs-$KVER.img" \
    "/boot/System.map-$KVER" \
    "/boot/config-$KVER"

make olddefconfig

echo ">>> Building kernel..."
make -j"$(nproc)"

echo ">>> Installing modules..."
sudo make modules_install

echo ">>> Installing kernel image..."
sudo make install

echo ">>> Built and installed kernel $KVER"
