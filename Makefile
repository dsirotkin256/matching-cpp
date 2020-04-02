BUILD_DIR = build

.PHONY: clean build test bench start

build: clean
	conan install . -if $(BUILD_DIR) --build=missing
	cmake -H. -B $(BUILD_DIR)
	make -C $(BUILD_DIR) -j $(shell nproc)
	conan remove -f -s -b -- '*'

clean:
	rm -rf $(BUILD_DIR)
	conan remove -f -s -b -- '*'

start:
	$(BUILD_DIR)/bin/matching_service

bench:
	$(BUILD_DIR)/bin/benchmark

test:
	$(BUILD_DIR)/bin/unit_tests
