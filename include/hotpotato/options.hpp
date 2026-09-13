#ifndef HOTPOTATO_OPTIONS_HPP
#define HOTPOTATO_OPTIONS_HPP

#include <cstddef>
#include <limits>
#include <vector>

namespace HotPotato {

using tier_id_t = std::size_t;
using domain_id_t = std::size_t;
using record_id_t = std::size_t;

inline constexpr record_id_t invalid_record = std::numeric_limits<record_id_t>::max();

struct Tier {
    tier_id_t id{};
    std::vector<domain_id_t> domains;
};

// Multiple domains in a tier (they are considered equal-ish latency)
struct Domain {
    domain_id_t domain_id{};
    std::size_t capacity{};
    std::size_t size{};
};

// where an item actually currently resides
struct Residency {
    tier_id_t tier{};
    domain_id_t domain{};
};

struct DomainOptions {
    bool lock_pages = true; // not needed for huge pages
    bool huge_pages = false;
    std::size_t huge_page_size = 2ULL * 1024 * 1024;
};

}

#endif // HOTPOTATO_OPTIONS_HPP
