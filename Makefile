.PHONY: build build-release build-debug test clean test-parallel benchmark


build-release:
	@cmake -B build -S.
	@cmake -DCMAKE_BUILD_TYPE=Release --build build

build-debug:
	@cmake -B build -S.
	@cmake -DCMAKE_BUILD_TYPE=Debug --build build

build:build-release

test: build
	@build/bin/kvcache_test

clean:
	@rm -rf build ./test.db.* test/test.db.*

benchmark: build
	@build/bin/kvcache_test --gtest_filter='KVCacheTest.Benchmark*'
