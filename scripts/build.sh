#!/usr/bin/env bash

set -e -v

rm -rf build

mkdir -p ./build/

conan install . -if ./build --build=missing

cmake -H. -B./build/

make -C ./build/ -j $(nproc)

conan remove -f -s -b -- '*'
