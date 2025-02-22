#include "status.h"

namespace KVCache   
{
    static constexpr int KB = 1024;
    static constexpr int MB = 1024 * 1024;
    static constexpr int GB = 1024 * MB;
struct Options
{
    int slab_mem_budget;
    int index_mem_budget;
    int index_table_size;
    float ops_rate;
    // When the number of free blocks hits free_block_water_mark_low, reduce ops_pool size and put ops blocks into free blocks
    float free_block_water_mark_low;
    // When the number of free blocks hits free_block_water_mark_high, increase ops_pool size and put free blocks back into ops_pool
    float free_block_water_mark_high;
    float free_block_slab_water_mark_low_min;
    float free_block_slab_water_mark_high_max;
    bool enable_background_flush;
    bool enable_background_gc;
    Options() {
        slab_mem_budget = 1 * GB;
        index_mem_budget = 100 * MB;
        index_table_size = 512 * KB;
        enable_background_flush = true;
        enable_background_gc = true;
        ops_rate = 0.2;
        free_block_slab_water_mark_low_min = 0.02;
        free_block_slab_water_mark_high_max = 0.90;
        free_block_water_mark_low = 0.07;
        free_block_water_mark_high = 0.80;
    }
    static Options DefaultOptions()
    {
        Options options;
        return options;
    }
};
}