# KVCache

This is a KV Cache on SSD, which is inspired by [Twitter'sFatCache](https://github.com/twitter/fatcache) and the paper [Optimizing Flash-based Key-value Cache Systems](https://www.usenix.org/system/files/conference/hotstorage16/hotstorage16_shen.pdf).

This repository has done the following work:

1. Implemented the algorithm of the paper [Optimizing Flash-based Key-value Cache Systems](https://www.usenix.org/system/files/conference/hotstorage16/hotstorage16_shen.pdf). Open-Channel SSD mentioned in the paper is mocked by RocksDB.

2. Concurrency: Get/Put/Delete operations, as well as background flushing and garbage collection (GC). In comparison, [Twitter's FatCache](https://github.com/twitter/fatcache) only supports single-threaded operations.

## Building

KVCache requires the following dependencies:

- C++20 compiler
- CMake 3.20+
- RocksDB
- OpenSSL
- Google Test (for testing)

To build:

```bash
make build
```

## Running Tests

To run the tests:

```bash
make test
```
##  Benchmark

To benchmark the performance:

```bash
make benchmark
```

This benchmark puts and read 4*SSD_SIZE data with random key and value size.

I use a small amount of data to minimize wear on my laptop's disk, you can adjust the test configurations in kvcache_test.cpp. This benchmark writes a large number of small objects (up to a maximum of 200 KB each, totaling 800 MB) and uses only 10 MB of memory.  On my laptop, the performance is as follows:

```sh

Benchmark: Put and Get 4 SSDs with random key and value
write_seconds: 1.67115 read_seconds: 0.775589
Performance test results:
Write: 9777.08 ops/s, actual write 478.72 MiB/s
Read: 105148.54 ops/s, actual read 1098.88 MiB/s
```

[!benchmark](./docs/images/benchmark.png)


