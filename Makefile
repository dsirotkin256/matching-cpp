clean:
	-rm -rf build
	-conan remove -f -s -b -- '*'

install:
	-rm -rf build
	-conan install . -if ./build --build=missing
	-cmake -H. -B./build/
	-make -C ./build/ -j $(nproc)
	-conan remove -f -s -b -- '*'

service:
	-./build/bin/matching_service

bench:
	-./build/bin/benchmark
