#ifndef KVCACHE_H
#define KVCACHE_H

#include "ssd.h"
#include "status.h"

class KVCacheImpl;

namespace KVCache
{
    constexpr int KB = 1024;
    constexpr int MB = 1024 * 1024;
    constexpr int GB = 1024 * MB;

    struct Options
    {
        int index_memory_budget = 100 * MB;
        int slab_memory_budget = 1 * GB;
    };

    class KVCache
    {
    public:
        KVCache(SSD *ssd, const Options &options);
        KVCache(const KVCache &) = delete;
        KVCache &operator=(const KVCache &) = delete;
        ~KVCache();

        Status Get(const std::string &key, std::string *value);
        Status Put(const std::string &key, const std::string &value);
        Status Delete(const std::string &key);
    private:
        std::unique_ptr<KVCacheImpl> impl_;
    };
} // namespace KVCache  

#endif // KVCACHE_H
