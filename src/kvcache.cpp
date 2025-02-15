#include "kvcache.h"

namespace KVCache
{
    KVCache::KVCache(SSD *ssd, const Options &options)
    {
        impl_ = std::make_unique<KVCacheImpl>(ssd, options);
    }
    
    Status KVCache::Get(const std::string &key, std::string *value)
    {
        return impl_->Get(key, value);
    }

    Status KVCache::Put(const std::string &key, const std::string &value)
    {
        return impl_->Put(key, value);
    }

    Status KVCache::Delete(const std::string &key)
    {
        return impl_->Delete(key);
    }
} // namespace KVCache
