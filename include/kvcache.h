#ifndef KVCACHE_H
#define KVCACHE_H

#include "status.h"
#include "ssd.h"
#include "list.h"
#include "options.h"
#include <thread>
#include <mutex>
#include <condition_variable>

namespace KVCache
{
static constexpr int KB = 1024;
static constexpr int MB = 1024 * 1024;
static constexpr int GB = 1024 * MB;

    static constexpr int kDigestLength = 20;

    struct SlabInfo {
        int sid;
        int cid; // slab class id
        int nr_alloc;
        int nr_slots;
        int slot_used;
        bool in_memory;
        // memory: index of the slab in mslab(memory slab) mmap area.
        // disk: (BlockID << 5) || ChannelID
        int addr;

        struct list_head list;
    };

    struct SlabClass {
        int cid;
        int slot_size;
        int nr_mslab;
        int nr_dslab;
        struct list_head slab_partial;
    };

    // struct Slab represent a dslab(disk slab).
    // 
    // Disk layout:
    // [struct Slab][entry_1][entry_2]...[entry_n]
    //
    // Entry is [key][value] 
    struct Slab {
        int sid;
        int cid;
        int nr_alloc;
        int slot_size;
        int nr_slots;
        int nr_used; // number of slots actually used in the slab  
        struct list_head list;
        int block_id; // only for dslab
        char data[0];
        static int slab_header_size() {  return offsetof(Slab, data); }
        Slot* slot_alloc() {
            assert(!is_full());
            auto slot = reinterpret_cast<Slot*>(data + slot_size * nr_alloc);
            nr_alloc++;
            nr_used++;
            return slot;
        }
        inline bool is_full() {
            return nr_alloc == nr_slots;
        }
    };

    struct Slot {
        struct Header {
            int key_len;
            int value_len;
        };
        Header header;
        char data[0];
        void Write(const std::string &key, const std::string &value) {
            header.key_len = key.size();
            header.value_len = value.size();
            memcpy(data, key.data(), key.size());
            memcpy(data + key.size(), value.data(), value.size());
        }
    };

    // sha1 digest
    using Digest = std::array<unsigned char, kDigestLength>;
    struct IndexEntry {
        Digest digest;
        int slab_id;
        int slot_id;
        struct list_head list;
    };
    
    class KVCache
    {
    public:
        KVCache(SSD *ssd, const Options &options);

        Status Get(const std::string &key, std::string *value);
        Status Put(const std::string &key, const std::string &value);
        Status Delete(const std::string &key);

    private:
        std::unique_ptr<SSD> ssd_;
        Options options_;
        int slab_size_; 
        int nr_mslab_;
        int nr_dslab_;

        std::hash<std::string> hasher_;

        struct list_head mslab_free_;
        struct list_head mslab_full_;
        std::vector<struct list_head> dslab_free_;
        std::vector<struct list_head> dslab_full_;

        Slab* *slab_info_table_;
        SlabClass* slab_class_table_;
        int nr_slab_class_;

        // mmap area
        // [mslab_base, mslab_end) actually store mslab
        Slab *mslab_base_;
        Slab *mslab_end_;
        // [dslab_base, dslab_end) only store dslab header(struct Slab)
        Slab *dslab_base_;
        Slab *dslab_end_;
        IndexEntry *index_area_base_;
        IndexEntry *index_area_end_;

        // hash table for index
        struct list_head *index_table_;
        int index_table_size_;
        struct list_head free_index_entry_;

        std::mutex writer_mutex;

    private:
        void slab_init();
        void index_init();

        IndexEntry* index_entry_alloc(const std::string &key);
        void index_entry_free(IndexEntry *entry);
        void put_index_entry(const std::string &key, IndexEntry *entry);
        IndexEntry* get_index_entry(const std::string &key);
        void delete_index_entry(const std::string &key);

        
        Status slot_alloc(size_t size, Slot **slot);
        size_t slot_size(const std::string &key, const std::string &value);
        SlabClass* get_slab_class(size_t size);

        Digest make_digest(const std::string &key);
        Slab* slot_to_slab(Slot *slot);
        int slot_id(Slot *slot);
        Slab* slab(int sid);
        bool in_memory(int sid);
    };
} // namespace KVCache
#endif