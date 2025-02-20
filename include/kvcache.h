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
#include <shared_mutex>
#include <string_view>
#include <string>
#include <vector>
#include <atomic>
#include <cassert>

namespace KVCache
{
    static constexpr int kDigestLength = 20;

    struct Slot
    {
        struct Header
        {
            int key_len;
            int value_len;
        };
        Header header;
        char data[0];
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
        static int slab_header_size() { return offsetof(Slab, data); }
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
            nr_used.fetch_add(1);
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
    // TODO: change slab_id to Slab*
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

        Status Get(std::string_view key, std::string *value);
        Status Put(std::string_view key, std::string_view value);
        void Delete(std::string_view key);
        int max_value_size() const { return slab_size_ - Slab::slab_header_size(); }

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
        std::unique_ptr<SSD> ssd_;
        Options options_;
        int slab_size_;
        int nr_mslab_;
        int nr_dslab_;

        std::hash<std::string_view> hasher_;

        List mslab_free_ GUARDED_BY(mslab_free_mutex_);
        List mslab_full_ GUARDED_BY(mslab_full_mutex_);
        std::mutex mslab_free_mutex_;
        std::mutex mslab_full_mutex_;
        std::vector<List> dslab_free_ GUARDED_BY(dslab_free_mutex_);
        std::vector<List> dslab_full_ GUARDED_BY(dslab_full_mutex_);
        std::mutex dslab_free_mutex_;
        std::mutex dslab_full_mutex_;
        std::vector<List> ops_pool_;
        int max_ops_pool_size_;
        int ops_pool_size_;
        int nr_free_dslab_ GUARDED_BY(dslab_free_mutex_);

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
        struct list_head *index_table_ GUARDED_BY(index_mutex_);
        int index_table_size_;
        struct list_head free_index_entry_ GUARDED_BY(index_mutex_);
        std::mutex index_mutex_;

        std::mutex writer_mutex_;
        // All writes on index and slab it points to should be protected by reader_mutex_,
        // except for Slab::nr_used and Slab::nr_alloc.
        std::shared_mutex reader_mutex_;

        std::condition_variable flush_signal_;
        std::condition_variable flush_finished_signal_;
        std::thread slab_flush_thread_;
        std::atomic_flag slab_flush_thread_started_;
        std::condition_variable gc_signal_;
        std::condition_variable gc_finished_signal_;
        std::thread slab_gc_thread_;
        std::atomic_flag slab_gc_thread_started_;
        std::mutex gc_mutex_;
        Slab *gc_buffer_;
        // Only accessed by flush thread
        int next_mslab_flush_channel_ = 0;
        // Only accessed by gc thread
        int next_ops_pool_remove_channel_ = 0;
        int free_block_water_mark_low_;
        int free_block_water_mark_high_;
        int free_block_water_mark_low_min_;
        int free_block_water_mark_high_max_;

    private:
        void slab_init();
        void index_init();

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
        void quick_gc();
        void normal_gc();
        // Delete all index entries of the dslab
        void evict_dslab(const Slab *dslab, std::string *read_buffer);
        Slab *read_dslab(const Slab *dslab, std::string *read_buffer);
        // GC all full dslabs in to_drop, the valid objects to ops_slab,
        // and add freed dslabs to free_list.
        void gc_dslabs(std::vector<Slab *> &to_drop, std::string *read_buffer, std::vector<struct list_head> &free_list);
        // modify_index_sid modifies sid of index entries in mslab to sid
        void modify_index_sid(Slab *mslab, int sid);
    };
} // namespace KVCache
#endif