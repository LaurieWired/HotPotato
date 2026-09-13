#ifndef HOTPOTATO_WHERENCY_HPP
#define HOTPOTATO_WHERENCY_HPP

#include <meta>
#include <optional>
#include <vector>

#include "hotpotato/options.hpp"

namespace HotPotato {

/*
Types used for annotations. Handle annotations pick an object's compile-time wherency.
A Wherency annotation on T itself only affects which NUMA nodes get domains: 
unlikely takes the farthest max_tiers nodes, but object allocation still uses clostest of those
*/
struct Wherency {
    enum class kind {
        likely, // 0th tier. unless it's full and then it'll just be the next closest spot
        unlikely, // last tier
        exact_tier,
    };

    kind type{kind::likely}; // default to likely
    tier_id_t exact{};

    static constexpr Wherency tier(tier_id_t id) noexcept {
        return {.type = kind::exact_tier, .exact = id};
    }
};

struct Evictable {
    bool value{true}; // default to evictable
};

// just convenient so caller doesn't have to type the full thing
inline constexpr Wherency likely{.type = Wherency::kind::likely};
inline constexpr Wherency unlikely{.type = Wherency::kind::unlikely};

/*
Retrieving annotations from a handle declaration or from the pooled type (^^T)
*/
consteval std::optional<Wherency> wherency_annotation(std::meta::info entity) {
    const std::vector<std::meta::info> annotations =
            std::meta::annotations_of_with_type(entity, ^^Wherency);
    if (annotations.empty()) return std::nullopt;

    return std::meta::extract<Wherency>(annotations.front());
}

consteval std::optional<bool> evictable_annotation(std::meta::info entity) {
    const std::vector<std::meta::info> annotations = 
            std::meta::annotations_of_with_type(entity, ^^Evictable);
    if (annotations.empty()) return std::nullopt;

    return std::meta::extract<Evictable>(annotations.front()).value;
}

} // namespace HotPotato

#endif // HOTPOTATO_WHERENCY_HPP