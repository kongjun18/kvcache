#include "kvcache.h"
#include <sys/mman.h>
#include <algorithm>
#include <openssl/sha.h>
#include <queue>
namespace KVCache
{
    // align the pointer to the alignment(upper alignment)
    void *align_ptr(void *ptr, size_t alignment)
    {
        return (void *)(((uintptr_t)ptr + alignment - 1) & ~(alignment - 1));
    }

    // TODO: use mmap to allocate memory precisely
    KVCache::KVCache(SSD *ssd, const Options &options)
        : ssd_(ssd), options_(options)
    {
        free_block_water_mark_low_ = options_.free_block_water_mark_low * ssd_->nr_blocks_;
        free_block_water_mark_high_ = options_.free_block_water_mark_high * ssd_->nr_blocks_;
        free_block_water_mark_low_min_ = options_.free_block_slab_water_mark_low_min * ssd_->nr_blocks_;
        free_block_water_mark_high_max_ = options_.free_block_slab_water_mark_high_max * ssd_->nr_blocks_;

        // initialize slab
        slab_size_ = ssd_->block_size_;
        nr_dslab_ = ssd_->nr_blocks_;
        nr_mslab_ = options.slab_mem_budget / slab_size_;
        auto mslab_area_size = nr_mslab_ * slab_size_;
        auto dslab_area_size = nr_dslab_ * sizeof(Slab);
        auto alignment = slab_size_;
        mslab_base_ = (Slab *)mmap(nullptr, mslab_area_size + dslab_area_size + alignment, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mslab_base_ == MAP_FAILED)
        {
            throw std::runtime_error("Failed to mmap memory slab");
        }
        mslab_base_ = static_cast<Slab *>(align_ptr(mslab_base_, alignment));
        mslab_end_ = reinterpret_cast<Slab *>(reinterpret_cast<char *>(mslab_base_) + mslab_area_size);
        dslab_base_ = mslab_end_;
        dslab_end_ = dslab_base_ + nr_dslab_;

        dslab_free_.resize(ssd_->nr_channels_);
        dslab_full_.resize(ssd_->nr_channels_);
        ops_pool_.resize(ssd_->nr_channels_);

        for (size_t i = 0; i < nr_mslab_ + nr_dslab_; i++)
        {
            auto slab_info = slab_by_sid(i);
            slab_info->sid = i;
            slab_info->cid = -1;
            slab_info->nr_alloc = 0;
            slab_info->nr_slots = -1;
            slab_info->block_id = -1;
            INIT_LIST_HEAD(&slab_info->list);
            if (i < nr_mslab_)
            {
                list_add(&slab_info->list, &mslab_free_.list_);
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
            } else {
                list_add(&slab_info->list, &dslab_free_[block.channel_id].list_);
                dslab_free_[block.channel_id].size_++;
                nr_free_dslab_++;
            }
            slab_id = slab_id + 1; });
        max_ops_pool_size_ = ops_pool_size_;

        // initialize slab class
        nr_slab_class_ = options_.nr_slab_class;
        slab_class_table_ = new SlabClass[nr_slab_class_];
        for (size_t i = 0; i < nr_slab_class_; i++)
        {
            slab_class_table_[i].cid = i;
            slab_class_table_[i].slot_size = options_.slab_class_size[i];
            slab_class_table_[i].nr_mslab = 0;
            slab_class_table_[i].nr_dslab = 0;
            INIT_LIST_HEAD(&slab_class_table_[i].slab_partial);
        }
        auto nr_index_entry = std::max(options_.index_mem_budget / sizeof(IndexEntry), static_cast<size_t>(options_.slab_mem_budget / options_.slab_class_size[0]));
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
        index_table_size_ = options_.index_table_size;
        index_table_ = new struct list_head[index_table_size_];
        for (size_t i = 0; i < index_table_size_; i++)
        {
            INIT_LIST_HEAD(&index_table_[i]);
        }
    }

    Status KVCache::Get(std::string_view key, std::string *value)
    {
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

            // Update index
            auto slab = slot_to_slab(slot);
            IndexEntry *index_entry = nullptr;
            {
                std::shared_lock<std::shared_mutex> reader_lock(reader_mutex_);
                index_entry = get_index_entry(key);
                if (index_entry)
                {
                    auto old_slab_id = index_entry->slab_id;
                    assert(old_slab_id != -1);
                    auto old_slabinfo = slab_by_sid(old_slab_id);
                    if (old_slab_id != slab->sid)
                    {
                        // This line needs to be protected by reader_mutex_, otherwise
                        // Del may decrement the nr_used of the old slab while Put is
                        // inserting the new slab, causing repeated decrement.
                        old_slabinfo->nr_used.fetch_sub(1);
                    }
                }
            }
            if (!index_entry)
            {
                std::unique_lock<std::shared_mutex> reader_lock(reader_mutex_);
                index_entry = index_entry_alloc(key);
                index_entry->slab_id = slab->sid;
                index_entry->slot_id = slot_id(slot);
                {
                    put_index_entry(key, index_entry);
                }
            }

            // Once the slab is inserted into mslab_full_, flush thread may flush it to dslab
            // and update the index_entry pointing to the new dslab. So we must insert the slab
            // into mslab_full after the index_entry is inserted into index_table_, otherwise
            // updates of flush thread will be lost.
            if (slab->is_full())
            {
                std::unique_lock<std::mutex> mslab_full_lock(mslab_full_.mutex_);
                list_add(&slab->list, &mslab_full_.list_);
                mslab_full_.size_++;
            }
            if (options_.enable_background_flush)
            {
                slab_flush();
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
        Digest digest;
        ::SHA_CTX sha1;
        ::SHA1_Init(&sha1);
        ::SHA1_Update(&sha1, key.data(), key.length());
        ::SHA1_Final(digest.data(), &sha1);
        return digest;
    }

    inline size_t KVCache::slot_size(std::string_view key, std::string_view value)
    {
        return key.size() + value.size();
    }

    IndexEntry *KVCache::index_entry_alloc(std::string_view key)
    {
        // KVCache::KVCache() make sure index entry mmap area is big enough.
        // TODO: consider the case that index entry mmap area is not big enough.
        assert(!list_empty(&free_index_entry_));
        auto index_entry = list_first_entry(&free_index_entry_, IndexEntry, list);
        list_del(&index_entry->list);
        auto digest = make_digest(key);
        index_entry->digest = digest;
        return index_entry;
    }

    inline Slab *KVCache::slot_to_slab(const Slot *slot)
    {
        auto slot_addr = reinterpret_cast<uintptr_t>(slot);
        auto diff = slot_addr - reinterpret_cast<uintptr_t>(mslab_base_);
        return reinterpret_cast<Slab *>(mslab_base_) + (diff / slab_size_);
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
        mslab_free_.mutex_.lock();
        if (!list_empty(&mslab_free_.list_))
        {
            auto slab = list_first_entry(&mslab_free_.list_, Slab, list);
            list_del(&slab->list);
            mslab_free_.size_--;
            mslab_free_.mutex_.unlock();

            slab->cid = slab_class->cid;
            slab->slot_size = slab_class->slot_size;
            slab->nr_slots = slab_size_ / slab_class->slot_size;
            list_add(&slab->list, &slab_class->slab_partial);
            // retry
            return slot_alloc(size, slotp);
        }
        mslab_free_.mutex_.unlock();

        // assert(list_empty(&mslab_free_.list_));
        // assert(!list_empty(&mslab_full_.list_));
        slab_flush();
        if (options_.enable_background_flush)
        {
            std::unique_lock<std::mutex> mslab_free_lock(mslab_free_.mutex_);
            flush_finished_signal_.wait(mslab_free_lock, [this]()
                                        { return !list_empty(&mslab_free_.list_); });
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
        if (it != (slab_class_table_ + nr_slab_class_) && it->slot_size >= size)
        {
            *slab_class = it;
            return Status::OK();
        }
        return Status::NotFound("No slab class found");
    }

    void KVCache::slab_flush()
    {
        if (options_.enable_background_flush)
        {
            if (!slab_flush_thread_started_.test_and_set())
            {
                slab_flush_thread_ = std::thread([this]()
                                                 { while(1) {do_slab_flush();} });
                slab_flush_thread_.detach();
            }
            flush_signal_.notify_one();
        }
        else
        {
            do_slab_flush();
        }
    }

    void KVCache::tune_ops_pool_size()
    {
        std::unique_lock<std::mutex> gc_lock(gc_mutex_);
        if (ops_pool_size_ > free_block_water_mark_high_)
        {
            free_block_water_mark_high_ = std::max(free_block_water_mark_high_ - 1, free_block_water_mark_low_ + 10);
            free_block_water_mark_low_ = std::max(free_block_water_mark_low_ - 1, free_block_water_mark_low_min_);

            Slab *ops_slab = nullptr;
            auto channel = next_channel(next_ops_pool_remove_channel_);
            for (auto beginning = next_ops_pool_remove_channel_; beginning != channel;
                 channel = next_channel(channel))
            {
                std::unique_lock<std::mutex> ops_pool_lock(ops_pool_[channel].mutex_);
                if (!list_empty(&ops_pool_[channel].list_))
                {
                    ops_slab = list_first_entry(&ops_pool_[channel].list_, Slab, list);
                    list_del(&ops_slab->list);
                    ops_pool_[channel].size_--;
                    ops_pool_size_--;
                    break;
                }
            }
            {
                std::unique_lock<std::mutex> dslab_free_lock(dslab_free_mutex_);
                list_add(&ops_slab->list, &dslab_free_[channel].list_);
                dslab_free_[channel].size_++;
                nr_free_dslab_++;
            }
        }
        if (ops_pool_size_ < free_block_water_mark_low_)
        {
            free_block_water_mark_high_ = std::min(2 * free_block_water_mark_high_, free_block_water_mark_high_max_);
            free_block_water_mark_low_ = std::min(2 * free_block_water_mark_low_, max_ops_pool_size_);

            // slab_gc must holds gc_lock
            gc_lock.unlock();
            slab_gc();
        }
    }
    void KVCache::do_slab_flush()
    {
        Slab *dslab = nullptr;
        Slab *mslab = nullptr;
        int dslab_channel = -1;

        // search for free dslab and full mslab
        {
            std::unique_lock<std::mutex> mslab_full_lock(mslab_full_.mutex_);
            flush_signal_.wait(mslab_full_lock, [this]()
                               { return !list_empty(&mslab_full_.list_); });
            mslab = list_first_entry(&mslab_full_.list_, Slab, list);
            list_del(&mslab->list);
            mslab_full_.size_--;
        }

        {
            std::unique_lock<std::mutex> dslab_free_lock(dslab_free_mutex_);
            for (auto beginning = next_mslab_flush_channel_, channel = next_channel(beginning); beginning != channel;
                 channel = next_channel(channel))
            {
                if (!list_empty(&dslab_free_[channel].list_))
                {
                    dslab = list_first_entry(&dslab_free_[channel].list_, Slab, list);
                    list_del(&dslab->list);
                    dslab_free_[channel].size_--;
                    nr_free_dslab_--;
                    dslab_channel = channel;
                    break;
                }
            }
        }
        // We can definitely find a full mslab, but we may not be able to find a free dslab.
        // So we need to wait GC to reclaim some dslab and retry.
        assert(mslab);
        if (!dslab)
        {
            if (options_.enable_background_gc)
            {
                slab_gc();
                std::unique_lock<std::mutex> dslab_free_lock(dslab_free_mutex_);
                gc_finished_signal_.wait(dslab_free_lock, [this]()
                                         { return nr_free_dslab_ > 0; });
            }
            else
            {
                slab_gc();
            }
            // Retry
            do_slab_flush();
        }

        assert(dslab);
        assert(mslab);
        assert(dslab_channel > -1 && dslab_channel < ssd_->nr_channels_);
        flush_mslab_to_dslab(mslab, dslab);

        // Update index table
        {
            std::unique_lock<std::shared_mutex> reader_lock(reader_mutex_);
            mslab->for_each_slot([this, mslab, dslab](Slot *slot, int slot_id)
                                 {
                auto index_entry = get_index_entry(slot->key());
                if (!index_entry)
                {
                    return true;
                }
                if (index_entry->slab_id == mslab->sid && index_entry->slot_id == slot_id)
                {
                    index_entry->slab_id = dslab->sid;
                }
                return true; });
        }

        // mslab -> mslab_free
        // dslab -> dslab_full
        {
            mslab->reset();
            std::unique_lock<std::mutex> mslab_free_lock(mslab_free_.mutex_);
            list_add(&mslab->list, &mslab_free_.list_);
            mslab_free_.size_++;
        }
        {
            std::unique_lock<std::mutex> dslab_full_lock(dslab_full_mutex_);
            list_add(&dslab->list, &dslab_full_[dslab_channel].list_);
            dslab_full_[dslab_channel].size_++;
        }

        flush_finished_signal_.notify_one();
        gc_signal_.notify_one();
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
                                              { while(1) {do_slab_gc();} });
                slab_gc_thread_.detach();
            }
            gc_finished_signal_.notify_one();
        }
        else
        {
            do_slab_gc();
        }
    }

    // TODO: implement slab gc
    void KVCache::do_slab_gc()
    {
        dslab_free_mutex_.lock();
        auto nr_free_dslab = nr_free_dslab_;
        dslab_free_mutex_.unlock();

        if (nr_free_dslab < free_block_water_mark_low_)
        {
            quick_gc();
        }
        else
        {
            normal_gc();
        }

        gc_finished_signal_.notify_one();
    }

    void KVCache::flush_mslab_to_dslab(Slab *mslab, Slab *dslab)
    {
        auto block_data = std::string_view(reinterpret_cast<char *>(mslab->data), slab_size_);
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
        dslab->nr_alloc = mslab->nr_alloc;
        dslab->nr_used.store(mslab->nr_used.load());
    }

    // 1. Drop full dslab directly to respond high disk space pressure immediately.
    // 2. Increase ops_pool_size to speedup normal GC.
    // 3. Increase high/low water maker.
    void KVCache::quick_gc()
    {
        dslab_free_mutex_.lock();
        int nr_free_dslab = nr_free_dslab_;
        dslab_free_mutex_.unlock();
        assert(nr_free_dslab < free_block_water_mark_low_);

        // Low water mark means the system is under high disk space pressure.
        // In this case, we need to drop some full dslab to free up disk space immediately
        // and ops_pool_size to speedup normal GC.
        int nr_back_to_free_dslab = 2 * free_block_water_mark_low_ - nr_free_dslab;
        int nr_back_to_ops_pool = std::min(ops_pool_size_, max_ops_pool_size_ - ops_pool_size_);
        int nr_dslab_to_drop = nr_back_to_free_dslab + nr_back_to_ops_pool;

        // Increase low water mark to 1.5x and free dslab to 2x,
        // which give the system more chance to reclaim dslab using normal GC.
        free_block_water_mark_high_ = std::min(static_cast<int>(1.5 * free_block_water_mark_high_), free_block_water_mark_high_max_);
        free_block_water_mark_low_ = std::min(static_cast<int>(1.5 * free_block_water_mark_low_), static_cast<int>(0.9 * free_block_water_mark_high_));

        // Select dslabs to drop
        struct list_head to_drop;
        INIT_LIST_HEAD(&to_drop);
        {
            std::unique_lock<std::mutex> dslab_full_lock(dslab_full_mutex_);
            while (nr_dslab_to_drop > 0)
            {
                auto channel = next_channel(next_gc_channel_++) % ssd_->nr_channels_;
                if (!list_empty(&dslab_full_[channel].list_))
                {
                    auto dslab = list_first_entry(&dslab_full_[channel].list_, Slab, list);
                    list_del(&dslab->list);
                    dslab_full_[channel].size_--;
                    list_add(&dslab->list, &to_drop);
                    nr_dslab_to_drop--;
                }
            }
        }

        // Reclaim dslabs back to ops_pool_ or dslab_free_
        int i = 0;
        Slab *dslab = nullptr;
        Slab *next = nullptr;
        list_for_each_with_entry(Slab, dslab, &to_drop, list)
        {
            auto slab = read_dslab(dslab);
            std::unique_lock<std::shared_mutex> reader_lock(reader_mutex_);
            evict_dslab(slab);
        }
        list_for_each_safe_with_entry(Slab, dslab, &to_drop, list, next)
        {
            dslab->reset();

            auto channel = ssd_->channel_id(dslab->block_id);
            if (i++ >= nr_back_to_ops_pool)
            {
                break;
            }
            list_add(&dslab->list, &ops_pool_[channel].list_);
            ops_pool_[channel].size_++;
        }
        {
            std::unique_lock<std::mutex> dslab_free_lock(dslab_free_mutex_);
            list_for_each_safe_with_entry(Slab, dslab, &to_drop, list, next)
            {
                dslab->reset();

                auto channel = ssd_->channel_id(dslab->block_id);
                list_add(&dslab->list, &dslab_free_[channel].list_);
                dslab_free_[channel].size_++;
                nr_free_dslab_++;
            }
        }
    }

    void KVCache::evict_dslab(const Slab *dslab)
    {
        auto mslab = read_dslab(dslab);
        mslab->for_each_slot([this, dslab](Slot *slot, int slot_id)
                             {
                                del_index_entry(slot->key());
                                return true; });
    }

    // reader_mutex_ must be exclusively held before calling this function
    void KVCache::del_index_entry(std::string_view key)
    {
        auto index_entry = get_index_entry(key);
        if (index_entry)
        {
            index_entry_free(index_entry);
        }
    }

    // reader_mutex_ must be exclusively held before calling this function
    void KVCache::index_entry_free(IndexEntry *entry)
    {
        list_add(&entry->list, &free_index_entry_);
    }

    // read_dslab return a fake mslab that can be used to read slots of dslab
    // read_dslab is forbidden to be called concurrently.
    Slab *KVCache::read_dslab(const Slab *dslab)
    {
        static std::string read_buffer(slab_size_, '\0');
        auto status = ssd_->read_block(dslab->block_id, &read_buffer);
        if (!status.ok())
        {
            throw std::runtime_error("Failed to read block");
        }
        auto slab = reinterpret_cast<Slab *>(read_buffer.data());
        // Copy fields manually since assignment operator is deleted
        slab->sid = dslab->sid;
        slab->cid = dslab->cid;
        slab->nr_alloc = dslab->nr_alloc;
        slab->slot_size = dslab->slot_size;
        slab->nr_slots = dslab->nr_slots;
        slab->block_id = dslab->block_id;
        slab->nr_used = -1;
        INIT_LIST_HEAD(&slab->list);
        return slab;
    }

    // GC full dslabs with largest number of used slots until ops_pool_ is full
    void KVCache::normal_gc()
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
        while (!pq.empty())
        {
            auto dslab = pq.top();
            pq.pop();
        }
            // auto mslab = read_dslab(dslab);
            // mslab->for_each_slot([this, dslab](Slot *slot, int slot_id)
            // {
            //     auto index_entry = get_index_entry(slot->key());
            //     if (!index_entry)
            //     {
            //         return true;
            //     }
            //     // Old version or collisioned key
            //     if (!(index_entry->slab_id == dslab->sid && index_entry->slot_id == slot_id))
            //     {
            //         return true;
            //     }

            //     return true;
            // })
    }
}
