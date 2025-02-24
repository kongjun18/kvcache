#include "kvcache_c_api.h"
#include "kvcache.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern "C" {

struct kvcache_t {
    KVCache::KVCache *kvcache;
};

struct ssd_t {
    KVCache::SSD *ssd;
    int (*block_size)(ssd_t* ssd);
    int (*nr_blocks)(ssd_t* ssd);
};

struct kvcache_options_t {
    KVCache::KVCache::Options options;
};

struct ssd_config_t {
    KVCache::SSD::Config config;
};


int ssd_create(const char* path, ssd_config_t* config, ssd_t** ssd, char **errptr) {
    auto status = KVCache::SSD::create(path, &(*ssd)->ssd, config->config);
    if (!status.ok()) {
        *errptr = strdup(status.msg().c_str());
        return -1;
    }
    return 0;
}

int ssd_open(const char* path, ssd_t** ssd, char **errptr) {
    try {
        *ssd = new ssd_t();
        (*ssd)->ssd = new KVCache::SSD(path);
        (*ssd)->block_size = [](ssd_t* ssd) { return ssd->ssd->block_size_; };
        (*ssd)->nr_blocks = [](ssd_t* ssd) { return ssd->ssd->nr_blocks_; };
        return 0;
    } catch (const std::exception& e) {
        *errptr = strdup(e.what());
        return -1;
    }
}

void ssd_destroy(ssd_t* ssd) {
    delete ssd->ssd;
    delete ssd;
}

int kvcache_create(ssd_t* ssd, kvcache_options_t* options, kvcache_t** cache, char **errptr) { 
    try {
        *cache = new kvcache_t();
        (*cache)->kvcache = new KVCache::KVCache(ssd->ssd, options->options);
        return 0;
    } catch (const std::exception& e) {
        *errptr = strdup(e.what());
        return -1;
    }
}

void kvcache_destroy(kvcache_t* cache) {
    delete cache->kvcache;
    delete cache;
}

int kvcache_get(kvcache_t* cache, const char* key, size_t key_len, char* value_buf, size_t buf_len, size_t *value_len, char **errptr) {
    auto status = cache->kvcache->Get(std::string_view(key, key_len), value_buf, buf_len, value_len);
    if (!status.ok()) {
        *errptr = strdup(status.msg().c_str());
        return -1;
    }
    return 0;
}

int kvcache_put(kvcache_t* cache, const char* key, size_t key_len, const char* value, size_t value_len, char **errptr) {
    auto status = cache->kvcache->Put(std::string_view(key, key_len), std::string_view(value, value_len));
    if (!status.ok()) {
        *errptr = strdup(status.msg().c_str());
        return -1;
    }
    return 0;
}   

void kvcache_delete(kvcache_t* cache, const char* key, size_t key_len){
    cache->kvcache->Delete(std::string_view(key, key_len));
}

size_t kvcache_max_kv_size(kvcache_t* cache) {
    return cache->kvcache->MaxKVSize();
}   

} // extern "C"
