#include "filter_avg.h"

void AvgFilter::push(int32_t val) {
    if (count < LOAD_CELL_AVG_SIZE) {
        buf[idx] = val;
        sum += val;
        count++;
    } else {
        sum -= buf[idx];
        buf[idx] = val;
        sum += val;
    }
    idx = (idx + 1) % LOAD_CELL_AVG_SIZE;
}

int32_t AvgFilter::get() const {
    if (count == 0) return 0;
    return (int32_t)(sum / count);
}
