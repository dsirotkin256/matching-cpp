#!/usr/bin/env bash

export CXXFLAGS=-'-std=c++17'
export CC=clang
export CXX=clang++

set -e

rm -rf build

mkdir -p ./build/

conan install . -s compiler=clang -if ./build --build=missing

cmake -H. -B./build/

make -C ./build/ -j4

conan remove -f -s -b -- '*'
