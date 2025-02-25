#ifndef SSD_H
#define SSD_H

#include <sys/mman.h>
#include "rocksdb/db.h"
#include <string>
#include "status.h"
#include <string_view>
#include "ssd.h"
#include <iostream>
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

        struct Config
        {
            int nr_channels;
            int block_size;
            int blocks_per_channel;
        };

    public:
        int nr_channels_;
        int nr_blocks_;
        int block_size_;
        int blocks_per_channel_;

    private:
        rocksdb::DB *db_;
        rocksdb::Options options_;

        bool is_dev_ = false;
        off_t dstart_ = 0;
        int fd_ = 0;

    public:
        explicit SSD(const std::string &path);
        SSD(const std::string &path, const rocksdb::Options &options);
        SSD(const SSD &other) = delete;
        SSD &operator=(const SSD &other) = delete;

        static Status create(const std::string &path, SSD **ssd, const Config &config, const rocksdb::Options &rocksdb_options = default_rocksdb_options());

        ~SSD();
        static rocksdb::Options default_rocksdb_options();

        Status read_block(int block_id, std::string *value);
        Status write_block(int block_id, std::string_view value);
        template <typename F>
        void iterate_all_blocks(F &&func)
        {
            if (is_dev_)
            {
                int block_id = 0;
                for (int channel = 0; channel < nr_channels_; channel++)
                {
                    for (int blocks_per_channel = 0; blocks_per_channel < blocks_per_channel_; blocks_per_channel++, block_id++)
                    {
                        auto block = Block(*this, block_id);
                        func(block);
                    }
                }
            }
            else
            {

                rocksdb::ReadOptions read_options;
                read_options.prefix_same_as_start = true;
                read_options.total_order_seek = false;
                auto it(db_->NewIterator(read_options));
                for (it->Seek(kBlockPrefix); it->Valid(); it->Next())
                {
                    // Check if key still has the prefix
                    if (!it->key().starts_with(kBlockPrefix))
                    {
                        break;
                    }

                    // blocks/<channel-id>/<block-id>
                    std::string key = it->key().ToString();
                    size_t start = strlen("/blocks/");
                    size_t delimiter = key.find('/', start);
                    if (delimiter == std::string::npos)
                    {
                        continue;
                    }

                    std::string channel_id = key.substr(start, delimiter - start);
                    std::string block_id = key.substr(delimiter + 1);
                    int channel = std::stoi(channel_id);
                    auto block = Block(*this, std::stoi(block_id));
                    func(block);
                }
            }
        }

        int channel_id(int block_id);

    private:
        static std::string block_key(int channel, int block);
        Block get_block(int block_id);
    };

}

#endif
