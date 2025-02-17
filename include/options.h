#include "status.h"

static constexpr int KB = 1024;
static constexpr int MB = 1024 * 1024;
static constexpr int GB = 1024 * MB;
constexpr int kMaxSlabClassID = 24;
int kSlabClassSize[kMaxSlabClassID] = {
    64, 128, 256, 512, 1 * KB, 2 * KB, 4 * KB, 8 * KB, 16 * KB, 32 * KB, 64 * KB, 128 * KB, 256 * KB, 512 * KB, 1 * MB, 2 * MB
};
    struct Options
    {
        int slab_mem_budget;
        int index_mem_budget;
        int nr_slab_class;
        int *slab_class_size;
        int index_table_size;
        float ops_rate;
        // When the number of free blocks hits free_slab_water_mark_low, reduce ops_pool size and put ops blocks into free blocks
        float free_block_water_mark_low;
        // When the number of free blocks hits free_slab_water_mark_high, increase ops_pool size and put free blocks back into ops_pool
        float free_block_water_mark_high;
        float free_block_slab_water_mark_low_min;
        float max_block_slab_water_mark_low;
        bool enable_background_flush;
        bool enable_background_gc;
        Options() {
            slab_mem_budget = 1 * GB;
            index_mem_budget = 100 * MB;
            nr_slab_class = 16;
            slab_class_size = kSlabClassSize;
            index_table_size = 512*KB;
            enable_background_flush = true;
            enable_background_gc = true;
            ops_rate =  0.2;
            free_block_slab_water_mark_low_min = 0.01;
            max_block_slab_water_mark_low = 0.2;
            free_block_water_mark_low = 0.07;
            free_block_water_mark_high = 0.16;    
        }
    };
    