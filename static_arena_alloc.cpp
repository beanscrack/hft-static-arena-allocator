#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,lzcnt,popcnt")

#ifndef ALLOC_STATIC_ARENA_ALLOC
#define ALLOC_STATIC_ARENA_ALLOC

#include <sys/mman.h>
#include <malloc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string_view>

namespace alloc {

class static_arena_alloc {
public:
  static constexpr std::size_t kNumBins{ 40 };
  static constexpr std::size_t kPoolSize{ 64 * 1024 }; // 64 KB (adds <0.06 MB to RSS)
  static constexpr std::uint32_t kLargeBin{ 40 };
  static constexpr std::uint32_t kOveralignedBin{ 41 };

  struct Block {
    Block* next;
  };

  struct alignas(16) Header {
    std::uint32_t bin;
    std::uint32_t size;
    std::uint64_t pad;
  };

private:
  alignas(64) static inline char pool_[kPoolSize];
  static inline std::size_t pool_ptr_{ 0 };
  static inline Block* free_lists_[kNumBins]{};

  // 40 bins spanning 16 B to 4096 B
  [[nodiscard]] static inline std::size_t get_bin(const std::size_t req) noexcept {
    if (req <= 256)  return (req <= 16) ? 0 : ((req - 1) >> 4);         // 16 B steps (bins 0..15)
    if (req <= 1024) return 16 + ((req - 257) >> 6);                    // 64 B steps (bins 16..27)
    if (req <= 4096) return 28 + ((req - 1025) >> 8);                   // 256 B steps (bins 28..39)
    return kLargeBin;
  }

  [[nodiscard]] static inline std::size_t get_bin_size(const std::size_t bin) noexcept {
    if (bin < 16) return (bin + 1) << 4;
    if (bin < 28) return 256 + ((bin - 15) << 6);
    return 1024 + ((bin - 27) << 8);
  }

public:
  [[nodiscard]] static void* alloc(
    const std::size_t pSize,
    const std::align_val_t pAlignment = static_cast<std::align_val_t>(alignof(std::max_align_t))
  ) noexcept {
    const std::size_t align = static_cast<std::size_t>(pAlignment);

    // Over-aligned allocation path (>16 B alignment)
    if (__builtin_expect(align > 16, 0)) {
      const std::size_t total_req = pSize + sizeof(Header) + align;
      const std::size_t mmap_sz = (total_req + 4095) & ~std::size_t{ 4095 };
      void* ptr = mmap(nullptr, mmap_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (ptr == MAP_FAILED) return nullptr;

      const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(ptr);
      const std::uintptr_t payload = (base + sizeof(Header) + align - 1) & ~(align - 1);
      Header* h = reinterpret_cast<Header*>(payload - sizeof(Header));
      h->bin = kOveralignedBin;
      h->size = static_cast<std::uint32_t>(mmap_sz);
      h->pad = static_cast<std::uint64_t>(payload - sizeof(Header) - base);
      return reinterpret_cast<void*>(payload);
    }

    const std::size_t req = pSize + sizeof(Header);
    const std::size_t bin = get_bin(req);

    // Small allocation path (<= 4096 B)
    if (__builtin_expect(bin < kNumBins, 1)) {
      if (free_lists_[bin]) {
        Block* blk = free_lists_[bin];
        free_lists_[bin] = blk->next;
        Header* h = reinterpret_cast<Header*>(blk);
        h->bin = static_cast<std::uint32_t>(bin);
        return reinterpret_cast<void*>(reinterpret_cast<char*>(h) + sizeof(Header));
      }

      const std::size_t chunk_size = get_bin_size(bin);

      // Fast bump from 64 KB pool
      if (__builtin_expect(pool_ptr_ + chunk_size <= kPoolSize, 1)) {
        Header* h = reinterpret_cast<Header*>(&pool_[pool_ptr_]);
        pool_ptr_ += chunk_size;
        h->bin = static_cast<std::uint32_t>(bin);
        return reinterpret_cast<void*>(reinterpret_cast<char*>(h) + sizeof(Header));
      }

      // Secondary fallback: carve single 4 KB page directly from OS (never glibc malloc)
      void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (page == MAP_FAILED) return nullptr;

      Header* h = static_cast<Header*>(page);
      h->bin = static_cast<std::uint32_t>(bin);

      std::byte* byte_ptr = static_cast<std::byte*>(page);
      const std::size_t num_chunks = 4096 / chunk_size;
      for (std::size_t i = 1; i < num_chunks; ++i) {
        Block* extra = reinterpret_cast<Block*>(byte_ptr + (i * chunk_size));
        extra->next = free_lists_[bin];
        free_lists_[bin] = extra;
      }
      return reinterpret_cast<void*>(reinterpret_cast<char*>(h) + sizeof(Header));
    }

    // Large allocation path (> 4096 B) - Direct mmap
    const std::size_t mmap_sz = (req + 4095) & ~std::size_t{ 4095 };
    void* ptr = mmap(nullptr, mmap_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;

    Header* h = static_cast<Header*>(ptr);
    h->bin = kLargeBin;
    h->size = static_cast<std::uint32_t>(mmap_sz);
    return reinterpret_cast<void*>(reinterpret_cast<char*>(h) + sizeof(Header));
  }

  static void free(void* pPtr) noexcept {
    if (!pPtr) return;

    Header* h = reinterpret_cast<Header*>(static_cast<char*>(pPtr) - sizeof(Header));
    const std::uint32_t bin = h->bin;

    // Small allocation: retain in LIFO cache (O(1))
    if (__builtin_expect(bin < kNumBins, 1)) {
      Block* blk = reinterpret_cast<Block*>(h);
      blk->next = free_lists_[bin];
      free_lists_[bin] = blk;
      return;
    }

    // Large allocation: immediately unmap from kernel (releases RSS instantly)
    if (__builtin_expect(bin == kLargeBin, 1)) {
      munmap(h, h->size);
      return;
    }

    // Over-aligned allocation
    void* original = reinterpret_cast<char*>(h) - h->pad;
    munmap(original, h->size);
  }
};

} // namespace alloc

// Standard new / delete operator overloads
void* operator new(std::size_t s) {
  void* p = alloc::static_arena_alloc::alloc(s);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { alloc::static_arena_alloc::free(p); }
void operator delete(void* p, std::size_t) noexcept { alloc::static_arena_alloc::free(p); }
void* operator new[](std::size_t s) { return ::operator new(s); }
void operator delete[](void* p) noexcept { alloc::static_arena_alloc::free(p); }
void operator delete[](void* p, std::size_t) noexcept { alloc::static_arena_alloc::free(p); }

// Aligned new / delete operator overloads
void* operator new(std::size_t s, std::align_val_t al) {
  void* p = alloc::static_arena_alloc::alloc(s, al);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p, std::align_val_t) noexcept { alloc::static_arena_alloc::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { alloc::static_arena_alloc::free(p); }
void* operator new[](std::size_t s, std::align_val_t al) { return ::operator new(s, al); }
void operator delete[](void* p, std::align_val_t) noexcept { alloc::static_arena_alloc::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { alloc::static_arena_alloc::free(p); }

// Nothrow new / delete operator overloads
void* operator new(std::size_t s, const std::nothrow_t&) noexcept { return alloc::static_arena_alloc::alloc(s); }
void operator delete(void* p, const std::nothrow_t&) noexcept { alloc::static_arena_alloc::free(p); }
void* operator new[](std::size_t s, const std::nothrow_t&) noexcept { return alloc::static_arena_alloc::alloc(s); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { alloc::static_arena_alloc::free(p); }

void* operator new(std::size_t s, std::align_val_t al, const std::nothrow_t&) noexcept {
  return alloc::static_arena_alloc::alloc(s, al);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { alloc::static_arena_alloc::free(p); }
void* operator new[](std::size_t s, std::align_val_t al, const std::nothrow_t&) noexcept {
  return alloc::static_arena_alloc::alloc(s, al);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { alloc::static_arena_alloc::free(p); }

#endif // ALLOC_STATIC_ARENA_ALLOC

// Fast I/O and runtime glibc heap trim
const int kSpeed{ [] {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  std::cout.tie(nullptr);
  malloc_trim(0);
  return 0;
}() };