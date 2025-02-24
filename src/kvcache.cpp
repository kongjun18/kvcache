#include "kvcache.h"
#include <sys/mman.h>
#include <algorithm>
#include <openssl/evp.h>
#include <queue>
#include <cmath>
#include <numeric>
namespace KVCache
{
    // align the pointer to the alignment(upper alignment).
    // alignment may not be a power of 2.
    void *align_ptr_up(void *ptr, size_t alignment)
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t remainder = addr % alignment;
        if (remainder == 0)
        {
            return ptr;
        }
        return reinterpret_cast<void *>(addr + (alignment - remainder));
    }

    // align the pointer to the alignment(lower alignment)
    // alignment may not be a power of 2.
    void *align_ptr_down(void *ptr, size_t alignment)
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t remainder = addr % alignment;
        return reinterpret_cast<void *>(addr - remainder);
    }

    void KVCache::slab_init()
    {
        slab_size_ = ssd_->block_size_;
        nr_dslab_ = ssd_->nr_blocks_;
        nr_mslab_ = options_.slab_mem_budget / slab_size_;
        auto mslab_area_size = nr_mslab_ * slab_size_;
        auto dslab_area_size = nr_dslab_ * sizeof(Slab);
        auto alignment = slab_size_;
        mslab_base_ = (Slab *)mmap(nullptr, mslab_area_size + dslab_area_size + alignment, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mslab_base_ == MAP_FAILED)
        {
            throw std::runtime_error("Failed to mmap memory slab");
        }
        mslab_base_ = static_cast<Slab *>(align_ptr_up(mslab_base_, alignment));
        mslab_end_ = reinterpret_cast<Slab *>(reinterpret_cast<char *>(mslab_base_) + mslab_area_size);
        gc_buffer_ = reinterpret_cast<Slab *>(reinterpret_cast<char *>(mslab_base_) + mslab_area_size - slab_size_);
        dslab_base_ = mslab_end_;
        dslab_end_ = dslab_base_ + nr_dslab_;

        dslab_free_.resize(ssd_->nr_channels_);
        std::for_each(dslab_free_.begin(), dslab_free_.end(), [](auto &dslab_free)
                      { INIT_LIST_HEAD(&dslab_free.list_); dslab_free.size_ = 0; });
        dslab_full_.resize(ssd_->nr_channels_);
        std::for_each(dslab_full_.begin(), dslab_full_.end(), [](auto &dslab_full)
                      { INIT_LIST_HEAD(&dslab_full.list_); dslab_full.size_ = 0; });
        ops_pool_.resize(ssd_->nr_channels_);
        std::for_each(ops_pool_.begin(), ops_pool_.end(), [](auto &ops_pool)
                      { INIT_LIST_HEAD(&ops_pool.list_); ops_pool.size_ = 0; });

        for (size_t i = 0; i < nr_mslab_ + nr_dslab_; i++)
        {
            auto slab_info = slab_by_sid(i);
            slab_info->reset();
            slab_info->block_id = -1;
            slab_info->sid = i;
            // Skip gc_buffer_
            if (i < (nr_mslab_ - 1))
            {
                list_add(&slab_info->list, &mslab_free_.list_);
                mslab_free_.size_++;
            }
        }

        int slab_id = nr_mslab_;
        int ops_size = options_.ops_rate * ssd_->blocks_per_channel_;
        ssd_->iterate_all_blocks([this, &slab_id, ops_size](SSD::Block &block)
                                 {
            auto slab_info = slab_by_sid(slab_id);
            slab_info->block_id = block.block_id;
            assert(block.channel_id < ssd_->nr_channels_);
            if (ops_pool_[block.channel_id].size_ < ops_size) {
                list_add(&slab_info->list, &ops_pool_[block.channel_id].list_); 
                ops_pool_[block.channel_id].size_++;
                ops_pool_size_++;
                assert(check_ops_pool());
            } else {
                list_add(&slab_info->list, &dslab_free_[block.channel_id].list_);
                dslab_free_[block.channel_id].size_++;
                nr_free_dslab_++;
                assert(nr_free_dslab_ >= 0 && nr_free_dslab_ <= ssd_->nr_blocks_);
            }
            slab_id = slab_id + 1; });
        max_ops_pool_size_ = ops_pool_size_;
        assert(check_ops_pool());
    }
    void KVCache::slab_class_init()
    {

        nr_slab_class_ = options_.nr_slab_class;
        slab_class_table_ = new SlabClass[nr_slab_class_];
        for (int i = 0; i < nr_slab_class_; i++)
        {
            slab_class_table_[i].cid = i;
            slab_class_table_[i].slot_size = options_.slab_class_size[i];
            slab_class_table_[i].slots_per_slab = ssd_->block_size_ / slab_class_table_[i].slot_size;
            INIT_LIST_HEAD(&slab_class_table_[i].slab_partial);
            const int items = (slab_size_ - Slab::header_size()) / slab_class_table_[i].slot_size;
            const int slot_size = slab_class_table_[i].slot_size;
            const int kv_size = slot_size - Slot::header_size();
        }
    }

    void KVCache::index_init()
    {
        auto nr_index_entry = options_.index_mem_budget / sizeof(IndexEntry);
        auto min_nr_index_entry = std::accumulate(slab_class_table_, slab_class_table_ + nr_slab_class_, 0, [](int sum, const SlabClass &sc) { return sum + sc.slots_per_slab; });
        // If free index_entrys are exhausted and all mdslabs are not full,
        // GC can not reclaim index_entrys. Thus, index_mem_budget must be
        // big enough to hold index_entrys for nr_slab_class_ mdslabs.
        if (nr_index_entry < min_nr_index_entry)
        {
            auto min_index_mem_budget = min_nr_index_entry * sizeof(IndexEntry);
            throw std::invalid_argument(std::format("index_mem_budget {} is too small, min_index_mem_budget {} bytes", options_.index_mem_budget, min_index_mem_budget));
        }
        index_area_base_ = (IndexEntry *)mmap(nullptr, nr_index_entry * sizeof(IndexEntry), PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (index_area_base_ == MAP_FAILED)
        {
            throw std::runtime_error("Failed to allocate index area");
        }
        index_area_end_ = index_area_base_ + nr_index_entry;
        INIT_LIST_HEAD(&free_index_entry_);
        for (size_t i = 0; i < nr_index_entry; i++)
        {
            INIT_LIST_HEAD(&index_area_base_[i].list);
            list_add(&index_area_base_[i].list, &free_index_entry_);
        }
        nr_free_index_entry_ = nr_index_entry;

        index_table_size_ = options_.index_table_size;
        index_table_ = new struct list_head[index_table_size_];
        for (size_t i = 0; i < index_table_size_; i++)
        {
            INIT_LIST_HEAD(&index_table_[i]);
        }
    }

    KVCache::Options KVCache::DefaultOptions(SSD *ssd)
    {
        Options options;
        options.slab_mem_budget = 1 * GB;
        options.index_mem_budget = 100 * MB;
        options.index_table_size = 512 * KB;
        options.enable_background_flush = true;
        options.enable_background_gc = true;
        options.ops_rate = 0.2;
        options.free_block_slab_water_mark_low_min = 0.02;
        options.free_block_slab_water_mark_high_max = 0.90;
        options.free_block_water_mark_low = 0.07;
        options.free_block_water_mark_high = 0.80;
        default_slab_class_size(ssd->block_size_, 32, options.slab_class_size, kMaxNumSlabClass, &options.nr_slab_class);
        return options;
    }

    KVCache::KVCache(SSD *ssd, const Options &options)
        : ssd_(ssd), options_(options)
    {
        free_block_water_mark_low_min_ = std::max(1, static_cast<int>(options_.free_block_slab_water_mark_low_min * ssd_->nr_blocks_));
        free_block_water_mark_high_max_ = std::max(free_block_water_mark_low_min_ + 10, static_cast<int>(options_.free_block_slab_water_mark_high_max * ssd_->nr_blocks_));
        free_block_water_mark_low_ = std::max(free_block_water_mark_low_min_, static_cast<int>(options_.free_block_water_mark_low * ssd_->nr_blocks_));
        free_block_water_mark_high_ = std::min(free_block_water_mark_high_max_, static_cast<int>(options_.free_block_water_mark_high * ssd_->nr_blocks_));

        slab_init();
        slab_class_init();
        index_init();
    }

    Status KVCache::get_slice(std::string_view key, std::string *read_buffer, std::string_view *value)
    {
        std::shared_lock<std::shared_mutex> reader_lock(index_mutex_);
        auto index_entry = get_index_entry(key);
        if (!index_entry)
        {
            return Status::NotFound("Key not found");
        }
        assert(index_entry->slab_id != -1);

        auto slab = slab_by_sid(index_entry->slab_id);
        std::string_view k, v;
        if (slab->im_memory())
        {
            assert(index_entry->slot_id < slab->nr_alloc);
            auto slot = slab->slot(index_entry->slot_id);
            k = slot->key();
            v = slot->value();
        }
        else
        {
            auto dslab = read_dslab(slab, read_buffer);
            assert(dslab);
            auto slot = dslab->slot(index_entry->slot_id);
            k = slot->key();
            v = slot->value();
        }
        if (key != k)
        {
            return Status::NotFound("Key not found");
        }
        *value = v;
        return Status::OK();
    }

    Status KVCache::Get(std::string_view key, std::string *value)
    {
        std::string read_buffer;
        std::string_view value_slice;
        auto status = get_slice(key, &read_buffer, &value_slice);
        if (!status.ok())
        {
            return status;
        }
        *value = value_slice;
        return Status::OK();
    }

    Status KVCache::Get(std::string_view key, char buffer[], size_t buffer_len, size_t *value_len)
    {
        std::string read_buffer;
        std::string_view value_slice;
        auto status = get_slice(key, &read_buffer, &value_slice);
        if (!status.ok())
        {
            return status;
        }
        if (value_slice.size() > buffer_len)
        {
            return Status::BufferTooSmall("Buffer is too small");
        }
        memcpy(buffer, value_slice.data(), value_slice.size());
        *value_len = value_slice.size();
        return Status::OK();
    }

    Status KVCache::Put(std::string_view key, std::string_view value)
    {
        {
            auto writer_lock = std::unique_lock(writer_mutex_);

            Slot *slot;
            auto status = slot_alloc(slot_size(key, value), &slot);
            if (!status.ok())
            {
                return status;
            }
            slot->Write(key, value);

            // Update index entry
            auto slab = slot_to_slab(slot);
            {
                std::unique_lock<std::shared_mutex> reader_lock(index_mutex_);
                auto index_entry = get_index_entry(key);
                if (index_entry)
                {
                    auto old_slab_id = index_entry->slab_id;
                    assert(old_slab_id != -1);
                    auto old_slabinfo = slab_by_sid(old_slab_id);
                    // This line needs to be protected by reader_mutex_, otherwise
                    // Del may decrement the nr_used of the old slab while Put is
                    // inserting the new slab, causing repeated decrement.
                    assert(old_slabinfo->nr_used.fetch_sub(1) > 0);
                    index_entry->slab_id = slab->sid;
                    index_entry->slot_id = slot_id(slot);
                }
                else
                {
                    IndexEntry *index_entry = nullptr;
                    while (!(index_entry = index_entry_alloc(key)))
                    {
                        // Flush full mslabs and GC to release index entries.

                        slab_flush();
                        slab_gc();
                        gc_finished_signal_.wait(reader_lock, [this]
                                                 { return nr_free_index_entry_ > 0; });
                    }
                    auto digest = make_digest(key);
                    index_entry->digest = digest;
                    index_entry->slab_id = slab->sid;
                    index_entry->slot_id = slot_id(slot);
                    {
                        put_index_entry(key, index_entry);
                    }
                }
            }

            // Once the slab is inserted into mslab_full_, flush thread may flush it to dslab
            // and update the index_entry pointing to the new dslab. So we must insert the slab
            // into mslab_full after the index_entry is inserted into index_table_, otherwise
            // updates of flush thread will be lost.
            if (slab->is_full())
            {
                std::unique_lock<std::mutex> mslab_full_lock(mslab_full_mutex_);
                list_add(&slab->list, &mslab_full_.list_);
                mslab_full_.size_++;
                if (options_.enable_background_flush)
                {
                    slab_flush();
                }
            }
        }
        return Status::OK();
    }
    void KVCache::put_index_entry(std::string_view key, IndexEntry *entry)
    {
        auto index_table_index = hasher_(key) % index_table_size_;
        list_add(&entry->list, &index_table_[index_table_index]);
    }

    // get_index_entry may returns the same index entry for different ke
    // if they have the same std::hash value and the same digest.
    IndexEntry *KVCache::get_index_entry(std::string_view key)
    {
        auto index_table_index = hasher_(key) % index_table_size_;
        IndexEntry *entry;
        Digest digest;
        digest = make_digest(key);
        list_for_each_with_entry(IndexEntry, entry, &index_table_[index_table_index], list)
        {
            if (entry->digest == digest)
            {
                return entry;
            }
        }
        return nullptr;
    }

    Digest KVCache::make_digest(std::string_view key)
    {
        Digest digest;                      // SHA-1 摘要长度为 20 字节
        EVP_MD_CTX *ctx = EVP_MD_CTX_new(); // 创建一个新的 EVP_MD_CTX 对象

        if (!ctx)
        {
            throw std::runtime_error("Failed to allocate EVP_MD_CTX");
        }

        // 初始化 SHA-1 摘要算法
        if (1 != EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr))
        {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("SHA-1 initialization failed");
        }

        // 更新数据
        if (1 != EVP_DigestUpdate(ctx, key.data(), key.size()))
        {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("SHA-1 update failed");
        }

        // 获取最终摘要
        unsigned int digest_len = 0;
        if (1 != EVP_DigestFinal_ex(ctx, digest.data(), &digest_len))
        {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("SHA-1 finalization failed");
        }

        EVP_MD_CTX_free(ctx); // 释放上下文对象

        return digest; // 返回摘要
    }

    inline size_t KVCache::slot_size(std::string_view key, std::string_view value)
    {
        return Slot::header_size() + key.size() + value.size();
    }

    IndexEntry *KVCache::index_entry_alloc(std::string_view key)
    {
        if (list_empty(&free_index_entry_))
        {
            return nullptr;
        }
        auto nr_free_index_entry = 0;
        IndexEntry *index_entry = nullptr;
        list_for_each_with_entry(IndexEntry, index_entry, &free_index_entry_, list)
        {
            nr_free_index_entry++;
        }
        assert(nr_free_index_entry >= 0 && nr_free_index_entry == nr_free_index_entry_);
        index_entry = list_first_entry(&free_index_entry_, IndexEntry, list);
        list_del(&index_entry->list);
        nr_free_index_entry_--;
        return index_entry;
    }

    inline Slab *KVCache::slot_to_slab(const Slot *slot)
    {
        auto slot_addr = reinterpret_cast<uintptr_t>(slot);
        auto diff = (slot_addr - reinterpret_cast<uintptr_t>(mslab_base_));
        auto slab = reinterpret_cast<Slab *>(reinterpret_cast<char *>(mslab_base_) + diff);
        slab = static_cast<Slab *>(align_ptr_down(slab, slab_size_));
        assert(slab >= mslab_base_ && slab < mslab_end_);
        return slab;
    }
    inline int KVCache::slot_id(const Slot *slot)
    {
        auto slab = slot_to_slab(slot);
        auto diff = reinterpret_cast<uintptr_t>(slot) - reinterpret_cast<uintptr_t>(slab->data);
        return diff / slab->slot_size;
    }

    Status KVCache::slot_alloc(size_t size, Slot **slotp)
    {
        SlabClass *slab_class;
        auto status = get_slab_class(size, &slab_class);
        if (!status.ok())
        {
            return status;
        }
        if (!list_empty(&slab_class->slab_partial))
        {
            auto slab = list_first_entry(&slab_class->slab_partial, Slab, list);
            assert(slab->cid >= -1);
            assert(!slab->is_full());
            auto slot = slab->slot_alloc();
            *slotp = slot;
            if (slab->is_full())
            {
                list_del(&slab->list);
            }
            return Status::OK();
        }

        assert(list_empty(&slab_class->slab_partial));
        std::unique_lock<std::mutex> mslab_free_lock(mslab_free_mutex_);
        if (!list_empty(&mslab_free_.list_))
        {
            auto slab = list_first_entry(&mslab_free_.list_, Slab, list);
            list_del(&slab->list);
            mslab_free_.size_--;
            mslab_free_lock.unlock();

            slab->cid = slab_class->cid;
            assert(slab->cid != -1);
            slab->slot_size = slab_class->slot_size;
            slab->nr_slots = (slab_size_ - Slab::header_size()) / slab_class->slot_size;
            list_add(&slab->list, &slab_class->slab_partial);
            // retry
            return slot_alloc(size, slotp);
        }
        mslab_free_lock.unlock();

        // assert(list_empty(&mslab_free_.list_));
        // assert(!list_empty(&mslab_full_.list_));
        slab_flush();
        if (!wait_slab_flush_until_shutdown())
        {
            return Status::Shutdown("KVCache is shutting down");
        }
        return slot_alloc(size, slotp);
    }

    inline Slab *KVCache::slab_by_sid(int sid)
    {
        assert(sid < nr_mslab_ + nr_dslab_);
        if (sid < nr_mslab_)
        {
            return reinterpret_cast<Slab *>(reinterpret_cast<char *>(mslab_base_) + sid * slab_size_);
        }
        return dslab_base_ + (sid - nr_mslab_);
    }

    Status KVCache::get_slab_class(size_t size, SlabClass **slab_class)
    {
        auto it = std::lower_bound(slab_class_table_, slab_class_table_ + nr_slab_class_, size,
                                   [](const SlabClass &sc, size_t target)
                                   { return sc.slot_size < target; });
        if (it < (slab_class_table_ + nr_slab_class_) && it->slot_size >= size)
        {
            *slab_class = it;
            return Status::OK();
        }
        return Status::ObjectTooLarge("Object is too large");
    }

    void KVCache::slab_flush()
    {
        if (options_.enable_background_flush)
        {
            if (!slab_flush_thread_started_.test_and_set())
            {
                slab_flush_thread_ = std::thread([this]()
                                                 {
                    while (1)
                    {
                        if (!do_slab_flush())
                        {
                            break;
                        }
                    } });
            }
            flush_signal_.notify_one();
        }
        else
        {
            do_slab_flush();
        }
    }

    bool KVCache::wait_slab_flush_until_shutdown()
    {
        if (options_.enable_background_flush)
        {
            std::unique_lock<std::mutex> mslab_free_lock(mslab_free_mutex_);
            flush_finished_signal_.wait(mslab_free_lock, [this]()
                                        { return !list_empty(&mslab_free_.list_) || shutdown_; });
            return !shutdown_;
        }
        return true;
    }

    bool KVCache::do_slab_flush()
    {
        Slab *dslab = nullptr;
        Slab *mslab = nullptr;
        int dslab_channel = -1;

        // search for free dslab and full mslab
        {
            std::unique_lock<std::mutex> mslab_full_lock(mslab_full_mutex_);
            flush_signal_.wait(mslab_full_lock, [this]()
                               { return !list_empty(&mslab_full_.list_) || shutdown_; });
            if (shutdown_)
            {
                return false;
            }
            mslab = list_first_entry(&mslab_full_.list_, Slab, list);
            assert(mslab->cid != -1);
            list_del(&mslab->list);
            mslab_full_.size_--;
        }

        assert(mslab);
        while (!dslab)
        {
            {
                std::unique_lock<std::mutex> dslab_free_lock(dslab_free_mutex_);
                auto beginning = next_mslab_flush_channel_;
                do
                {
                    auto channel = next_mslab_flush_channel_;
                    next_mslab_flush_channel_ = next_channel(channel);
                    if (!list_empty(&dslab_free_[channel].list_))
                    {
                        dslab = list_first_entry(&dslab_free_[channel].list_, Slab, list);
                        assert(dslab->cid == -1);
                        list_del(&dslab->list);
                        dslab_free_[channel].size_--;
                        nr_free_dslab_--;
                        assert(nr_free_dslab_ >= 0);
                        dslab_channel = channel;
                        break;
                    }
                } while (next_mslab_flush_channel_ != beginning);
            }
            if (dslab)
            {
                break;
            }

            // Wait for GC to reclaim some dslab
            assert(!dslab);
            slab_gc();
            if (!wait_slab_gc_until_shutdown())
            {
                return false;
            }
        }

        assert(dslab);
        assert(mslab);
        assert(dslab_channel > -1 && dslab_channel < ssd_->nr_channels_);
        flush_mslab_to_dslab(mslab, dslab);
        assert(dslab->cid == mslab->cid);
        assert(dslab->cid != -1);

        // Update index table
        {
            std::unique_lock<std::shared_mutex> reader_lock(index_mutex_);
            modify_index_to(mslab, dslab);
        }

        // mslab -> mslab_free
        // dslab -> dslab_full
        {
            mslab->reset();
            std::unique_lock<std::mutex> mslab_free_lock(mslab_free_mutex_);
            list_add(&mslab->list, &mslab_free_.list_);
            mslab_free_.size_++;
        }
        {
            std::unique_lock<std::mutex> dslab_full_lock(dslab_full_mutex_);
            list_add(&dslab->list, &dslab_full_[dslab_channel].list_);
            dslab_full_[dslab_channel].size_++;
            nr_full_dslab_++;
        }

        flush_finished_signal_.notify_one();
        gc_signal_.notify_one();
        return true;
    }

    inline int KVCache::next_channel(int current_channel)
    {
        return (current_channel + 1) % ssd_->nr_channels_;
    }

    void KVCache::slab_gc()
    {
        if (options_.enable_background_gc)
        {
            if (!slab_gc_thread_started_.test_and_set())
            {
                slab_gc_thread_ = std::thread([this]()
                                              {
                    while (1)
                    {
                        if (!do_slab_gc())
                        {
                            break;
                        }
                    } });
                gc_signal_.notify_one();
            }
        }
        else
        {
            do_slab_gc();
        }
    }

    bool KVCache::wait_slab_gc_until_shutdown()
    {
        if (options_.enable_background_gc)
        {
            std::unique_lock<std::mutex> dslab_free_lock(dslab_free_mutex_);
            gc_finished_signal_.wait(dslab_free_lock, [this]()
                                     { return nr_free_dslab_ > 0 || shutdown_; });
            return !shutdown_;
        }
        return true;
    }

    bool KVCache::do_slab_gc()
    {
        {
            std::unique_lock<std::mutex> dslab_full_lock(dslab_full_mutex_);
            gc_signal_.wait(dslab_full_lock, [this]()
                            { return nr_full_dslab_ > 0 || shutdown_; });
            if (shutdown_)
            {
                return false;
            }
        }

        dslab_free_mutex_.lock();
        auto nr_free_dslab = nr_free_dslab_;
        dslab_free_mutex_.unlock();
        assert(nr_free_dslab >= 0);

        auto run_next_round = false;
        if (!nr_free_index_entry_)
        {
            // Drop one full dslab to release index entries.
            run_next_round = do_quick_gc(1, 0);
        }
        else if (nr_free_dslab < free_block_water_mark_low_)
        {
            run_next_round = quick_gc();
        }
        else
        {
            run_next_round = normal_gc();
        }

        gc_finished_signal_.notify_all();
        return run_next_round;
    }

    void KVCache::flush_mslab_to_dslab(Slab *mslab, Slab *dslab)
    {
        auto block_data = std::string_view(reinterpret_cast<char *>(mslab), slab_size_);
        auto status = ssd_->write_block(dslab->block_id, block_data);
        // TODO: handle error
        if (!status.ok())
        {
            throw std::runtime_error("Failed to flush mslab to dslab");
        }

        // assert(list_empty(&dslab->list));
        dslab->cid = mslab->cid;
        dslab->slot_size = mslab->slot_size;
        dslab->nr_slots = mslab->nr_slots;
        // mslab is full or gc_buffer_, so nr_alloc is immutable.
        dslab->nr_alloc.store(mslab->nr_alloc.load());
        // if mslab is gc_buffer_, nr_used may be decreased before load(),
        // causing overestimated dslab nr_used.
        dslab->nr_used.store(mslab->nr_used.load());
    }

    // Drop full dslab directly to release disk space and index entries.
    bool KVCache::do_quick_gc(int nr_back_to_free_dslab, int nr_back_to_ops_pool)
    {
        assert(ops_pool_size_ > 0 && ops_pool_size_ <= max_ops_pool_size_);
        assert(check_ops_pool());

        // Select dslabs to drop
        int nr_dslab_to_drop = nr_back_to_free_dslab + nr_back_to_ops_pool;
        struct list_head to_drop;
        INIT_LIST_HEAD(&to_drop);
        {
            std::unique_lock<std::mutex> dslab_full_lock(dslab_full_mutex_);
            while (nr_dslab_to_drop > 0)
            {
                auto channel = next_channel(next_ops_pool_remove_channel_) % ssd_->nr_channels_;
                next_ops_pool_remove_channel_ = (next_ops_pool_remove_channel_ + 1) % ssd_->nr_channels_;
                if (!list_empty(&dslab_full_[channel].list_))
                {
                    auto dslab = list_first_entry(&dslab_full_[channel].list_, Slab, list);
                    list_del(&dslab->list);
                    dslab_full_[channel].size_--;
                    nr_full_dslab_--;
                    list_add(&dslab->list, &to_drop);
                    nr_dslab_to_drop--;
                }
            }
        }

        // Reclaim dslabs back to ops_pool_ or dslab_free_
        int i = 0;
        Slab *dslab = nullptr;
        Slab *next = nullptr;
        static std::string read_buffer(slab_size_, '\0');
        // Delete index entries of evicted dslabs
        list_for_each_with_entry(Slab, dslab, &to_drop, list)
        {
            auto fake_slab = read_dslab(dslab, &read_buffer);
            assert(fake_slab->sid == dslab->sid);
            std::unique_lock<std::shared_mutex> reader_lock(index_mutex_);
            evict_dslab(fake_slab, &read_buffer);
            dslab->nr_used.store(0);
        }
        // Reclaim dslabs back to ops_pool_
        while (!list_empty(&to_drop))
        {
            auto dslab = list_first_entry(&to_drop, Slab, list);
            list_del(&dslab->list);
            dslab->reset();

            auto channel = ssd_->channel_id(dslab->block_id);
            if (i++ >= nr_back_to_ops_pool)
            {
                break;
            }
            list_add(&dslab->list, &ops_pool_[channel].list_);
            ops_pool_[channel].size_++;
            ops_pool_size_++;
            assert(check_ops_pool());
        }
        // Reclaim dslabs back to dslab_free_
        {
            std::unique_lock<std::mutex> dslab_free_lock(dslab_free_mutex_);
            while (!list_empty(&to_drop))
            {
                auto dslab = list_first_entry(&to_drop, Slab, list);
                list_del(&dslab->list);
                dslab->reset();

                auto channel = ssd_->channel_id(dslab->block_id);
                list_add(&dslab->list, &dslab_free_[channel].list_);
                dslab_free_[channel].size_++;
                nr_free_dslab_++;
                assert(nr_free_dslab_ >= 0 && nr_free_dslab_ <= ssd_->nr_blocks_);
            }
        }
        assert(check_ops_pool());
        return true;
    }

    // 1. Drop full dslab directly to respond high disk space pressure immediately.
    // 2. Increase ops_pool_size to speedup normal GC.
    // 3. Increase high/low water maker.
    bool KVCache::quick_gc()
    {

        dslab_free_mutex_.lock();
        int nr_free_dslab = nr_free_dslab_;
        dslab_free_mutex_.unlock();
        assert(nr_free_dslab > -1);
        assert(ops_pool_size_ > 0 && ops_pool_size_ <= max_ops_pool_size_);
        assert(check_ops_pool());
        // Low water mark means the system is under high disk space pressure.
        // In this case, we need to drop some full dslab to free up disk space immediately
        // and ops_pool_size to speedup normal GC.
        int old_free_block_water_mark_low = free_block_water_mark_low_;
        int old_free_block_water_mark_high = free_block_water_mark_high_;

        // water mark determines the number of dslabs to free and GC methods.
        // We increase the high water mark to 1.5x for quick response.
        free_block_water_mark_high_ = std::min(static_cast<int>(1.5 * free_block_water_mark_high_), free_block_water_mark_high_max_);
        free_block_water_mark_low_ = std::min(static_cast<int>(1.5 * free_block_water_mark_low_), static_cast<int>(0.9 * free_block_water_mark_high_));
        free_block_water_mark_low_ = std::min(free_block_water_mark_low_, max_ops_pool_size_);

        // Double ops_pool_size_ to speedup normal GC.
        //
        // Free dslabs to free_block_water_mark_low_
        // plus 0.3x extra dslab to give the system more
        // chance to reach [low_water_mark, high_water_mark]
        // and use normal GC to reclaim dslabs.
        int nr_back_to_ops_pool = std::min(ops_pool_size_, max_ops_pool_size_ - ops_pool_size_);
        int nr_back_to_free_dslab = free_block_water_mark_low_ - nr_free_dslab;
        nr_back_to_free_dslab += 0.3 * free_block_water_mark_low_;
        int nr_dslab_to_drop = nr_back_to_free_dslab + nr_back_to_ops_pool;

        return do_quick_gc(nr_back_to_free_dslab, nr_back_to_ops_pool);
    }

    void KVCache::evict_dslab(Slab *dslab, std::string *read_buffer)
    {
        auto fake_slab = read_dslab(dslab, read_buffer);
        fake_slab->for_each_slot([this, dslab](Slot *slot, int slot_id)
                                 {
            del_index_entry(slot->key());
            return true; });
        dslab->nr_used.store(0);
    }

    // reader_mutex_ must be exclusively held before calling this function
    void KVCache::del_index_entry(std::string_view key)
    {
        auto index_entry = get_index_entry(key);
        if (index_entry)
        {
            list_del(&index_entry->list);
            index_entry_free(index_entry);
        }
    }

    // reader_mutex_ must be exclusively held before calling this function
    void KVCache::index_entry_free(IndexEntry *entry)
    {
        list_add(&entry->list, &free_index_entry_);
        nr_free_index_entry_++;
    }

    // read_dslab return a fake mslab that can be used to read slots of dslab
    Slab *KVCache::read_dslab(const Slab *dslab, std::string *read_buffer)
    {
        auto status = ssd_->read_block(dslab->block_id, read_buffer);
        if (!status.ok())
        {
            throw std::runtime_error("Failed to read block");
        }
        auto slab = reinterpret_cast<Slab *>(read_buffer->data());
        *slab = *dslab;
        return slab;
    }

    // GC full dslabs with largest number of used slots until ops_pool_ is full
    bool KVCache::normal_gc()
    {
        std::priority_queue<Slab *, std::vector<Slab *>, SlabGCPriorityComparator> pq;
        for (int channel = 0; channel < ssd_->nr_channels_; channel++)
        {
            std::unique_lock<std::mutex> dslab_full_lock(dslab_full_mutex_);
            Slab *dslab = nullptr;
            list_for_each_with_entry(Slab, dslab, &dslab_full_[channel].list_, list)
            {
                if (pq.size() < 3 * ops_pool_size_)
                {
                    pq.push(dslab);
                }
                else if (SlabGCPriorityComparator{}(dslab, pq.top()))
                {
                    pq.pop();
                    pq.push(dslab);
                }
            }
        }

        // Calculate the number of ops dslab needed for GC for each slab_class
        auto slab_class_to_drop = std::vector<std::vector<Slab *>>(nr_slab_class_);
        while (!pq.empty())
        {
            auto dslab = pq.top();
            pq.pop();
            // Descending order based on the number of used slots
            assert(dslab->cid != -1);
            slab_class_to_drop[dslab->cid].push_back(dslab);
        }

        static std::string read_buffer(slab_size_, '\0');
        auto free_list = std::vector<List>(ssd_->nr_channels_);
        auto old_ops_size = ops_pool_size_;
        for (int i = 0; i < nr_slab_class_; i++)
        {
            auto nr_evicted_slots = 0;
            auto to_drop = std::vector<Slab *>();

            while (!slab_class_to_drop[i].empty())
            {
                auto dslab = slab_class_to_drop[i].back();
                auto nr_used = dslab->nr_used.load();
                assert(nr_used >= 0);
                auto nr_total_slots = dslab->nr_slots;
                auto nr_slots_will_be_evicted = (nr_used + nr_evicted_slots);
                // Accumulate enough valid objects to move to evict_buffer
                if (nr_slots_will_be_evicted > nr_total_slots)
                {
                    if (to_drop.size() > 1)
                    {
                        if (ops_pool_size_ <= 0)
                        {
                            goto tune_ops_pool_size;
                        }
                        assert(check_ops_pool());
                        gc_dslabs(to_drop, &read_buffer, free_list);
                        to_drop.clear();
                        nr_evicted_slots = 0;
                    }
                    // to_drop.size() == 1 means that drop a full dslab need to
                    // consume a ops_pool dslab, which is meaningless. We have
                    // to stop here because other full dslabs have move used slots.
                    break;
                }
                slab_class_to_drop[i].pop_back();
                to_drop.push_back(dslab);
                nr_evicted_slots += nr_used;
            }
            if (to_drop.size() > 1)
            {
                if (ops_pool_size_ <= 0)
                {
                    goto tune_ops_pool_size;
                }
                assert(check_ops_pool());
                gc_dslabs(to_drop, &read_buffer, free_list);
            }
        }

    tune_ops_pool_size:
        //  tune ops_pool size
        int new_ops_size = old_ops_size;
        {
            std::unique_lock<std::mutex> dslab_free_lock(dslab_free_mutex_);
            // Hit high water mark, increase high water mark
            if (nr_free_dslab_ > free_block_water_mark_high_)
            {
                free_block_water_mark_low_ = std::max(free_block_water_mark_low_ - 10, free_block_water_mark_low_min_);
                free_block_water_mark_high_ = std::max(free_block_water_mark_high_ - 10, static_cast<int>(1.1 * free_block_water_mark_low_));
                new_ops_size = std::max(10, old_ops_size - 1);
            }
            // quick_gc handles low water mark case.
            else if (nr_free_dslab_ < free_block_water_mark_low_)
            {
                // DO NOTHING.
            }
            else
            { // Normal water mark
                new_ops_size = old_ops_size;
            }
        }
        // Reclaim dslabs back to ops_pool_ and dslab_free_
        int nr_reclaimed = 0;
        int ops_dslab_consumed = old_ops_size - ops_pool_size_;
        for (const auto &list : free_list)
        {
            nr_reclaimed += list.size_;
        }
        // Tune ops_pool_size_ to new_ops_size
        assert(nr_reclaimed == 0 || nr_reclaimed >= ops_dslab_consumed);
        for (int channel = 0, i = 0; i < ops_dslab_consumed;)
        {
            if (!list_empty(&free_list[channel].list_))
            {
                auto dslab = list_first_entry(&free_list[channel].list_, Slab, list);
                list_del(&dslab->list);
                dslab->reset();
                list_add(&dslab->list, &ops_pool_[channel].list_);
                ops_pool_[channel].size_++;
                ops_pool_size_++;
                assert(check_ops_pool());
                i++;
            }
            channel = next_channel(channel);
        }
        assert(ops_pool_size_ <= old_ops_size);
        for (int nr_back_to_free_dslab_ = ops_pool_size_ - new_ops_size; nr_back_to_free_dslab_ > 0; nr_back_to_free_dslab_--)
        {
            auto channel = next_channel(next_ops_pool_remove_channel_);
            next_ops_pool_remove_channel_ = (next_ops_pool_remove_channel_ + 1) % ssd_->nr_channels_;
            if (!list_empty(&ops_pool_[channel].list_))
            {
                auto dslab = list_first_entry(&ops_pool_[channel].list_, Slab, list);
                list_del(&dslab->list);
                ops_pool_[channel].size_--;
                ops_pool_size_--;
                assert(check_ops_pool());
                dslab->reset();
                list_add(&dslab->list, &free_list[channel].list_);
                free_list[channel].size_++;
            }
        }
        // Reclaim dslabs back to dslab_free_
        {
            std::unique_lock<std::mutex> dslab_free_lock(dslab_free_mutex_);
            for (int channel = 0; channel < ssd_->nr_channels_; channel++)
            {
                while (!list_empty(&free_list[channel].list_))
                {
                    auto dslab = list_first_entry(&free_list[channel].list_, Slab, list);
                    list_del(&dslab->list);
                    dslab->reset();
                    list_add(&dslab->list, &dslab_free_[channel].list_);
                    dslab_free_[channel].size_++;
                    nr_free_dslab_++;
                    assert(nr_free_dslab_ >= 0 && nr_free_dslab_ <= ssd_->nr_blocks_);
                }
            }
        }
        assert(check_ops_pool());
        return true;
    }

    bool KVCache::check_ops_pool()
    {
        auto size = 0;
        for (const auto &list : ops_pool_)
        {
            int i = 0;
            Slab *dslab = nullptr;
            list_for_each_with_entry(Slab, dslab, &list.list_, list)
            {
                i++;
            }
            if (i != list.size_)
            {
                assert(false);
                return false;
            }
            size += list.size_;
        }
        if (size != ops_pool_size_)
        {
            assert(false);
            return false;
        }
        return true;
    }
    void KVCache::gc_dslabs(std::vector<Slab *> &to_drop, std::string *read_buffer, std::vector<List> &free_list)
    {
        // Get an ops slab
        Slab *ops_slab = nullptr;
        auto begin = next_ops_pool_remove_channel_;
        do
        {
            auto channel = next_ops_pool_remove_channel_;
            next_ops_pool_remove_channel_ = next_channel(next_ops_pool_remove_channel_);
            if (!list_empty(&ops_pool_[channel].list_))
            {
                ops_slab = list_first_entry(&ops_pool_[channel].list_, Slab, list);
                list_del(&ops_slab->list);
                ops_pool_[channel].size_--;
                ops_pool_size_--;
                assert(check_ops_pool());
                break;
            }
        } while (begin != next_ops_pool_remove_channel_);
        assert(ops_slab);

        // Modify dslab_full_
        {
            std::unique_lock<std::mutex> slab_full_lock(dslab_full_mutex_);

            ops_slab->reset();
            list_add(&ops_slab->list, &dslab_full_[ssd_->channel_id(ops_slab->block_id)].list_);
            dslab_full_[ssd_->channel_id(ops_slab->block_id)].size_++;

            for (auto dslab : to_drop)
            {
                list_del(&dslab->list);
                dslab_full_[ssd_->channel_id(dslab->block_id)].size_--;
                nr_full_dslab_--;
                auto channel = ssd_->channel_id(dslab->block_id);
                list_add(&dslab->list, &free_list[channel].list_);
                free_list[channel].size_++;
            }
        }

        // Move valid objects to gc_buffer_ and modify index entries
        assert(ops_slab);
        gc_buffer_->reset();
        gc_buffer_->slot_size = to_drop[0]->slot_size;
        gc_buffer_->nr_slots = to_drop[0]->nr_slots;
        gc_buffer_->cid = to_drop[0]->cid;
        auto size = 0;
        for (auto dslab : to_drop)
        {
            size += dslab->nr_used.load();
            assert(size <= gc_buffer_->nr_slots);
            assert(dslab->cid != -1);
            assert(dslab->cid == gc_buffer_->cid);
            auto fake_slab = read_dslab(dslab, read_buffer);
            std::unique_lock<std::shared_mutex> reader_lock(index_mutex_);
            fake_slab->for_each_slot([this, dslab](Slot *slot, int slot_id)
                                     {
                auto index_entry = get_index_entry(slot->key());
                if (index_entry) {
                    if (index_entry->slab_id == dslab->sid && index_entry->slot_id == slot_id) {
                        auto nr_alloc = gc_buffer_->nr_alloc.load();
                        auto new_slot = gc_buffer_->slot_alloc();
                        assert(new_slot);
                        new_slot->Write(slot->key(), slot->value());
                        index_entry->slab_id = gc_buffer_->sid;
                        index_entry->slot_id = nr_alloc;
                    }   
                }
                return true; });
            dslab->nr_used.store(0);
        }
        // Move valid objects to disk and modify index entries
        assert(gc_buffer_->cid != -1);
        flush_mslab_to_dslab(gc_buffer_, ops_slab);
        {
            std::unique_lock<std::shared_mutex> reader_lock(index_mutex_);
            modify_index_to(gc_buffer_, ops_slab);
        }
    }

    void KVCache::modify_index_to(Slab *src, Slab *dst)
    {
        auto nr_used = 0;
        src->for_each_slot([this, src, dst, &nr_used](Slot *slot, int slot_id)
                           {
                auto index_entry = get_index_entry(slot->key());
                if (!index_entry)
                {
                    return true;
                }
                if (index_entry->slab_id == src->sid && index_entry->slot_id == slot_id)
                {
                    index_entry->slab_id = dst->sid;
                    nr_used++;
                }
                return true; });
        src->nr_used.store(0);
        dst->nr_used.store(nr_used);
    }

    void KVCache::Delete(std::string_view key)
    {
        std::unique_lock<std::shared_mutex> reader_lock(index_mutex_);
        auto index_entry = get_index_entry(key);
        if (!index_entry)
        {
            return;
        }
        list_del(&index_entry->list);
        index_entry_free(index_entry);
        auto slab = slab_by_sid(index_entry->slab_id);
        assert(slab->nr_used.fetch_sub(1) > 0);
        if (slab->nr_used.load() <= 0)
        {
            // TODO: reclaim dslab
        }
    }
    KVCache::~KVCache()
    {
        shutdown_ = true;
        gc_signal_.notify_all();
        flush_signal_.notify_all();
        gc_finished_signal_.notify_all();
        flush_finished_signal_.notify_all();
        if (slab_flush_thread_.joinable())
        {
            slab_flush_thread_.join();
        }
        if (slab_gc_thread_.joinable())
        {
            slab_gc_thread_.join();
        }
    }

    // Calculate the default slab class size [min_kv_size, slab_size - Slab::header_size()]
    void KVCache::default_slab_class_size(int slab_size, int min_kv_size, int slab_class_size[], int max_nr_slab_class, int *nr_slab_class_out)
    {
        int max_slot_size = slab_size - Slab::header_size();
        int min_slot_size = min_kv_size + Slot::header_size();
        double factor = 2;
        int curr_size = min_slot_size;
        int i = 0;
        for (i = 0; i < max_nr_slab_class; i++)
        {
            slab_class_size[i] = curr_size;
            curr_size = std::min(static_cast<int>(curr_size * factor), max_slot_size);
            if (i > 1 && slab_class_size[i - 1] >= max_slot_size)
            {
                break;
            }
        }
        *nr_slab_class_out = i;
    }
}