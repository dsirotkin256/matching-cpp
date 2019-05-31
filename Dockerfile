FROM ubuntu:18.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=GMT
RUN apt-get -y update \
 && apt-get -y dist-upgrade
RUN apt-get -y install software-properties-common build-essential linux-tools-common net-tools
RUN apt-get -y install libpthread-stubs0-dev libjemalloc-dev libc++-dev libc++abi-dev
RUN apt-get -y update
RUN apt-get -y install clang-6.0 clang++-6.0 autoconf clang-tidy-6.0 clang-format-6.0 lldb-6.0 postgresql-client
RUN apt-get -y install gdb cmake vim lsof strace htop sudo iftop curl tmux
RUN update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-6.0 1000 \
 && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-6.0 1000 \
 && update-alternatives --config clang \
 && update-alternatives --config clang++
RUN apt-get -y install python3-pip && pip3 install pip && pip3 install conan
RUN mkdir -p /opt/matching/builder/
WORKDIR /opt/matching/
RUN conan profile show default || conan profile new default --detect
RUN conan profile update settings.compiler=clang default
RUN conan profile update settings.compiler.version=6.0 default
RUN conan profile update settings.compiler.libcxx=libc++ default
RUN conan profile update env.CC=clang-6.0 default
RUN conan profile update env.CXX=clang++-6.0 default
RUN conan profile update env.CXXFLAGS=-stdlib=libc++ default
RUN conan remote add ess-dmsc https://api.bintray.com/conan/ess-dmsc/conan
ENV CXXFLAGS=-stdlib=libc++
ENV CC=clang-6.0
ENV CXX=clang++-6.0
