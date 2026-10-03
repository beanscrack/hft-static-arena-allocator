# HFT Static Arena Allocator

An experimental C++ global allocator prototype for exploring allocation behavior in latency-sensitive, single-threaded high-frequency trading (HFT) worker code paths.

## Design

- A 64 KiB static bump pool for small allocations.
- 40 size classes from 16 bytes through 4 KiB, with LIFO free lists.
- `mmap`-backed fallback pages when a small bin runs out of pool space.
- Direct `mmap` for large and over-aligned allocations, released with `munmap`.
- Global C++ `new`/`delete` overloads.

## Scope and limitations

This is a prototype for single-threaded allocation paths. The pool pointer and free lists are shared static state, so the implementation is **not thread-safe** and cannot be used concurrently by multiple worker threads in one process. It also has no cross-thread free handling.

The source targets GCC on Linux/x86: it uses POSIX `mmap`, GCC builtins and x86 AVX2/BMI target pragmas, and glibc `malloc_trim`. It does not check all allocation-size arithmetic for overflow, and mapping sizes are stored in 32-bit metadata. No benchmark results or production suitability claims are included; measure correctness, throughput, tail latency, and memory use against the actual workload before considering deployment.

The implementation is kept as supplied in `static_arena_alloc.cpp`.