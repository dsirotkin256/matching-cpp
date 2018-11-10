FROM ubuntu:18.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=GMT
RUN apt-get -y update \
 && apt-get -y dist-upgrade
RUN apt-get -y install software-properties-common build-essential wget curl git tmux
RUN add-apt-repository ppa:ubuntu-toolchain-r/test \
 && apt-get -y update \
 && apt-get -y install clang-5.0 clang++-5.0 libc++-dev libc++abi-dev autoconf clang-tidy-5.0 clang-format-5.0 lldb-5.0 gdb cmake \
 && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-5.0 1000 \
 && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-5.0 1000 \
 && update-alternatives --config clang \
 && update-alternatives --config clang++ \
 && ln -sf /usr/bin/lldb-server-5.0 /usr/lib/llvm-5.0/bin/lldb-server-5.0.0
RUN apt-get install -y python3-pip && pip3 install pip && pip3 install conan
RUN mkdir -p /opt/matching/builder/
WORKDIR /opt/matching/
RUN conan profile show default || conan profile new default --detect
RUN conan profile update settings.compiler=clang default
RUN conan profile update settings.compiler.version=5.0 default
RUN conan profile update settings.compiler.libcxx=libc++ default
RUN conan profile update env.CC=clang-5.0 default
RUN conan profile update env.CXX=clang++-5.0 default
RUN conan profile update env.CXXFLAGS=-stdlib=libc++ default
