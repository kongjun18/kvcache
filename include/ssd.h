#ifndef SSD_H
#define SSD_H

#include "rocksdb/db.h"
#include <string>
#include "status.h"
#include <string_view>
#include "ssd.h"
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

        struct SSDConfig
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

    public:
        explicit SSD(const std::string &db_path) : SSD(db_path, default_rocksdb_options()) {}
        SSD(const std::string &db_path, const rocksdb::Options &options);
        SSD(const SSD &other) = delete;
        SSD &operator=(const SSD &other) = delete;

        static SSDConfig default_ssd_config();
        static SSD *create(const std::string &db_path) { return create(db_path, default_ssd_config()); }
        static SSD *create(const std::string &db_path, const SSDConfig &config) { return create(db_path, config, default_rocksdb_options()); }
        static SSD *create(const std::string &db_path, const SSDConfig &config, const rocksdb::Options &rocksdb_options);

        ~SSD();
        static rocksdb::Options default_rocksdb_options();

        Status read_block(int block_id, std::string *value);
        Status write_block(int block_id, std::string_view value);
        template <typename F>
        void iterate_all_blocks(F &&func)
        {
            rocksdb::ReadOptions read_options;
            read_options.prefix_same_as_start = true;
            read_options.total_order_seek = false;
            auto it(db_->NewIterator(read_options));
            for (it->Seek(std::format("{}/", kBlockPrefix)); it->Valid() && it->key().starts_with(kBlockPerChannelKey); it->Next())
            {
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

        int channel_id(int block_id);

    private:
        static std::string block_key(int channel, int block);
        Block get_block(int block_id);
    };

}

#endif
