#include "../src/mtvar.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <unordered_set>

namespace {

struct CopyTrackedSet {
    static std::atomic<unsigned int> copies;
    std::unordered_set<uint32_t> values;

    CopyTrackedSet() = default;
    CopyTrackedSet(const CopyTrackedSet& other) : values(other.values) {
        copies.fetch_add(1, std::memory_order_relaxed);
    }
    CopyTrackedSet& operator=(const CopyTrackedSet& other) {
        values = other.values;
        copies.fetch_add(1, std::memory_order_relaxed);
        return *this;
    }

    bool contains(uint32_t value) const {
        return values.contains(value);
    }
};

std::atomic<unsigned int> CopyTrackedSet::copies{0};

} // namespace

int main() {
    CopyTrackedSet source;
    for (uint32_t value = 1; value <= 10000; ++value)
        source.values.insert(value);

    MTVariable<CopyTrackedSet> variable(source);
    CopyTrackedSet::copies.store(0, std::memory_order_relaxed);

    for (uint32_t value = 1; value <= 10000; ++value) {
        if (!variable.contains(value)) {
            std::fprintf(stderr, "FAIL: membership lookup lost value %u\n", value);
            return 1;
        }
    }
    if (variable.contains(10001)) {
        std::fprintf(stderr, "FAIL: membership lookup accepted a missing value\n");
        return 1;
    }
    if (CopyTrackedSet::copies.load(std::memory_order_relaxed) != 0) {
        std::fprintf(stderr, "FAIL: membership lookup copied the guarded container\n");
        return 1;
    }

    std::puts("MTVariable contains test passed");
    return 0;
}
