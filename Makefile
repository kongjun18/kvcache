.PHONY: build test clean test-parallel benchmark

build:
	@cmake -B build -S.
	@cd build && cmake --build . -j$(shell nproc)

test: build
	@cd build && ctest --output-on-failure

clean:
	@rm -rf build

benchmark: build
	@echo "Running benchmarks..."
	@./build/bin/kvcache_benchmark_test --gtest_filter=*Benchmark*

