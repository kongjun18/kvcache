#ifndef SSD_H
#define SSD_H

#include "rocksdb/db.h"
#include <string>
#include "status.h"
using std::string;
namespace KVCache
{
    static const string kNumBlocksKey = "/nr_blocks";
    static const string kBlockSizeKey = "/block_size";
    static const string kNumChannelsKey = "/nr_channels";
    static const string kBlockPerChannelKey = "/block_per_channel";
    static const string kBlockPrefix = "/blocks";

    class SSD
    {
    public:
        struct Block
        {
            int block_id;
            int channel_id;
            SSD &ssd;

            Block(SSD &ssd, int block_id);
            Status read(std::string *value);
            Status write(const std::string *value);
        };

    public:
        int nr_channels_;
        int nr_blocks_;
        int block_size_;
        int blocks_per_channel_;

    private:
        rocksdb::DB *db_;
        rocksdb::Options options_;

    public:
        explicit SSD(const string &db_path, const rocksdb::Options *options);
        SSD(const SSD &other) = delete;
        SSD &operator=(const SSD &other) = delete;

        ~SSD();
        static rocksdb::Options default_rocksdb_options();

        Status read_block(int block_id, std::string *value);
        Status write_block(int block_id, const std::string *value);
        template <typename F>
        Status iterate_all_blocks(F &&func);
        static std::string block_key(int channel, int block);
        Block get_block(int block_id);

    };

}

#endif
