#include "kvcache.h"

namespace KVCache
{
    class KVCacheImpl
    {
    public:
        KVCacheImpl(SSD *ssd, const Options &options);
        KVCacheImpl(const KVCacheImpl &) = delete;
        KVCacheImpl &operator=(const KVCacheImpl &) = delete;

        ~KVCacheImpl();

        Status Get(const std::string &key, std::string *value);
        Status Put(const std::string &key, const std::string &value);
        Status Delete(const std::string &key);

    private:
        std::unique_ptr<SSD> ssd_;
        Options options_;

        
    };
} // namespace KVCache
