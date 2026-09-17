<img width="2301" height="971" alt="hot_potato_logo" src="https://github.com/user-attachments/assets/2d1e9ad0-1ef7-4e0e-8c02-5096b132b12d" />

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://www.apache.org/licenses/LICENSE-2.0)
[![GitHub stars](https://img.shields.io/github/stars/LaurieWired/HotPotato)](https://github.com/LaurieWired/HotPotato/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/LaurieWired/HotPotato)](https://github.com/LaurieWired/HotPotato/network/members)
[![GitHub contributors](https://img.shields.io/github/contributors/LaurieWired/HotPotato)](https://github.com/LaurieWired/HotPotato/graphs/contributors)
[![Follow @lauriewired](https://img.shields.io/twitter/follow/lauriewired?style=social)](https://twitter.com/lauriewired)

# HotPotato

This repository contains an example C++ custom allocator to demonstrate explicit placement of objects on heterogeneous memory systems. It uses C++26 Annotations and Reflection to be able to attach metadata to both objects and types for hinting at "wherency". Wherency can be defined as where an object should optimally reside according to its access patterns. Objects that are accessed more frequently and recently (hot objects) should be placed in memory tiers with lower relative access latency. Conversely, cold objects should be placed in memory tiers farther away from the main workload.

The wherency annotation can be attached either to individual object handles to be allocated within the pool, or it can be attached to a uniformly hot / cold type. With the latter, pool configurations can decide how many tiers of memory are added to the pool. Even within uniform pools, the wherency annotations can also be applied to individual object handles. The decision of the handle was chosen to abstract the actual pointer `T*` which may change locations spontaneously due to promotion, demotion, or eviction.

By default, the pool uses an LRU algorithm for evicting objects to slower tiers. This can be overridden or disabled for objects / types. Domains of memory are bound to particular NUMA nodes. This can also simulate a CXL workload as the slowest tier of memory, acting like a headless NUMA node.

<img width="2570" height="1713" alt="signal-2026-09-17-09-28-22-430" src="https://github.com/user-attachments/assets/b549c0a9-69a8-4c18-8dee-205826744edc" />

## Usage

The library is available in the `include/hotpotato/` directory. To use it, copy `include/hotpotato` into your project and `#include "hotpotato/tiered_pool.hpp"`. A full example showing handle-level placement, type-level cold pools, and runtime promote / demote can be found in `main.cpp`. To use HotPotato:

1. **Configure a pool**: Construct a `HotPotato::TieredPool<T>` and call `configure_pool(local_node, max_tiers, objects_per_domain)` to bind domains onto NUMA nodes.
2. **Annotate placement**: Mark handles (or the pooled type itself) with `[[=HotPotato::likely]]`, `[[=HotPotato::unlikely]]`, and/or `[[=HotPotato::Evictable{false}]]`.
3. **Allocate through a handle**: Pass `allocate<^^handle>(...)` so reflection can read those annotations. Access objects with `get()`, and optionally `promote()` / `demote()` them at runtime.

### Example 1

Handle-level annotations place individual objects onto hot or cold tiers, pin them against LRU eviction, or both:

```cpp
#include <cstdint>
#include <iostream>
#include "hotpotato/handle.hpp"
#include "hotpotato/tiered_pool.hpp"

struct Record {
    std::uint64_t id{};
    float x{};
    float y{};
    float z{};
    char payload[40]{};
};

int main() {
    HotPotato::TieredPool<Record> records;
    records.configure_pool(0, 2, 1024);

    [[=HotPotato::likely]]
    HotPotato::Handle<Record> hot_row =
        records.allocate<^^hot_row>(1ULL, 10.0f, 20.0f, 30.0f).value();

    [[=HotPotato::unlikely]]
    HotPotato::Handle<Record> archived_row =
        records.allocate<^^archived_row>(2ULL, 100.0f, 200.0f, 300.0f).value();

    [[=HotPotato::Evictable{false}]]
    HotPotato::Handle<Record> pinned_row =
        records.allocate<^^pinned_row>(3ULL, 1.0f, 2.0f, 3.0f).value();

    std::cout << "hot_row tier=" << *records.get_tier(hot_row)
              << " id=" << records.get(hot_row)->id << '\n';

    records.deallocate(hot_row);
    records.deallocate(archived_row);
    records.deallocate(pinned_row);
    return 0;
}
```

### Example 2

A type-level `unlikely` annotation places the whole pool on the farthest NUMA nodes, and `Evictable{false}` pins every object against LRU unless a handle overrides it:

```cpp
#include <cstdint>
#include <iostream>
#include "hotpotato/handle.hpp"
#include "hotpotato/tiered_pool.hpp"

struct [[=HotPotato::unlikely, =HotPotato::Evictable{false}]] ColdLog {
    std::uint64_t id{};
    char message[64]{};
};

int main() {
    HotPotato::TieredPool<ColdLog> logs;
    logs.configure_pool(0, 2, 1024);

    HotPotato::Handle<ColdLog> default_log = logs.allocate(99ULL).value();

    [[=HotPotato::likely]]
    HotPotato::Handle<ColdLog> urgent_log =
        logs.allocate<^^urgent_log>(100ULL).value();

    std::cout << "default_log tier=" << *logs.get_tier(default_log)
              << " domain=" << *logs.get_domain(default_log) << '\n';
    std::cout << "urgent_log tier=" << *logs.get_tier(urgent_log)
              << " domain=" << *logs.get_domain(urgent_log) << '\n';

    logs.deallocate(default_log);
    logs.deallocate(urgent_log);
    return 0;
}
```

## Build Example

To build and run the example code, run the following:

```
g++-16 -std=c++26 -freflection -O3 -I include main.cpp -o hotpotato
./hotpotato
```

If NUMA headers are present, also link libnuma (`-lnuma`).

## Important Usage Notes

- Currently requires C++26 annotations and reflection with the `-freflection` flag
- Pooled type `T` must be trivially copyable or nothrow move-constructible
- Always store `HotPotato::Handle<T>` instead of raw `T*` or just account that the underlying address may change after promotion, demotion, or eviction
- Type-level `unlikely` selects the farthest NUMA nodes for the pool. Object allocation is still done as closest-first within those nodes
- Without libnuma, `configure_pool` falls back to a single local domain
