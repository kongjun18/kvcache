#include "kvcache_c_api.h"
#include "kvcache.h"
#include <string.h>
#include <memory>

extern "C" {

struct kvcache_t {
    KVCache::KVCache *kvcache;
};

struct ssd_t {
    KVCache::SSD *ssd;
};

struct kvcache_options_t {
    KVCache::KVCache::Options options;
};

struct ssd_config_t {
    KVCache::SSD::Config config;
};

ssd_config_t* ssd_config_create() {
    auto config = new ssd_config_t();
    config->config = KVCache::SSD::Config();
    return config;
}

void ssd_config_destroy(ssd_config_t* config) {
    delete config;
}

void ssd_config_set_nr_channels(ssd_config_t* config, int nr_channels) {
    config->config.nr_channels = nr_channels;
}

void ssd_config_set_blocks_per_channel(ssd_config_t* config, int blocks_per_channel) {
    config->config.blocks_per_channel = blocks_per_channel;
}

void ssd_config_set_block_size(ssd_config_t* config, int block_size) {
    config->config.block_size = block_size;
}

ssd_t* ssd_create(const char* path, const ssd_config_t *config, char** errptr) {
    try {
        auto ssd = new ssd_t();
        KVCache::SSD::create(path, &ssd->ssd, config->config);
        return ssd;
    } catch (const std::exception& e) {
        if (errptr) *errptr = strdup(e.what());
        return nullptr;
    }
}

ssd_t* ssd_open(const char* path, char** errptr) {
    try {
        auto ssd = new ssd_t();
        ssd->ssd = new KVCache::SSD(path);
        return ssd;
    } catch (const std::exception& e) {
        if (errptr) *errptr = strdup(e.what());
        return nullptr;
    }
}

void ssd_destroy(ssd_t* ssd) {
    delete ssd;
}

kvcache_options_t* kvcache_options_create(ssd_t* ssd) {
    auto options = new kvcache_options_t();
    options->options = KVCache::KVCache::DefaultOptions(ssd->ssd);
    return options;
}

void kvcache_options_destroy(kvcache_options_t* options) {
    delete options;
}

void kvcache_options_set_slab_mem_budget(kvcache_options_t* options, size_t slab_mem_budget) {
    options->options.slab_mem_budget = slab_mem_budget;
}

void kvcache_options_set_index_mem_budget(kvcache_options_t* options, size_t index_mem_budget) {
    options->options.index_mem_budget = index_mem_budget;
}

void kvcache_options_set_index_table_size(kvcache_options_t* options, size_t index_table_size) {
    options->options.index_table_size = index_table_size;
}

void kvcache_options_set_slab_class_size(kvcache_options_t* options, int* slab_class_size, int nr_slab_class) {
    memcpy(options->options.slab_class_size, slab_class_size, sizeof(int) * nr_slab_class);
    options->options.nr_slab_class = nr_slab_class;
}

void kvcache_options_set_ops_rate(kvcache_options_t* options, float ops_rate) {
    options->options.ops_rate = ops_rate;
}

void kvcache_options_set_free_block_water_mark_low(kvcache_options_t* options, float mark) {
    options->options.free_block_water_mark_low = mark;
}

void kvcache_options_set_free_block_water_mark_high(kvcache_options_t* options, float mark) {
    options->options.free_block_water_mark_high = mark;
}

void kvcache_options_set_free_block_slab_water_mark_low_min(kvcache_options_t* options, float mark) {
    options->options.free_block_slab_water_mark_low_min = mark;
}

void kvcache_options_set_free_block_slab_water_mark_high_max(kvcache_options_t* options, float mark) {
    options->options.free_block_slab_water_mark_high_max = mark;
}

void kvcache_options_set_enable_background_flush(kvcache_options_t* options, bool enable) {
    options->options.enable_background_flush = enable;
}

void kvcache_options_set_enable_background_gc(kvcache_options_t* options, bool enable) {
    options->options.enable_background_gc = enable;
}


kvcache_t* kvcache_create(ssd_t* ssd, const kvcache_options_t *options, char** errptr) {
    try {
        auto cache = new kvcache_t();
        cache->kvcache = new KVCache::KVCache(ssd->ssd, options->options);
        return cache;
    } catch (const std::exception& e) {
        if (errptr) *errptr = strdup(e.what());
        return nullptr;
    }
}

void kvcache_destroy(kvcache_t* cache) {
    delete cache;
}

int kvcache_get(kvcache_t* cache, const char* key, size_t key_len, 
                char* value_buf, size_t buf_len, size_t* value_len, char** errptr) {
    try {
        auto status = cache->kvcache->Get(std::string_view(key, key_len), 
                                     value_buf, buf_len, value_len);
        if (!status.ok()) {
            if (errptr) *errptr = strdup(status.msg().c_str());
            return -1;
        }
        return 0;
    } catch (const std::exception& e) {
        if (errptr) *errptr = strdup(e.what());
        return -1;
    }
}

int kvcache_put(kvcache_t* cache, const char* key, size_t key_len, 
                const char* value, size_t value_len, char** errptr) {
    try {
        auto status = cache->kvcache->Put(std::string_view(key, key_len),
                                     std::string_view(value, value_len));
        if (!status.ok()) {
            if (errptr) *errptr = strdup(status.msg().c_str());
            return -1;
        }
        return 0;
    } catch (const std::exception& e) {
        if (errptr) *errptr = strdup(e.what());
        return -1;
    }
}

void kvcache_delete(kvcache_t* cache, const char* key, size_t key_len) {
    cache->kvcache->Delete(std::string_view(key, key_len));
}

size_t kvcache_max_kv_size(kvcache_t* cache) {
    return cache->kvcache->MaxKVSize();
}

} // extern "C"
