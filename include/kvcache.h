#ifndef KVCACHE_H
#define KVCACHE_H

#include "status.h"
#include "ssd.h"
#include "list.h"
#include "options.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include "thread_safety.h"

namespace KVCache
{
    static constexpr int KB = 1024;
    static constexpr int MB = 1024 * 1024;
    static constexpr int GB = 1024 * MB;

    static constexpr int kDigestLength = 20;

    struct SlabInfo
    {
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

    struct SlabClass
    {
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
    struct Slab
    {
        int sid;
        int cid;
        int nr_alloc;
        int slot_size;
        int nr_slots;
        int nr_used; // number of slots actually used in the slab
        struct list_head list;
        int block_id; // only for dslab
        char data[0];
        static int slab_header_size() { return offsetof(Slab, data); }
        Slot *slot_alloc()
        {
            assert(!is_full());
            auto slot = reinterpret_cast<Slot *>(data + slot_size * nr_alloc);
            nr_alloc++;
            nr_used++;
            return slot;
        }
        inline bool is_full()
        {
            return nr_alloc == nr_slots;
        }
    };

    struct Slot
    {
        struct Header
        {
            int key_len;
            int value_len;
        };
        Header header;
        char data[0];
        void Write(const std::string &key, const std::string &value)
        {
            header.key_len = key.size();
            header.value_len = value.size();
            memcpy(data, key.data(), key.size());
            memcpy(data + key.size(), value.data(), value.size());
        }
    };

    // sha1 digest
    using Digest = std::array<unsigned char, kDigestLength>;
    struct IndexEntry
    {
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

        List mslab_free_;
        List mslab_full_;
        std::vector<List> dslab_free_;
        std::vector<List> dslab_full_;
        std::vector<List> ops_pool_;

        SlabClass *slab_class_table_;
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

        std::mutex writer_mutex_;
        std::shared_mutex reader_mutex;

        std::thread slab_flush_thread_;
        std::thread slab_gc_thread_;
        std::atomic_flag slab_flush_thread_started_;
        std::atomic_flag slab_gc_thread_started_;
        std::condition_variable flush_signal_;
        std::condition_variable gc_signal_;
        std::condition_variable gc_finished_signal_;
        std::mutex gc_mutex_;
        int next_mslab_flush_channel_ = 0;
        int next_ops_pool_remove_channel_ = 0 ;
        int max_ops_pool_size_;
        int free_block_water_mark_low_min_;
        int free_block_water_mark_high_max_;
        int nr_free_dslab_ GUARDED_BY(gc_mutex_);
        int nr_full_dslab_ GUARDED_BY(gc_mutex_);
        int ops_pool_size_ GUARDED_BY(gc_mutex_);
        int free_block_water_mark_low_ GUARDED_BY(gc_mutex_);
        int free_block_water_mark_high_ GUARDED_BY(gc_mutex_);

    private:
        void slab_init();
        void index_init();

        IndexEntry *index_entry_alloc(const std::string &key);
        void index_entry_free(IndexEntry *entry);
        void put_index_entry(const std::string &key, IndexEntry *entry);
        IndexEntry *get_index_entry(const std::string &key);
        void delete_index_entry(const std::string &key);

        Status slot_alloc(size_t size, Slot **slot);
        size_t slot_size(const std::string &key, const std::string &value);
        Status get_slab_class(size_t size, SlabClass **slab_class);

        Digest make_digest(const std::string &key);
        Slab *slot_to_slab(Slot *slot);
        int slot_id(Slot *slot);
        Slab *slab_by_sid(int sid);
        bool in_memory(int sid);

        void slab_flush();
        void do_slab_flush();
        void slab_gc() EXCLUDES(gc_mutex_);
        void do_slab_gc() EXCLUDES(gc_mutex_);
        int next_channel(int current_channel);
        void flush_mslab_to_dslab(Slab *mslab, Slab *dslab);

        // Self-tuning feedback-based OPS management algorithm  
        // 1. If the number of ops blocks is less than free_block_water_mark_low_,
        //    move some ops blocks to dslab_free_.
        // 2. If the number of ops blocks is greater than free_block_water_mark_high_,
        //    start GC to reclaim some full dslab back to ops_pool_.
        void tune_ops_pool_size() EXCLUDES(gc_mutex_);
    };
} // namespace KVCache
#endif