#!/bin/bash

# Setup script for PSP SDK and libraries
mkdir -p ~/psp-sdk-setup
cd ~/psp-sdk-setup

echo "Fetching latest pspdev release for Ubuntu..."
URL=$(curl -s https://api.github.com/repos/pspdev/pspdev/releases/tags/latest | grep browser_download_url | grep -i "ubuntu" | cut -d '"' -f 4)

if [ -z "$URL" ]; then
    echo "Ubuntu build not found, trying generic linux..."
    URL=$(curl -s https://api.github.com/repos/pspdev/pspdev/releases/tags/latest | grep browser_download_url | grep -i "linux" | cut -d '"' -f 4 | head -n 1)
fi

echo "Downloading from: $URL"
curl -L -o pspdev.tar.gz "$URL"

echo "Extracting PSP SDK to ~/.pspdev ..."
mkdir -p ~/.pspdev
tar -xzf pspdev.tar.gz -C ~/.pspdev --strip-components=1

echo "Adding to PATH (please also add this to your ~/.bashrc)..."
export PSPDEV=$HOME/.pspdev
export PATH=$PATH:$PSPDEV/bin

echo "Fetching psp-cfw-sdk..."
git clone https://github.com/pspdev/psp-cfw-sdk.git
cd psp-cfw-sdk
make install

echo "PSP SDK Setup complete."
