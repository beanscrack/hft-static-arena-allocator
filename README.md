# HFT Static Arena Allocator

An experimental C++ global allocator prototype for exploring allocation behavior in latency-sensitive, single-threaded high-frequency trading (HFT) worker code paths.

## Design

- A 64 KiB static bump pool for small allocations.
- 40 size classes from 16 bytes through 4 KiB, with LIFO free lists.
- `mmap`-backed fallback pages when a small bin runs out of pool space.
- Direct `mmap` for large and over-aligned allocations, released with `munmap`.
- Global C++ `new`/`delete` overloads.

## Benchmark

A single-threaded steady-state churn benchmark compares the platform `new`/`delete` implementation with this allocator. Each pair frees one occupied slot and allocates its replacement from a ring of 1,024 slots. The fixed workload requests 64 bytes. The mixed workload rotates through 24, 48, 96, 192, 384, 768, 1,536, and 3,072-byte requests. Each process warms up for 1,000,000 pairs, then measures seven rounds of 3,000,000 pairs. The table reports the median of three process runs; ranges show the minimum and maximum round medians observed across those runs.

| Workload | Platform `new`/`delete` | This allocator | Speedup |
| --- | ---: | ---: | ---: |
| Fixed 64 B | 4.67 ns/pair (~214 M pairs/s) | 1.69 ns/pair (~592 M pairs/s) | 2.76x |
| Mixed 24–3,072 B | 6.23 ns/pair (~161 M pairs/s) | 2.01 ns/pair (~496 M pairs/s) | 3.10x |

Observed per-round ranges: fixed 64 B was 4.64–4.73 ns/pair for the platform allocator and 1.68–1.74 ns/pair for this allocator; mixed sizes were 6.17–6.91 ns/pair and 1.98–2.03 ns/pair, respectively. Runs were performed on 2026-10-03.

The benchmark was run on an AMD Ryzen 7 9800X3D host in a Dockerized Ubuntu Linux 6.18.33.2 WSL2 environment, using GCC 13.3.0 and glibc 2.39. Both executables used `-O3 -DNDEBUG -std=c++20 -march=native`; the custom executable linked `static_arena_alloc.cpp`.

To reproduce on Linux/x86 with GCC:

```sh
g++ -O3 -DNDEBUG -std=c++20 -march=native benchmarks/allocator_benchmark.cpp -o /tmp/allocator_benchmark_system
g++ -O3 -DNDEBUG -std=c++20 -march=native benchmarks/allocator_benchmark.cpp static_arena_alloc.cpp -o /tmp/allocator_benchmark_custom
/tmp/allocator_benchmark_system
/tmp/allocator_benchmark_custom
```

These results cover warmed small-allocation churn only. The timed loop does not touch allocated payloads, and the test does not measure large `mmap` allocations, multithreaded contention, tail latency, or full HFT application behavior. Treat the measurements as a local microbenchmark, not a production performance guarantee.

## Scope and limitations

This is a prototype for single-threaded allocation paths. The pool pointer and free lists are shared static state, so the implementation is **not thread-safe** and cannot be used concurrently by multiple worker threads in one process. It also has no cross-thread free handling.

The source targets GCC on Linux/x86: it uses POSIX `mmap`, GCC builtins and x86 AVX2/BMI target pragmas, and glibc `malloc_trim`. It does not check all allocation-size arithmetic for overflow, and mapping sizes are stored in 32-bit metadata. No production suitability claims are included; measure correctness, throughput, tail latency, and memory use against the actual workload before considering deployment.

The implementation is kept as supplied in `static_arena_alloc.cpp`.