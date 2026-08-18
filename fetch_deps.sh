#!/bin/bash

# Script to fetch project dependencies

# 1. Fetch trezor-crypto
echo "Fetching trezor-crypto..."
git clone https://github.com/trezor/trezor-firmware.git
mv trezor-firmware/crypto ./trezor-crypto
rm -rf trezor-firmware

# 2. Fetch mbedtls (version 2.28 or 3.x, let's use 2.28 as it's easier to port to embedded)
echo "Fetching mbedtls..."
git clone -b mbedtls-2.28 --single-branch https://github.com/Mbed-TLS/mbedtls.git

# 3. Fetch intraFont (for graphical UI on PSP)
echo "Fetching intraFont..."
git clone https://github.com/pspdev/intraFont.git

echo "Dependencies fetched successfully!"
