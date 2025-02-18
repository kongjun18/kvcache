#ifndef SSD_H
#define SSD_H

#include "rocksdb/db.h"
#include <string>
#include "status.h"
#include <string_view>
namespace KVCache
{
    static const std::string kNumBlocksKey = "/nr_blocks";
    static const std::string kBlockSizeKey = "/block_size";
    static const std::string kNumChannelsKey = "/nr_channels";
    static const std::string kBlockPerChannelKey = "/block_per_channel";
    static const std::string kBlockPrefix = "/blocks";

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
            Status write(std::string_view value);
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
        explicit SSD(const std::string &db_path, const rocksdb::Options *options);
        SSD(const SSD &other) = delete;
        SSD &operator=(const SSD &other) = delete;

        ~SSD();
        static rocksdb::Options default_rocksdb_options();

        Status read_block(int block_id, std::string *value);
        Status write_block(int block_id, std::string_view value);
        template <typename F>
        Status iterate_all_blocks(F &&func);
        int channel_id(int block_id);

        private:
        static std::string block_key(int channel, int block);
        Block get_block(int block_id);

    };

}

#endif
