#!/usr/bin/env bash
# TODO: Makefile and Dockerfile
g++ -std=c++17 -lprotobuf -lgrpc++ -o bin/trading_info trading_info.cpp proto/**.cc;