#include "filter_median.h"
#include <string.h>

void MedianFilter::push(int32_t val) {
    buf[idx] = val;
    idx = (idx + 1) % LOAD_CELL_MEDIAN_SIZE;
    if (count < LOAD_CELL_MEDIAN_SIZE) count++;
}

int32_t MedianFilter::get() const {
    if (count == 0) return 0;
    // Kopieren und sortieren
    int32_t tmp[LOAD_CELL_MEDIAN_SIZE];
    memcpy(tmp, buf, count * sizeof(int32_t));

    // Insertion Sort (kleines Fenster, sehr effizient)
    for (int i = 1; i < count; i++) {
        int32_t key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = key;
    }
    return tmp[count / 2];
}
