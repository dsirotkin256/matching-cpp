#!/usr/bin/env bash

set -e

rm -rf build

mkdir -p ./build/

conan install . -if ./build --build=missing

cmake -H. -B./build/

make -C ./build/ -j4

conan remove -f -s -b -- '*'
