#ifndef SSD_H
#define SSD_H

#include "rocksdb/db.h"
#include <string>
#include "status.h"
using std::string;
namespace KVCache {

const string kNR_BLOCKS_KEY = "/nr_blocks";
const string kBLOCK_SIZE_KEY = "/block_size";
const string kNR_CHANNELS_KEY = "/nr_channels";
const string kBLOCK_PREFIX = "/blocks";

class SSD {
public:
    int nr_channels_;
    int nr_blocks_;
    int block_size_;
private:
    rocksdb::DB* db_;
    rocksdb::Options options_;
public:
    explicit SSD(const string& db_path, const rocksdb::Options *options);
    SSD(const SSD& other) = delete;
    SSD& operator=(const SSD& other) = delete;

    ~SSD();

    Status Read(int channel, int block, std::string* value);
    Status Write(int channel, int block, const std::string* value);

    template<typename F>
    Status IterateAllBlocks(F&& func);

    static std::string BlockKey(int channel, int block);
};

}

#endif
