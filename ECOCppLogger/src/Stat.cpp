#include "../include/ECOCppLogger/Stat.h"

#include <algorithm>
#include <bit>

namespace lg {

HistoryRing::HistoryRing(size_t capacity) {
    const size_t size = std::bit_ceil(std::max<size_t>(capacity, 2));
    capacity_ = std::max<size_t>(capacity, 1);
    mask_ = size - 1;
    entries_ = new Entry[size];
}

HistoryRing::~HistoryRing() {
    delete[] entries_;
}

}  // namespace lg
