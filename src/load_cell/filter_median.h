#pragma once
#include <stdint.h>
#include "../config.h"

struct MedianFilter {
    int32_t buf[LOAD_CELL_MEDIAN_SIZE];
    int32_t sorted[LOAD_CELL_MEDIAN_SIZE];
    int     idx   = 0;
    int     count = 0;

    void    push(int32_t val);
    int32_t get() const;
};
