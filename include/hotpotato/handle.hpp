#ifndef HOTPOTATO_HANDLE_HPP
#define HOTPOTATO_HANDLE_HPP

#include <cstdint>

#include "hotpotato/options.hpp"

namespace HotPotato {

// handle so that the underlying address can change and move around without
//  being affected when the object gets migrated
template <typename T>
struct Handle {
    record_id_t record_id = invalid_record;
    std::uint64_t generation = 0;

    explicit operator bool() const noexcept {
        return record_id != invalid_record;
    }

    bool operator==(const Handle& other) const = default;
};

} // namespace HotPotato

#endif // HOTPOTATO_HANDLE_HPP