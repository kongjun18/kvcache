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
int ssd_create(const char* path, ssd_config_t* config, ssd_t** ssd, char **errptr);
int ssd_open(const char* path, ssd_t** ssd, char **errptr);
void ssd_destroy(ssd_t* ssd);

// KVCache API
int kvcache_create(ssd_t* ssd, kvcache_options_t* options, kvcache_t** cache, char **errptr);
void kvcache_destroy(kvcache_t* cache);

int kvcache_get(kvcache_t* cache, const char* key, size_t key_len, char* value_buf, size_t buf_len, size_t *value_len, char **errptr);
int kvcache_put(kvcache_t* cache, const char* key, size_t key_len, const char* value, size_t value_len, char **errptr);
void kvcache_delete(kvcache_t* cache, const char* key, size_t key_len);

size_t kvcache_max_kv_size(kvcache_t* cache);

#ifdef __cplusplus
}
#endif

#endif // KVCACHE_C_API_H
