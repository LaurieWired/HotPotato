#ifndef HOTPOTATO_DETAIL_POOL_STATE_HPP
#define HOTPOTATO_DETAIL_POOL_STATE_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "hotpotato/options.hpp"
#include "hotpotato/wherency.hpp"

namespace HotPotato {
namespace detail {

inline constexpr unsigned record_generation_bits = 48;
inline constexpr unsigned record_tier_bits = 13;
inline constexpr unsigned record_domain_bits = 16;
inline constexpr unsigned record_slot_bits = 48;

inline constexpr std::uint64_t max_record_generation = (std::uint64_t{1} << record_generation_bits) - 1;
inline constexpr tier_id_t max_record_tier = (tier_id_t{1} << record_tier_bits) - 1;
inline constexpr domain_id_t max_record_domain = (domain_id_t{1} << record_domain_bits) - 1;
inline constexpr std::uint64_t max_record_slot = (std::uint64_t{1} << record_slot_bits) - 1;

template <typename T>
struct Record { // minimize this since each object has to have one :(
    T* ptr = nullptr;
    
    record_id_t lru_prev = invalid_record;
    record_id_t lru_next = invalid_record;

    // 64-bit word bitmask
    std::uint64_t generation : record_generation_bits = 1;
    std::uint64_t evictable  : 1  = 1;
    std::uint64_t live       : 1  = 0;
    std::uint64_t in_lru     : 1  = 0;
    std::uint64_t tier       : record_tier_bits = 0; // up to 8192 tiers

    // second bitmask since first is full
    std::uint64_t domain     : record_domain_bits = 0; // 65,536 domains
    std::uint64_t slot_index : record_slot_bits = 0;   // 281 trillion objects per domain
};

struct DomainState : Domain {
    tier_id_t tier_id{};
    int numa_node = -1;

    void* mapping = nullptr;
    std::size_t mapping_bytes = 0;
    std::size_t stride = 0;
    bool locked = false;
    bool huge_pages = false;

    std::vector<std::size_t> free_slots;
};

struct TierState : Tier {
    record_id_t lru_head = invalid_record;
    record_id_t lru_tail = invalid_record;
};

struct SlotReservation {
    DomainState* domain;
    std::size_t slot_index;
};

} // namespace detail
} // namespace HotPotato

#endif // HOTPOTATO_DETAIL_POOL_STATE_HPP