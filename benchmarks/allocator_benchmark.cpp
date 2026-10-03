#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <new>

namespace {
constexpr std::size_t kSlotCount = 1024;
constexpr std::size_t kWarmupIterations = 1'000'000;
constexpr std::size_t kTimedIterations = 3'000'000;
constexpr std::size_t kRounds = 7;
constexpr std::array<std::size_t, 8> kMixedSizes{
    24, 48, 96, 192, 384, 768, 1536, 3072};

struct Slot {
  void* ptr;
};

template <bool kMixed>
std::size_t request_size(const std::size_t iteration,
                         const std::size_t slot_index) noexcept {
  if constexpr (kMixed) {
    const std::size_t phase = iteration / kSlotCount;
    return kMixedSizes[(phase + slot_index * 5) & (kMixedSizes.size() - 1)];
  }
  return 64;
}

template <bool kMixed>
void churn_once(std::array<Slot, kSlotCount>& slots,
                const std::size_t iteration) {
  const std::size_t index = iteration & (kSlotCount - 1);
  Slot& slot = slots[index];
  ::operator delete(slot.ptr);
  const std::size_t size = request_size<kMixed>(iteration, index);
  slot.ptr = ::operator new(size);
}

template <bool kMixed>
void run_workload(const char* label) {
  std::array<Slot, kSlotCount> slots{};
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    const std::size_t size = request_size<kMixed>(i, i);
    slots[i].ptr = ::operator new(size);
  }

  for (std::size_t i = 0; i < kWarmupIterations; ++i) {
    churn_once<kMixed>(slots, i);
  }

  std::array<double, kRounds> ns_per_pair{};
  for (std::size_t round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kTimedIterations; ++i) {
      churn_once<kMixed>(slots, i);
    }
    const auto stop = std::chrono::steady_clock::now();
    const double elapsed_ns =
        std::chrono::duration<double, std::nano>(stop - start).count();
    ns_per_pair[round] = elapsed_ns / static_cast<double>(kTimedIterations);
  }

  std::sort(ns_per_pair.begin(), ns_per_pair.end());
  const double median = ns_per_pair[kRounds / 2];
  const double pairs_per_second_millions = 1000.0 / median;
  std::printf("%s: median %.2f ns per alloc/free pair (%.2f M pairs/s), "
              "range %.2f-%.2f ns\n",
              label, median, pairs_per_second_millions,
              ns_per_pair.front(), ns_per_pair.back());

  for (Slot& slot : slots) {
    ::operator delete(slot.ptr);
  }
}
}  // namespace

int main() {
  run_workload<false>("fixed 64 B");
  run_workload<true>("mixed 24-3072 B");
}