// g++-16 -std=c++26 -freflection -O3 -I include main.cpp -o hotpotato

#include <cstdint>
#include <exception>
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

// whole pool placed on the farthest max_tiers nodes
struct [[=HotPotato::unlikely, =HotPotato::Evictable{false}]] ColdLog {
    std::uint64_t id{};
    char message[64]{};
};

// using all the tiers, but heavy customization of object placements
void record_pool_example() {
    HotPotato::TieredPool<Record> records;
    records.configure_pool(0, 2, 1024, HotPotato::DomainOptions{.lock_pages = true, .huge_pages = false});

    std::cout << "NUMA="
              << (HotPotato::TieredPool<Record>::numa_supported() ? "yes" : "no")
              << " tiers=" << records.tier_count() << '\n';

    [[=HotPotato::likely]]
    HotPotato::Handle<Record> hot_row =
        records.allocate<^^hot_row>(1ULL, 10.0f, 20.0f, 30.0f).value();

    [[=HotPotato::unlikely]]
    HotPotato::Handle<Record> archived_row =
        records.allocate<^^archived_row>(2ULL, 100.0f, 200.0f, 300.0f).value();

    [[=HotPotato::Evictable{false}]]
    HotPotato::Handle<Record> pinned_row =
        records.allocate<^^pinned_row>(3ULL, 1.0f, 2.0f, 3.0f).value();

    [[=HotPotato::likely, =HotPotato::Evictable{false}]]
    HotPotato::Handle<Record> pinned_hot_row =
        records.allocate<^^pinned_hot_row>(4ULL).value();

    std::cout << "hot_row: id=" << records.get(hot_row)->id
              << " tier=" << *records.get_tier(hot_row)
              << " domain=" << *records.get_domain(hot_row)
              << " evictable=" << *records.is_evictable(hot_row)
              << '\n';

    std::cout << "archived: id=" << records.get(archived_row)->id
              << " tier=" << *records.get_tier(archived_row)
              << " domain=" << *records.get_domain(archived_row)
              << " evictable=" << *records.is_evictable(archived_row)
              << '\n';

    std::cout << "pinned: id=" << records.get(pinned_row)->id
              << " tier=" << *records.get_tier(pinned_row)
              << " domain=" << *records.get_domain(pinned_row)
              << " evictable=" << *records.is_evictable(pinned_row)
              << '\n';

    if (records.tier_count() > 1) {
        std::cout << "\nRuntime migration demo:\n";

        const HotPotato::tier_id_t hot_before = *records.get_tier(hot_row);
        if (records.demote(hot_row)) {
            std::cout << "hot_row demoted: tier " << hot_before
                      << " -> " << *records.get_tier(hot_row)
                      << ", id still " << records.get(hot_row)->id << '\n';
        }

        const HotPotato::tier_id_t archived_before = *records.get_tier(archived_row);
        if (records.promote(archived_row)) {
            std::cout << "archived promoted: tier " << archived_before
                      << " -> " << *records.get_tier(archived_row)
                      << ", id still " << records.get(archived_row)->id << '\n';
        }

        const HotPotato::tier_id_t pinned_before = *records.get_tier(pinned_row);
        if (records.demote(pinned_row)) {
            std::cout << "pinned explicitly demoted despite not_evictable: tier "
                      << pinned_before << " -> "
                      << *records.get_tier(pinned_row) << '\n';
        }
    }

    records.deallocate(hot_row);
    records.deallocate(archived_row);
    records.deallocate(pinned_row);
    records.deallocate(pinned_hot_row);
}

void cold_pool_example() {
    HotPotato::TieredPool<ColdLog> logs;
    logs.configure_pool(0, 2, 1024);

    // unlikely type: farthest max_tiers nodes get domains (e.g. 6 nodes -> 4 and 5).
    // object allocate is still closest-first: likely -> node 4, unlikely -> node 5.
    HotPotato::Handle<ColdLog> default_log = logs.allocate(99ULL).value();
    std::cout << "default_log: id=" << logs.get(default_log)->id
              << " tier=" << *logs.get_tier(default_log)
              << " domain=" << *logs.get_domain(default_log)
              << " evictable=" << *logs.is_evictable(default_log)
              << " (tier 0 = closest of the far nodes)\n";

    [[=HotPotato::likely]]
    HotPotato::Handle<ColdLog> urgent_log = logs.allocate<^^urgent_log>(100ULL).value();
    std::cout << "urgent_log: id=" << logs.get(urgent_log)->id
              << " tier=" << *logs.get_tier(urgent_log)
              << " domain=" << *logs.get_domain(urgent_log)
              << " evictable=" << *logs.is_evictable(urgent_log)
              << " (handle likely -> tier 0)\n";

    logs.deallocate(default_log);
    logs.deallocate(urgent_log);
}

int main() {
    try {
        record_pool_example();
        std::cout << "\nCold pool example:\n";
        cold_pool_example();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }

    return 0;
}
