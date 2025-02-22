.PHONY: build test clean test-parallel benchmark

build:
	@cmake -B build -S.
	@cd build && cmake --build . -j$(shell nproc)

test: build
	@build/bin/kvcache_test

clean:
	@rm -rf build

benchmark: build
	@build/bin/kvcache_test --gtest_filter='KVCacheTest.Benchmark*'