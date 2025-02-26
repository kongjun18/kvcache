.PHONY: build build-release build-debug test clean test-parallel benchmark


build-release:
	@cmake -DCMAKE_BUILD_TYPE=Release -B build -S.
	@cd build && cmake --build .

build-debug:
	@cmake -DCMAKE_BUILD_TYPE=Debug -B build -S.
	@cd build && cmake --build .

build:build-release

test: build
	@build/bin/kvcache_test

clean:
	@rm -rf build ./test.db.* test/test.db.*

benchmark: build
	@build/bin/kvcache_test --gtest_filter='KVCacheTest.Benchmark*'
