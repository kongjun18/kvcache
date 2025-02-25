#ifndef KVCACHE_H
#define KVCACHE_H

#include "status.h"
#include "ssd.h"
#include "list.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <string_view>
#include <string>
#include <vector>
#include <atomic>
#include <cassert>

namespace KVCache
{
    static constexpr int kDigestLength = 20;
    static constexpr int kMaxNumSlabClass = 50;
    static constexpr int KB = 1024;
    static constexpr int MB = 1024 * 1024;
    static constexpr int GB = 1024 * MB;

    struct Slot
    {
        struct Header
        {
            int key_len;
            int value_len;
        };
        Header header;
        char data[0];
        static constexpr size_t header_size() { return offsetof(Slot, data); }
        void Write(std::string_view key, std::string_view value)
        {
            header.key_len = key.size();
            header.value_len = value.size();
            memcpy(data, key.data(), key.size());
            memcpy(data + key.size(), value.data(), value.size());
        }
        std::string_view key() const
        {
            return std::string_view(data, header.key_len);
        }
        std::string_view value() const
        {
            return std::string_view(data + header.key_len, header.value_len);
        }
    };

    struct SlabClass
    {
        int cid;
        int slot_size;
        int slots_per_slab;
        // slab_partial doesn't need to be protected by mutex,
        // because it's only accessed by the writer thread.
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
        // Only Get increases nr_alloc
        std::atomic_int nr_alloc;
        int slot_size;
        int nr_slots;
        // Number of slots actually used in the slab
        // Del decreases nr_used.
        // Put increases new value's nr_used and decreases old value's nr_used.
        std::atomic_int nr_used;
        struct list_head list;
        int block_id; // only for dslab
        char data[0];
        static constexpr size_t header_size() { return offsetof(Slab, data); }
        Slab &operator=(const Slab &other)
        {
            if (this != &other)
            {
                sid = other.sid;
                cid = other.cid;
                nr_alloc.store(other.nr_alloc.load());
                slot_size = other.slot_size;
                nr_slots = other.nr_slots;
                nr_used.store(other.nr_used.load());
                list = other.list;
                block_id = other.block_id;
            }
            return *this;
        }
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

        inline bool im_memory()
        {
            return block_id == -1;
        }

        template <typename T>
        void for_each_slot(T &&func)
        {
            for (int slot_id = 0; slot_id < nr_alloc; slot_id++)
            {
                auto slot = reinterpret_cast<Slot *>(data + slot_size * slot_id);
                if (!func(slot, slot_id))
                {
                    break;
                }
            }
        }
        Slot *slot(int i)
        {
            return reinterpret_cast<Slot *>(data + slot_size * i);
        }

        void reset()
        {
            cid = -1;
            nr_alloc = 0;
            slot_size = 0;
            nr_slots = 0;
            nr_used = 0;
            INIT_LIST_HEAD(&list);
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
        struct Options
        {
            // The maximum memory used for slabs
            int slab_mem_budget;
            // The maximum memory used for index
            int index_mem_budget;
            // The number of buckets in index hash table
            int index_table_size;
            // The ratio of OPS(Over-Provisioned Space) blocks to total blocks
            float ops_rate;
            // When the number of free blocks hits free_block_water_mark_low, reduce ops_pool size and put ops blocks into free blocks
            float free_block_water_mark_low;
            // When the number of free blocks hits free_block_water_mark_high, increase ops_pool size and put free blocks back into ops_pool
            float free_block_water_mark_high;
            // The minimum ratio of free blocks to total blocks
            float free_block_slab_water_mark_low_min;
            // The maximum ratio of free blocks to total blocks
            float free_block_slab_water_mark_high_max;
            // Whether to enable background flush.
            // enable_background_flush can be set by KVCACHE_ENABLE_BACKGROUND_FLUSH
            // environment variable, 1 means true and 0 means false.
            bool enable_background_flush;
            // Whether to enable background gc
            // enable_background_gc can be set by KVCACHE_ENABLE_BACKGROUND_GC
            // environment variable, 1 means true and 0 means false.
            bool enable_background_gc;
            // The size of each slab class
            int slab_class_size[kMaxNumSlabClass];
            // The number of slab classes
            int nr_slab_class;
        };

    public:
        static Options DefaultOptions(SSD *ssd);

        KVCache(SSD *ssd) : KVCache(ssd, DefaultOptions(ssd)) {}
        KVCache(SSD *ssd, const Options &options);
        ~KVCache();

        Status Get(std::string_view key, std::string *value);
        Status Get(std::string_view key, char buffer[], size_t buffer_len, size_t *value_len); // For C API
        Status Put(std::string_view key, std::string_view value);
        void Delete(std::string_view key);
        size_t MaxKVSize() const { return slab_class_table_[nr_slab_class_ - 1].slot_size - Slot::header_size(); }

    private:
        // GC priority: the slab with smaller valid objects has higher priority
        struct SlabGCPriorityComparator
        {
            bool operator()(const Slab *a, const Slab *b)
            {
                return a->nr_used.load() * a->slot_size < b->nr_used.load() * b->slot_size;
            }
        };

    private:
        SSD *ssd_;
        Options options_;
        int slab_size_ = 0;
        int nr_mslab_ = 0;
        int nr_dslab_ = 0;

        std::hash<std::string_view> hasher_;

        // Put remove slab from mslab_free_ and add to mslab_full_
        // Flush thread will remove slab from mslab_full_ and add to mslab_free_
        List mslab_free_; // GUARDED_BY(mslab_free_mutex_)
        List mslab_full_; // GUARDED_BY(mslab_full_mutex_)
        std::mutex mslab_free_mutex_;
        std::mutex mslab_full_mutex_;
        // Flush thread will remove slab from dslab_free_ and add to dslab_full_
        // GC thread will remove slab from dslab_full_ and add to dslab_free_
        std::vector<List> dslab_free_; // GUARDED_BY(dslab_free_mutex_)
        std::vector<List> dslab_full_; // GUARDED_BY(dslab_full_mutex_)
        std::mutex dslab_free_mutex_;
        std::mutex dslab_full_mutex_;
        std::vector<List> ops_pool_;
        int max_ops_pool_size_ = 0;
        int ops_pool_size_ = 0;
        int nr_free_dslab_ = 0; // GUARDED_BY(dslab_free_mutex_)
        int nr_full_dslab_ = 0; // GUARDED_BY(dslab_full_mutex_)

        SlabClass *slab_class_table_;
        int nr_slab_class_ = 0;

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
        struct list_head *index_table_; // GUARDED_BY(index_mutex_)
        int index_table_size_ = 0;
        struct list_head free_index_entry_; // GUARDED_BY(index_mutex_)
        std::atomic_int nr_free_index_entry_; // GUARDED_BY(index_mutex_)

        std::mutex writer_mutex_;
        // All writes on index and slab it points to should be protected by index_mutex_,
        // except for Slab::nr_used and Slab::nr_alloc.
        std::shared_mutex index_mutex_;

        std::condition_variable flush_signal_;
        std::condition_variable flush_finished_signal_;
        std::thread slab_flush_thread_;
        std::atomic_flag slab_flush_thread_started_;
        std::condition_variable gc_signal_;
        std::condition_variable_any gc_finished_signal_;
        std::thread slab_gc_thread_;
        std::atomic_flag slab_gc_thread_started_;
        std::mutex gc_mutex_;
        Slab *gc_buffer_;
        // Only accessed by flush thread
        int next_mslab_flush_channel_ = 0;
        // Only accessed by gc thread
        int next_ops_pool_remove_channel_ = 0;
        int free_block_water_mark_low_ = 0;
        int free_block_water_mark_high_ = 0;
        int free_block_water_mark_low_min_ = 0;
        int free_block_water_mark_high_max_ = 0;

        // Shutndown
        std::atomic_bool shutdown_;

    private:
        void slab_init();
        void index_init();
        void slab_class_init();
        static void default_slab_class_size(int slab_size, int min_value_size, int slab_class_size[], int max_nr_slab_class, int *nr_slab_class_out);

        Status get_slice(std::string_view key, std::string *read_buffer, std::string_view *value);

        IndexEntry *index_entry_alloc(std::string_view key);
        void index_entry_free(IndexEntry *entry);
        void put_index_entry(std::string_view key, IndexEntry *entry);
        IndexEntry *get_index_entry(std::string_view key);
        void del_index_entry(std::string_view key);

        Status slot_alloc(size_t size, Slot **slot);
        size_t slot_size(std::string_view key, std::string_view value);
        Status get_slab_class(size_t size, SlabClass **slab_class);

        Digest make_digest(std::string_view key);
        Slab *slot_to_slab(const Slot *slot);
        int slot_id(const Slot *slot);
        Slab *slab_by_sid(int sid);
        bool in_memory(int sid);

        void slab_flush();
        bool wait_slab_flush_until_shutdown();
        bool do_slab_flush();
        void slab_gc();
        bool wait_slab_gc_until_shutdown();
        bool do_slab_gc();
        int next_channel(int current_channel);
        void flush_mslab_to_dslab(Slab *mslab, Slab *dslab);
        bool quick_gc();
        bool do_quick_gc(int nr_back_to_free_dslab, int nr_back_to_ops_pool);
        bool normal_gc();
        // Delete all index entries of the dslab
        void evict_dslab(Slab *dslab, std::string *read_buffer);
        Slab *read_dslab(const Slab *dslab, std::string *read_buffer);
        // GC all full dslabs in to_drop, the valid objects to ops_slab,
        // and add freed dslabs to free_list.
        void gc_dslabs(std::vector<Slab *> &to_drop, std::string *read_buffer, std::vector<List> &free_list);
        // modify_index_to modifies index entries in src to dst
        void modify_index_to(Slab *src, Slab *dst);
        bool check_ops_pool();
    };
} // namespace KVCache
#endif