#include "kvcache.h"
#include <sys/mman.h>
#include <algorithm>
#include <openssl/sha.h>
namespace KVCache
{
    // align the pointer to the alignment(upper alignment)
    void *align_ptr(void *ptr, size_t alignment) {
        return (void *)(((uintptr_t)ptr + alignment - 1) & ~(alignment - 1));
    }

    // TODO: use mmap to allocate memory precisely
    KVCache::KVCache(SSD *ssd, const Options &options)
        : ssd_(ssd), options_(options)
    {
        slab_size_ = ssd_->block_size_;
        nr_dslab_ = ssd_->nr_blocks_;
        nr_mslab_ = options.slab_mem_budget / slab_size_;
        auto mslab_area_size = nr_mslab_ * slab_size_;
        auto dslab_area_size = nr_dslab_ * sizeof(Slab);
        auto alignment = slab_size_;
        mslab_base_ = (Slab *)mmap(nullptr, mslab_area_size + dslab_area_size+alignment, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE| MAP_ANONYMOUS, -1, 0);
        if (mslab_base_ == MAP_FAILED)
        {
            throw std::runtime_error("Failed to mmap memory slab");
        }
        mslab_base_ = static_cast<Slab *>(align_ptr(mslab_base_, alignment));
        mslab_end_ = reinterpret_cast<Slab *>(reinterpret_cast<char *>(mslab_base_) + mslab_area_size);
        dslab_base_ = mslab_end_;
        dslab_end_ = dslab_base_ + nr_dslab_;

        INIT_LIST_HEAD(&mslab_free_);
        INIT_LIST_HEAD(&mslab_full_);
        dslab_free_.resize(ssd_->nr_channels_);
        dslab_full_.resize(ssd_->nr_channels_);
        for (size_t i = 0; i < ssd_->nr_channels_; i++) {
            INIT_LIST_HEAD(&dslab_free_[i]);
            INIT_LIST_HEAD(&dslab_full_[i]);
        }
        
        slab_info_table_ = new Slab*[nr_mslab_ + nr_dslab_];
        for (size_t i = 0; i < nr_mslab_ + nr_dslab_; i++)
        {
            auto slab_info = slab(i);
            slab_info->sid = i;
            slab_info->cid = -1;
            slab_info->nr_alloc = 0;
            slab_info->nr_slots = -1;
            slab_info->block_id = -1;
            INIT_LIST_HEAD(&slab_info->list);
            if (i < nr_mslab_) {
                list_add(&slab_info->list, &mslab_free_);
            }
        }

        int slab_id = nr_mslab_;
        ssd_->iterate_all_blocks([this, &slab_id](SSD::Block &block) {
            auto slab_info = slab(slab_id);
            slab_info->block_id = block.block_id;
            assert(block.channel_id < ssd_->nr_channels_);
            list_add(&slab_info->list, &dslab_free_[block.channel_id]);  
            slab_id++;
        });

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
        auto nr_index_entry_ = std::max(options_.index_mem_budget / sizeof(IndexEntry), static_cast<size_t>(options_.slab_mem_budget / options_.slab_class_size[0]));
        index_area_base_ = (IndexEntry *)mmap(nullptr, nr_index_entry_ * sizeof(IndexEntry), PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE, -1, 0);
        if (index_area_base_ == MAP_FAILED)
        {
            throw std::runtime_error("Failed to allocate index area");
        }
        index_area_end_ = index_area_base_ + nr_index_entry_ * sizeof(IndexEntry);
        INIT_LIST_HEAD(&free_index_entry_);
        for (size_t i = 0; i < nr_index_entry_; i++)
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

    Status KVCache::Get(const std::string &key, std::string *value)
    {
        return Status::OK();
    }

    Status KVCache::Put(const std::string &key, const std::string &value)
    {
        {
            auto lock = std::unique_lock(writer_mutex);

            Slot *slot;
            auto status = slot_alloc(slot_size(key, value), &slot);
            if (!status.ok())
            {
                return status;
            }
            auto slab = slot_to_slab(slot);
            slot->Write(key, value);

            auto index_entry = get_index_entry(key);
            if (index_entry)
            {
                auto old_slab_id = index_entry->slab_id;
                assert(old_slab_id != -1);
                auto old_slabinfo = slab_info_table_[old_slab_id];
                if (old_slab_id != slab->sid)
                {
                    old_slabinfo->nr_used--;
                }
            }
            else
            {
                index_entry = index_entry_alloc(key);
                index_entry->slab_id = slab->sid;
                index_entry->slot_id = slot_id(slot);
                put_index_entry(key, index_entry);
            }
        }
        return Status::OK();
    }
    void KVCache::put_index_entry(const std::string &key, IndexEntry *entry) {
        auto index_table_index = hasher_(key) % index_table_size_;
        list_add(&index_table_[index_table_index], &entry->list);
    }

    IndexEntry *KVCache::get_index_entry(const std::string &key) {
        auto index_table_index = hasher_(key) % index_table_size_;
        IndexEntry *entry;
        Digest digest;
        digest = make_digest(key);
        list_for_each_with_entry(IndexEntry, entry, &index_table_[index_table_index], list) {
            if (entry->digest == digest) {
                return entry;
            }
        }
        return nullptr;
    }   

    Digest KVCache::make_digest(const std::string &key) {
        Digest digest;
        ::SHA_CTX sha1;
        ::SHA1_Init(&sha1);
        ::SHA1_Update(&sha1, key.c_str(), key.length());
        ::SHA1_Final(digest.data(), &sha1);
        return digest;
    }

    inline size_t KVCache::slot_size(const std::string &key, const std::string &value)
    {
        return key.size() + value.size();
    }

    IndexEntry *KVCache::index_entry_alloc(const std::string &key)
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

    inline Slab* KVCache::slot_to_slab(Slot *slot) {
        auto slot_addr = reinterpret_cast<uintptr_t>(slot);
        auto diff = slot_addr - reinterpret_cast<uintptr_t>(mslab_base_);
        return reinterpret_cast<Slab*>(mslab_base_ )+ (diff / slab_size_);
    }
    inline int KVCache::slot_id(Slot *slot) {
        auto slab = slot_to_slab(slot);
        auto diff = reinterpret_cast<uintptr_t>(slot) - reinterpret_cast<uintptr_t>(slab->data);
        return diff / slab->slot_size;
    }

    Status KVCache::slot_alloc(size_t size,  Slot **slotp) {
        auto slab_class = get_slab_class(size);
        if (slab_class == nullptr) {
            return Status::NotFound("No slab class found");
        }
        if (!list_empty(&slab_class->slab_partial)) {
            auto slab = list_first_entry(&slab_class->slab_partial, Slab, list);
            auto slot = slab->slot_alloc();
            *slotp = slot;
            if (slab->is_full()) {
                list_del(&slab->list);
                list_add(&slab->list, &mslab_full_);
            }
            return Status::OK();
        }
        return Status::NotFound("No slab class found");
    }

    inline Slab* KVCache::slab(int sid) {
        assert(sid < nr_mslab_ + nr_dslab_);
        if (sid < nr_mslab_) {
            return reinterpret_cast<Slab*>(reinterpret_cast<char*>(mslab_base_) + sid * slab_size_);
        }
        return dslab_base_ + (sid - nr_mslab_);
    }

    SlabClass* KVCache::get_slab_class(size_t size) {
        auto it = std::lower_bound(slab_class_table_, slab_class_table_ + nr_slab_class_, size, 
            [](const SlabClass& sc, size_t target) { return sc.slot_size < target; });
        if (it != (slab_class_table_ + nr_slab_class_) && it->slot_size >= size) {
            return it;
        }
        return nullptr;
    }
}
