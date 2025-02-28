#include "ssd.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/db.h>

#include <stdexcept>
#include <format>

namespace KVCache
{

    void* alloc_aligned_buffer(size_t size, size_t align) {
        void* ptr = NULL;

        if (posix_memalign(&ptr, align, size) != 0) {
            return NULL;
        }
        return ptr;
    }

    SSD::Block::Block(SSD &ssd, int block_id) : ssd(ssd), block_id(block_id)
    {
        channel_id = block_id / ssd.blocks_per_channel_;
    }

    Status SSD::Block::read(std::string *value)
    {
        if (ssd.is_dev_) {
            thread_local char *read_buffer = static_cast<char*>(alloc_aligned_buffer(ssd.block_size_, 4096));
            assert(read_buffer);
            off_t offset = ssd.dstart_ + block_id * ssd.block_size_;
            ssize_t nread = pread(ssd.fd_, read_buffer, ssd.block_size_, offset);
            if (nread != ssd.block_size_) {
                return Status::Corruption("read failed");
            }
            value->assign(read_buffer, ssd.block_size_);
        } else {
            std::string key = ssd.block_key(channel_id, block_id);
            rocksdb::Status status = ssd.db_->Get(rocksdb::ReadOptions(), key, value);
            if (!status.ok())
            {
                return Status::Corruption("read failed");
            }
        }
        return Status::OK();
    }

    Status SSD::Block::write(std::string_view value)
    {
        if (ssd.is_dev_) {
            off_t offset = ssd.dstart_ + block_id * ssd.block_size_;
            ssize_t nwrite = pwrite(ssd.fd_, value.data(), value.size(), offset);
            if (nwrite != value.size()) {
                printf("block_id: %d block_size: %d, buffer: %p offset: %ld, value_size: %d, errno: %d\n:, error %s\n",block_id, ssd.block_size_, value.data(), offset, value.size(),errno, strerror(errno));
                return Status::Corruption(std::format("write failed: %s", strerror(errno)));
            }
        } else {
            std::string key = ssd.block_key(channel_id, block_id);
            rocksdb::Status status = ssd.db_->Put(rocksdb::WriteOptions(), key, value);
            if (!status.ok())
            {
                return Status::Corruption("write failed");
            }
        }
        return Status::OK();
    }

    rocksdb::Options SSD::default_rocksdb_options()
    {
        rocksdb::Options options;
        options.create_if_missing = true;
        options.compression = rocksdb::kLZ4Compression;
        options.write_buffer_size = 64 << 20;
        return options;
    }

    SSD::SSD(const std::string &path) {
        if (path.starts_with("/dev/")) {
            is_dev_ = true;
            fd_ = open(path.c_str(), O_RDWR | O_DIRECT, 0644);
            if (fd_ == -1) {
                throw std::runtime_error("open dev failed");
            }
            dstart_ = 0;
        } else {
            SSD(path, default_rocksdb_options());
        }
    }
    SSD::SSD(const std::string &path, const rocksdb::Options &options)
    {
        options_ = options;
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
        std::cout << nr_blocks_ << std::endl;
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
        status = db_->Get(read_options, kBlockPerChannelKey, &value);
        if (!status.ok())
        {
            throw std::runtime_error("get blocks_per_channel failed");
        }
        blocks_per_channel_ = std::stoi(value);
    }


KVCache::Status SSD::create(const std::string &path, SSD **ssd, const Config &ssd_config, const rocksdb::Options &rocksdb_options)
    {
        if (!path.starts_with("/dev/")) {
            rocksdb::DB *db = nullptr;
            rocksdb::Status status = rocksdb::DB::Open(rocksdb_options, path, &db);
            if (!status.ok())
            {
                return Status::Corruption("open db failed");
            }

            rocksdb::WriteOptions write_options;
            status = db->Put(write_options, kBlockPerChannelKey, std::to_string(ssd_config.blocks_per_channel));
            if (!status.ok())
            {
                delete db;
                return Status::Corruption("put blocks_per_channel failed");
            }

            status = db->Put(write_options, kNumBlocksKey, std::to_string(ssd_config.blocks_per_channel * ssd_config.nr_channels));
            if (!status.ok())
            {
                delete db;
                return Status::Corruption("put num_blocks failed");
            }

            status = db->Put(write_options, kBlockSizeKey, std::to_string(ssd_config.block_size));
            if (!status.ok())
            {
                delete db;
                return Status::Corruption("put block_size failed");
            }

            status = db->Put(write_options, kNumChannelsKey, std::to_string(ssd_config.nr_channels));
            if (!status.ok())
            {
                delete db;
                return Status::Corruption("put nr_channels failed");
            }

            int block_id = 0;
            for (int channel = 0; channel < ssd_config.nr_channels; channel++)
            {
                for (int blocks_per_channel = 0; blocks_per_channel < ssd_config.blocks_per_channel; blocks_per_channel++, block_id++)
                {
                    status = db->Put(write_options, block_key(channel, block_id), "");
                    if (!status.ok())
                    {
                        delete db;
                        return Status::Corruption("put block_key failed");
                    }
                }
            }

            delete db;
            *ssd = new SSD(path, rocksdb_options);
            return Status::OK();
        } else {
            *ssd = new SSD(path);
            (*ssd)->nr_blocks_ = ssd_config.blocks_per_channel * ssd_config.nr_channels;
            (*ssd)->block_size_ = ssd_config.block_size;
            (*ssd)->blocks_per_channel_ = ssd_config.blocks_per_channel;
            (*ssd)->nr_channels_ = ssd_config.nr_channels;
            return Status::OK();
        }
    }

    SSD::~SSD()
    {
        if (is_dev_) {
            close(fd_);
        } else {
            delete db_;
        }
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

    int SSD::channel_id(int block_id)
    {
        return block_id / blocks_per_channel_;
    }

    SSD::Block SSD::get_block(int block_id)
    {
        return Block(*this, block_id);
    }

} // namespace ssd
