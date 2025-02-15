#include "include/ssd.h"
#include <rocksdb/write_batch.h>
#include <rocksdb/db.h>

#include <stdexcept>
#include <format>
namespace KVCache
{

    SSD::SSD(const std::string &path, const rocksdb::Options *options)
    {
        if (!options)
        {
            options_ = rocksdb::Options();
            options_.create_if_missing = true;
            options_.compression = rocksdb::kLZ4Compression;
            options_.write_buffer_size = 64 << 20; // 64MB
            options_.max_write_buffer_number = 3;
        }
        else
        {
            options_ = *options;
        }

        rocksdb::Status status = rocksdb::DB::Open(options_, path, &db_);
        if (!status.ok())
        {
            throw std::runtime_error("open db failed");
        }

        rocksdb::ReadOptions read_options;
        std::string value;
        status = db_->Get(read_options, kNR_BLOCKS_KEY, &value);
        if (!status.ok())
        {
            throw std::runtime_error("get nr_blocks failed");
        }
        nr_blocks_ = std::stoi(value); // 将字符串转换为整数

        status = db_->Get(read_options, kBLOCK_SIZE_KEY, &value);
        if (!status.ok())
        {
            throw std::runtime_error("get block_size failed");
        }
        block_size_ = std::stoi(value);

        status = db_->Get(read_options, kNR_CHANNELS_KEY, &value);
        if (!status.ok())
        {
            throw std::runtime_error("get nr_channels failed");
        }
        nr_channels_ = std::stoi(value);
    }

    template <typename F>
    Status SSD::IterateAllBlocks(F &&func)
    {
        rocksdb::ReadOptions read_options;
        read_options.prefix_same_as_start = true;
        read_options.total_order_seek = false;
        auto it(db_->NewIterator(read_options));
        for (it->Seek(kBLOCK_PREFIX); it->Valid() && it->key().starts_with(prefix); it->Next())
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
            Status status = func(channel, block);
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
    std::string SSD::BlockKey(int channel, int block)
    {
        return std::format("/blocks/{}/{}", channel, block);
    }

    Status SSD::Read(int channel, int block, std::string *value)
    {
        std::string key = BlockKey(channel, block);
        rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, value);
        if (!status.ok())
        {
            return Status::Corruption("read failed");
        }
        return Status::OK();
    }

    Status SSD::Write(int channel, int block, const std::string *value)
    {
        std::string key = BlockKey(channel, block);
        rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, *value);
        if (!status.ok())
        {
            return Status::Corruption("write failed");
        }
        return Status::OK();
    }

} // namespace ssd