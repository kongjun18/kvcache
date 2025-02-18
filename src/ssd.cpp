#include "include/ssd.h"
#include <rocksdb/write_batch.h>
#include <rocksdb/db.h>

#include <stdexcept>
#include <format>
namespace KVCache
{

    SSD::Block::Block(SSD &ssd, int block_id) : ssd(ssd), block_id(block_id) {
        channel_id = block_id / ssd.blocks_per_channel_;
    }

    Status SSD::Block::read(std::string *value) {
        std::string key = ssd.block_key(channel_id, block_id);
        rocksdb::Status status = ssd.db_->Get(rocksdb::ReadOptions(), key, value);
        if (!status.ok())
        {
            return Status::Corruption("read failed");
        }
        return Status::OK();
    }

    Status SSD::Block::write(std::string_view value) {
        std::string key = ssd.block_key(channel_id, block_id);
        rocksdb::Status status = ssd.db_->Put(rocksdb::WriteOptions(), key, value);
        if (!status.ok())
        {
            return Status::Corruption("write failed");
        }
        return Status::OK();
    }

    rocksdb::Options SSD::default_rocksdb_options() {
        rocksdb::Options options;
        options.create_if_missing = true;
        options.compression = rocksdb::kLZ4Compression;
        options.write_buffer_size = 64 << 20;
        return options;
    }

    SSD::SSD(const std::string &path, const rocksdb::Options *options)
    {
        options_ = options ? *options : default_rocksdb_options();
        rocksdb::Status status = rocksdb::DB::Open(options_, path, &db_);
        if (!status.ok())
        {
            throw std::runtime_error("open db failed");
        }

        rocksdb::ReadOptions read_options;
        std::string value;
        status = db_->Get(read_options, kNumBlocksKey, &value);
        if (!status.ok())
        {
            throw std::runtime_error("get nr_blocks failed");
        }
        nr_blocks_ = std::stoi(value); 

        status = db_->Get(read_options, kBlockSizeKey, &value);
        if (!status.ok())
        {
            throw std::runtime_error("get block_size failed");
        }
        block_size_ = std::stoi(value);

        status = db_->Get(read_options, kNumChannelsKey, &value);
        if (!status.ok())
        {
            throw std::runtime_error("get nr_channels failed");
        }
        nr_channels_ = std::stoi(value);
    }

    template <typename F>
    Status SSD::iterate_all_blocks(F &&func)
    {
        rocksdb::ReadOptions read_options;
        read_options.prefix_same_as_start = true;
        read_options.total_order_seek = false;
        auto it(db_->NewIterator(read_options));
        for (it->Seek(std::format("{}/{}", kBlockPerChannelKey, channel)); it->Valid() && it->key().starts_with(kBlockPerChannelKey); it->Next())
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
            int block = std::stoi(block_id);
            auto block = Block{block, channel, block_size_};
            Status status = func(block);
            if (!status.ok())
            {
                return status;
            }
        }

        return Status::OK();
    }

    // TODO: finish all pending flush
    SSD::~SSD()
    {
        delete db_;
    }

    std::string SSD::block_key(int channel, int block)
    {
        return std::format("/blocks/{}/{}", channel, block);
    }

    Status SSD::read_block(int block_id, std::string *value)
    {
        return get_block(block_id).read(value);
    }

    Status SSD::write_block(int block_id, std::string_view value)
    {
        return get_block(block_id).write(value);
    }

    inline int SSD::channel_id(int block_id)
    {
        return block_id / blocks_per_channel_;
    }

} // namespace ssd