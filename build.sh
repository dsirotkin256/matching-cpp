#!/usr/bin/env bash

set -e

rm -rf build
rm -rf builder

mkdir -p ./build/

conan install . -if ./builder --build=missing

cmake -H. -B./build/

make -C ./build/ -j$(nproc)

conan remove -f -s -b -- '*'
