#include "status.h"
static constexpr int KB = 1024;
static constexpr int MB = 1024 * 1024;
static constexpr int GB = 1024 * MB;
static constexpr int kMaxSlabClassID = 24;
static constexpr int kSlabClassSize[kMaxSlabClassID] = {
    64, 128, 256, 512, 1 * KB, 2 * KB, 4 * KB, 8 * KB, 16 * KB, 32 * KB, 64 * KB, 128 * KB, 256 * KB, 512 * KB, 1 * MB, 2 * MB};
struct Options
{
    int slab_mem_budget;
    int index_mem_budget;
    int nr_slab_class;
    int slab_class_size[kMaxSlabClassID];
    int index_table_size;
    float ops_rate;
    // When the number of free blocks hits free_slab_water_mark_low, reduce ops_pool size and put ops blocks into free blocks
    float free_block_water_mark_low;
    // When the number of free blocks hits free_slab_water_mark_high, increase ops_pool size and put free blocks back into ops_pool
    float free_block_water_mark_high;
    float free_block_slab_water_mark_low_min;
    float free_block_slab_water_mark_high_max;
    bool enable_background_flush;
    bool enable_background_gc;
    static Options DefaultOptions()
    {
        Options options;
        options.slab_mem_budget = 1 * GB;
        options.index_mem_budget = 100 * MB;
        options.nr_slab_class = sizeof(kSlabClassSize) / sizeof(kSlabClassSize[0]);
        options.index_table_size = 512 * KB;
        options.enable_background_flush = true;
        options.enable_background_gc = true;
        options.ops_rate = 0.2;
        options.free_block_slab_water_mark_low_min = 0.01;
        options.free_block_slab_water_mark_high_max = 0.2;
        options.free_block_water_mark_low = 0.07;
        options.free_block_water_mark_high = 0.16;
        return options;
    }
};
