FROM ubuntu:18.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=GMT
RUN apt -y update \
 && apt -y dist-upgrade
RUN apt -y install software-properties-common build-essential wget curl git tmux net-tools
RUN add-repository ppa:ubuntu-toolchain-r/test ppa:jonathonf/vim \
 && apt -y update \
 && apt -y install clang-6.0 clang++-6.0 libc++-dev libc++abi-dev autoconf clang-tidy-6.0 clang-format-6.0 lldb-6.0 gdb cmake vim \
 && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-6.0 1000 \
 && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-6.0 1000 \
 && update-alternatives --config clang \
 && update-alternatives --config clang++ \
 && ln -sf /usr/bin/lldb-server-6.0 /usr/lib/llvm-6.0/bin/lldb-server-6.0.0
RUN apt install -y python3-pip && pip3 install pip && pip3 install conan
RUN mkdir -p /opt/matching/builder/
WORKDIR /opt/matching/
RUN conan profile show default || conan profile new default --detect
RUN conan profile update settings.compiler=clang default
RUN conan profile update settings.compiler.version=6.0 default
RUN conan profile update settings.compiler.libcxx=libc++ default
RUN conan profile update env.CC=clang-6.0 default
RUN conan profile update env.CXX=clang++-6.0 default
RUN conan profile update env.CXXFLAGS=-stdlib=libc++ default
ENV CXXFLAGS=-'-stdlib=libc++'
ENV CC=clang-6.0
ENV CXX=clang++-6.0
