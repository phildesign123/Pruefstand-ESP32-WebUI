#pragma once
#include <stdint.h>
#include "../config.h"

struct AvgFilter {
    int32_t buf[LOAD_CELL_AVG_SIZE];
    int     idx   = 0;
    int     count = 0;
    int64_t sum   = 0;

    void    push(int32_t val);
    int32_t get() const;
};
