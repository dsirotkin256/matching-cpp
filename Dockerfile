FROM debian:stable

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=GMT
RUN apt -y update \
 && apt -y dist-upgrade
RUN apt -y install software-properties-common build-essential linux-perf net-tools
RUN apt -y install libpthread-stubs0-dev libjemalloc-dev g++
RUN apt -y update
RUN apt -y install gdb cmake vim lsof strace htop sudo iftop curl tmux
RUN apt install -y python3-pip && pip3 install pip && pip3 install conan
RUN mkdir -p /opt/matching/builder/
WORKDIR /opt/matching/
RUN conan profile show default || conan profile new default --detect
RUN conan profile update settings.compiler=gcc default
RUN conan profile update settings.compiler.version=8 default
RUN conan profile update settings.compiler.libcxx=libstdc++11 default
RUN conan profile update env.CC=gcc default
RUN conan profile update env.CXX=g++ default
RUN conan profile update env.CXXFLAGS=-std=c++17 default
RUN conan remote add ess-dmsc https://api.bintray.com/conan/ess-dmsc/conan
RUN conan remote add inexorgame https://api.bintray.com/conan/inexorgame/inexor-conan
ENV CXXFLAGS=-std=c++17
ENV CC=gcc
ENV CXX=g++

EXPOSE 9001 8080
