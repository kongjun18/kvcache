#ifndef KVCACHE_C_API_H
#define KVCACHE_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct kvcache_t kvcache_t;
typedef struct ssd_t ssd_t;
typedef struct kvcache_options_t kvcache_options_t;
typedef struct ssd_config_t ssd_config_t;

// SSD API
ssd_config_t* ssd_config_create();
void ssd_config_destroy(ssd_config_t* config);
ssd_t* ssd_create(const char* path, const ssd_config_t *config, char **errptr);
ssd_t* ssd_open(const char* path, char **errptr);
void ssd_destroy(ssd_t* ssd);
void ssd_config_set_block_size(ssd_config_t* config, int block_size);
void ssd_config_set_blocks_per_channel(ssd_config_t* config, int blocks_per_channel);
void ssd_config_set_nr_channels(ssd_config_t* config, int nr_channels);

// KVCache API
kvcache_options_t* kvcache_options_create(ssd_t* ssd);
void kvcache_options_destroy(kvcache_options_t* options);
kvcache_t* kvcache_create(ssd_t* ssd, const kvcache_options_t *options, char **errptr);
void kvcache_destroy(kvcache_t* cache);
void kvcache_options_set_index_mem_budget(kvcache_options_t* options, size_t index_mem_budget);
void kvcache_options_set_slab_mem_budget(kvcache_options_t* options, size_t slab_mem_budget);
void kvcache_options_set_slab_class_size(kvcache_options_t* options, int slab_class_size[], int nr_slab_class);
void kvcache_options_set_ops_rate(kvcache_options_t* options, float ops_rate);
void kvcache_options_set_index_table_size(kvcache_options_t* options, size_t index_table_size);
void kvcache_options_set_free_block_water_mark_low(kvcache_options_t* options, float free_block_water_mark_low);
void kvcache_options_set_free_block_water_mark_high(kvcache_options_t* options, float free_block_water_mark_high);
void kvcache_options_set_free_block_slab_water_mark_low_min(kvcache_options_t* options, float free_block_slab_water_mark_low_min);
void kvcache_options_set_free_block_slab_water_mark_high_max(kvcache_options_t* options, float free_block_slab_water_mark_high_max);
void kvcache_options_set_enable_background_flush(kvcache_options_t* options, bool enable_background_flush);
void kvcache_options_set_enable_background_gc(kvcache_options_t* options, bool enable_background_gc);

int kvcache_get(kvcache_t* cache, const char* key, size_t key_len, char* value_buf, size_t buf_len, size_t *value_len, char **errptr);
int kvcache_put(kvcache_t* cache, const char* key, size_t key_len, const char* value, size_t value_len, char **errptr);
void kvcache_delete(kvcache_t* cache, const char* key, size_t key_len);

size_t kvcache_max_kv_size(kvcache_t* cache);

#ifdef __cplusplus
}
#endif

#endif // KVCACHE_C_API_H
