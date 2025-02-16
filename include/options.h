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
        Options() {
            slab_mem_budget = 1 * GB;
            index_mem_budget = 100 * MB;
            nr_slab_class = 16;
            slab_class_size = kSlabClassSize;
            index_table_size = 512*KB;
        }
    };
    