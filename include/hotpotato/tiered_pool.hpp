#ifndef HOTPOTATO_TIERED_POOL_HPP
#define HOTPOTATO_TIERED_POOL_HPP

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <meta>
#include <new>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__has_include)
#  if __has_include(<numa.h>) && __has_include(<numaif.h>)
#    include <numa.h>
#    include <numaif.h>
#    define HOTPOTATO_HAS_NUMA 1
#  endif
#endif

#include <sys/mman.h>
#include <unistd.h>

#include "hotpotato/detail/pool_state.hpp"
#include "hotpotato/handle.hpp"
#include "hotpotato/options.hpp"
#include "hotpotato/wherency.hpp"

namespace HotPotato {

template <typename T>
class TieredPool {
    static_assert(!std::is_reference_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>);
    static_assert(
        std::is_trivially_copyable_v<T> || std::is_nothrow_move_constructible_v<T>,
        "HotPotato v1 requires T to be trivially copyable or nothrow move-constructible "
        "so object migration cannot strand an object between tiers."
    );

    using Record = detail::Record<T>;
    using DomainState = detail::DomainState;
    using TierState = detail::TierState;
    using SlotReservation = detail::SlotReservation;

public:
    using value_type = T;

    TieredPool() = default;

    TieredPool(const TieredPool&) = delete;
    TieredPool& operator=(const TieredPool&) = delete;
    TieredPool(TieredPool&&) = delete;
    TieredPool& operator=(TieredPool&&) = delete;

    ~TieredPool() {
        for (Record& record : records_) {
            if (record.live && record.ptr) {
                std::destroy_at(record.ptr);
                record.live = false;
                record.ptr = nullptr;
            }
        }

        for (DomainState& d : domains_) {
            if (d.locked && d.mapping) ::munlock(d.mapping, d.mapping_bytes);
            if (d.mapping) ::munmap(d.mapping, d.mapping_bytes);
        }
    }

    bool add_domain(tier_id_t tier_id, domain_id_t domain_id, int numa_node, std::size_t object_capacity, DomainOptions options = {}) {
        if (object_capacity == 0
            || object_capacity - 1 > detail::max_record_slot
            || domain_id > detail::max_record_domain
            || tier_id > detail::max_record_tier
            || domain_index_.contains(domain_id)
            || !ensure_tier_exists(tier_id)) {
            return false;
        }

#ifdef HOTPOTATO_HAS_NUMA
        if (numa_supported() && (numa_node < 0 || numa_node > numa_max_node())) return false;
#endif

        std::optional<std::size_t> opt_stride = align_up(sizeof(T), alignof(T));
        if (!opt_stride) return false;

        std::optional<std::size_t> opt_raw_bytes = checked_multiply(*opt_stride, object_capacity);
        if (!opt_raw_bytes) return false;
        
        const std::size_t mapping_alignment = options.huge_pages ? options.huge_page_size : static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        std::optional<std::size_t> opt_mapping_bytes = align_up(*opt_raw_bytes, mapping_alignment);
        if (!opt_mapping_bytes) return false;

        int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | (options.huge_pages ? MAP_HUGETLB : 0);
        void* mapping = ::mmap(nullptr, *opt_mapping_bytes, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
        if (mapping == MAP_FAILED) return false;

        if (!bind_mapping_to_node(mapping, *opt_mapping_bytes, numa_node)) {
            ::munmap(mapping, *opt_mapping_bytes);
            return false;
        }

        std::memset(mapping, 0, *opt_mapping_bytes);

        bool locked = false;
        if (options.lock_pages && !options.huge_pages) {
            if (::mlock(mapping, *opt_mapping_bytes) != 0) {
                ::munmap(mapping, *opt_mapping_bytes);
                return false;
            }
            locked = true;
        }

        DomainState state;
        state.domain_id = domain_id;
        state.capacity = object_capacity;
        state.size = 0;
        state.tier_id = tier_id;
        state.numa_node = numa_node;
        state.mapping = mapping;
        state.mapping_bytes = *opt_mapping_bytes;
        state.stride = *opt_stride;
        state.locked = locked;
        state.huge_pages = options.huge_pages;

        state.free_slots.reserve(object_capacity);
        for (std::size_t i = object_capacity; i > 0; --i) {
            state.free_slots.push_back(i - 1);
        }

        domain_index_.emplace(domain_id, domains_.size());
        domains_.push_back(std::move(state));
        tiers_[tier_id].domains.push_back(domain_id);
        return true;
    }

    bool configure_pool([[maybe_unused]] int local_node, std::size_t max_tiers, std::size_t objects_per_domain, DomainOptions options = {}) {
        if (!domains_.empty() || max_tiers == 0 || max_tiers - 1 > detail::max_record_tier) return false;

        // type-level likely / unlikely defines what nodes the pool is placed on
        // object-level likely is still the nearest selected node
        constexpr Wherency type_wherency = wherency_annotation(^^T).value_or(Wherency{});
        [[maybe_unused]] constexpr bool far_nodes = (type_wherency.type == Wherency::kind::unlikely);

#ifdef HOTPOTATO_HAS_NUMA
        if (numa_supported()) {
            if (local_node < 0 || local_node > numa_max_node()) return false;

            std::vector<std::pair<int, int>> valid_nodes; // {node_id, distance}

            for (int node = 0; node <= numa_max_node(); ++node) {
                if (node != local_node && !numa_bitmask_isbitset(numa_all_nodes_ptr, node)) continue;

                int dist = ::numa_distance(local_node, node);
                if (dist == 0) {
                    dist = (node == local_node) ? 10 : 20;
                }
                valid_nodes.push_back({node, dist});
            }

            if (valid_nodes.empty()) return false;

            // closest first -> smaller SLIT distance, then lower node id
            for (std::size_t i = 0; i < valid_nodes.size(); ++i) {
                for (std::size_t j = i + 1; j < valid_nodes.size(); ++j) {
                    if (valid_nodes[j].second < valid_nodes[i].second
                        || (valid_nodes[j].second == valid_nodes[i].second
                            && valid_nodes[j].first < valid_nodes[i].first)) {
                        std::swap(valid_nodes[i], valid_nodes[j]);
                    }
                }
            }

            // likely -> nodes[0, max_tiers)
            // unlikely -> nodes[n - max_tiers, n)
            // still allocate on the nearest node by default
            const std::size_t count = (valid_nodes.size() < max_tiers) ? valid_nodes.size() : max_tiers;
            const std::size_t begin = far_nodes ? (valid_nodes.size() - count) : 0;

            std::vector<int> distances;
            for (std::size_t i = begin; i < begin + count; ++i) {
                const int dist = valid_nodes[i].second;
                bool found = false;
                for (int d : distances) {
                    if (d == dist) found = true;
                }
                if (!found) distances.push_back(dist);
            }

            for (std::size_t i = 0; i < distances.size(); ++i) {
                for (std::size_t j = i + 1; j < distances.size(); ++j) {
                    if (distances[j] < distances[i]) {
                        std::swap(distances[i], distances[j]);
                    }
                }
            }

            for (std::size_t i = begin; i < begin + count; ++i) {
                tier_id_t tier = 0;
                for (std::size_t d = 0; d < distances.size(); ++d) {
                    if (distances[d] == valid_nodes[i].second) {
                        tier = static_cast<tier_id_t>(d);
                        break;
                    }
                }

                const int node = valid_nodes[i].first;
                if (!add_domain(tier, static_cast<domain_id_t>(node), node, objects_per_domain, options)) {
                    return false;
                }
            }
            return true;
        }
#endif
        return add_domain(0, 0, 0, objects_per_domain, options);
    }

    /*
    Allocation API
    */
    
    // handle-declaration annotations; missing Evictable falls back to T, then true
    template <std::meta::info Declaration, class... Args>
    std::optional<Handle<T>> allocate(Args&&... args) {
        static_assert(std::meta::is_variable(Declaration), 
            "HotPotato: allocate<...> expects a reflection of a Handle variable");
        constexpr Wherency where = wherency_annotation(Declaration).value_or(Wherency{});
        constexpr bool evictable = evictable_annotation(Declaration).value_or(type_evictable());

        return allocate_impl(where, evictable, std::forward<Args>(args)...);
    }

    // unannotated allocate: likely, Evictable from T if present
    template <class... Args>
    std::optional<Handle<T>> allocate(Args&&... args) {
        return allocate_impl(Wherency{}, type_evictable(), std::forward<Args>(args)...);
    }

    bool deallocate(Handle<T>& h) {
        Record* record = try_get_record(h);
        if (!record) return false;

        if (record->in_lru) lru_remove(h.record_id);
        if (record->ptr) std::destroy_at(record->ptr);
        
        if (DomainState* d = domain_by_id(static_cast<domain_id_t>(record->domain))) {
            release_slot(*d, static_cast<std::size_t>(record->slot_index));
            --d->size;
        }

        record->ptr = nullptr;
        record->live = false;
        record->evictable = true;
        record->in_lru = false;
        ++record->generation;

        free_record_ids_.push_back(h.record_id);
        h = Handle<T>{};
        return true;
    }

    /*
    Handle-specific functions
    */

    // returns the raw pointer and updates LRU
    T* get(const Handle<T>& h) {
        Record* record = try_get_record(h);
        if (!record) return nullptr;
        touch(h.record_id);
        return record->ptr;
    }

    const T* get(const Handle<T>& h) const {
        const Record* record = try_get_record(h);
        if (!record) return nullptr;
        const_cast<TieredPool*>(this)->touch(h.record_id); // have to cast to modify LRU state
                                                            // need to come back if I add thread safety
        return record->ptr;
    }

    std::optional<tier_id_t> get_tier(const Handle<T>& h) const {
        if (const Record* record = try_get_record(h)) return static_cast<tier_id_t>(record->tier);
        return std::nullopt;
    }

    std::optional<domain_id_t> get_domain(const Handle<T>& h) const {
        if (const Record* record = try_get_record(h)) return static_cast<domain_id_t>(record->domain);
        return std::nullopt;
    }

    std::optional<bool> is_evictable(const Handle<T>& h) const {
        if (const Record* record = try_get_record(h)) return static_cast<bool>(record->evictable);
        return std::nullopt;
    }

    // promote a specific object by handle
    bool promote(const Handle<T>& h) {
        Record* record = try_get_record(h);
        return record && record->tier > 0 
            ? migrate_impl(h.record_id, static_cast<tier_id_t>(record->tier) - 1) 
            : false;
    }

    // demote a specific object by handle
    bool demote(const Handle<T>& h) {
        Record* record = try_get_record(h);
        return record && static_cast<tier_id_t>(record->tier) + 1 < tiers_.size() 
            ? migrate_impl(h.record_id, static_cast<tier_id_t>(record->tier) + 1) 
            : false;
    }

    // migrate an object to a specific target tier
    bool migrate(const Handle<T>& h, tier_id_t target_tier) {
        Record* record = try_get_record(h);
        return record && target_tier < tiers_.size() 
            ? migrate_impl(h.record_id, target_tier) 
            : false;
    }

    /*
    Pool state queries
    */

    std::optional<bool> tier_full(tier_id_t tier_id) const {
        if (tier_id >= tiers_.size()) return std::nullopt;
        if (tiers_[tier_id].domains.empty()) return true;
        
        for (domain_id_t domain_id : tiers_[tier_id].domains) {
            const std::optional<bool> full = domain_full(tier_id, domain_id);
            if (full && !*full) return false;
        }
        return true;
    }

    std::optional<bool> domain_full(tier_id_t tier_id, domain_id_t domain_id) const {
        const DomainState* d = domain_by_id(domain_id);
        if (!d || d->tier_id != tier_id) return std::nullopt;
        return d->size == d->capacity;
    }

    const Tier* tier_info(tier_id_t tier_id) const {
        return (tier_id < tiers_.size()) ? &tiers_[tier_id] : nullptr;
    }

    const Domain* domain_info(domain_id_t domain_id) const {
        return domain_by_id(domain_id);
    }

    std::size_t tier_count() const noexcept { return tiers_.size(); }

    static bool numa_supported() noexcept {
#ifdef HOTPOTATO_HAS_NUMA
        return ::numa_available() >= 0;
#else
        return false;
#endif
    }

private:
    static consteval bool type_evictable() {
        return evictable_annotation(^^T).value_or(true);
    }

    template <class... Args>
    std::optional<Handle<T>> allocate_impl(Wherency where, bool evictable, Args&&... args) {
        if (domains_.empty() || tiers_.empty()) return std::nullopt;

        const std::optional<tier_id_t> target_tier = resolve_wherency(where);
        if (!target_tier) return std::nullopt;

        std::optional<SlotReservation> slot = reserve_slot_with_eviction(*target_tier);
        if (!slot) return std::nullopt;

        T* object = ptr_for_slot(*slot->domain, slot->slot_index);
        std::construct_at(object, std::forward<Args>(args)...);

        std::optional<record_id_t> opt_id = acquire_record();
        if (!opt_id) {
            std::destroy_at(object);
            release_slot(*slot->domain, slot->slot_index);
            return std::nullopt;
        }
        
        const record_id_t id = *opt_id;
        Record& record = records_[id];
        record.ptr = object;
        record.tier = *target_tier;
        record.domain = slot->domain->domain_id;
        record.evictable = evictable;
        record.slot_index = slot->slot_index;
        record.live = true;
        record.in_lru = false;

        ++slot->domain->size;

        if (record.evictable) lru_insert_mru(id);

        return Handle<T>{id, record.generation};
    }
    
    static std::optional<std::size_t> align_up(std::size_t value, std::size_t alignment) {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0 || value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
            return std::nullopt;
        }
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static std::optional<std::size_t> checked_multiply(std::size_t a, std::size_t b) {
        if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) return std::nullopt;
        return a * b;
    }

    static bool bind_mapping_to_node([[maybe_unused]] void* addr, [[maybe_unused]] std::size_t bytes, [[maybe_unused]] int numa_node) {
#ifdef HOTPOTATO_HAS_NUMA
        if (!numa_supported()) return true;

        bitmask* mask = numa_allocate_nodemask();
        if (!mask) return false;

        numa_bitmask_clearall(mask);
        numa_bitmask_setbit(mask, static_cast<unsigned int>(numa_node));
        const long rc = ::mbind(addr, bytes, MPOL_BIND, mask->maskp, mask->size, MPOL_MF_STRICT);
        numa_bitmask_free(mask);
        return (rc == 0);
#else
        return true;
#endif
    }

    std::optional<tier_id_t> resolve_wherency(Wherency where) const {
        if (tiers_.empty()) return std::nullopt;

        switch (where.type) {
            case Wherency::kind::likely:     return 0;
            case Wherency::kind::unlikely:   return tiers_.size() - 1;
            case Wherency::kind::exact_tier: return (where.exact < tiers_.size()) ? std::make_optional(where.exact) : std::nullopt;
        }
        return std::nullopt;
    }

    bool ensure_tier_exists(tier_id_t tier_id) {
        if (tier_id > detail::max_record_tier) return false;
        while (tiers_.size() <= tier_id) {
            TierState state;
            state.id = tiers_.size();
            tiers_.push_back(std::move(state));
        }
        return true;
    }

    std::optional<record_id_t> acquire_record() {
        if (!free_record_ids_.empty()) {
            const record_id_t id = free_record_ids_.back();
            free_record_ids_.pop_back();
            return id;
        }
        records_.emplace_back();
        return records_.size() - 1;
    }

    const Record* try_get_record(const Handle<T>& h) const {
        if (h.record_id >= records_.size()) return nullptr;
        const Record& record = records_[h.record_id];
        return (record.live && record.generation == h.generation) ? &record : nullptr;
    }

    Record* try_get_record(const Handle<T>& h) {
        return const_cast<Record*>(static_cast<const TieredPool*>(this)->try_get_record(h));
    }

    const DomainState* domain_by_id(domain_id_t id) const {
        const std::unordered_map<domain_id_t, std::size_t>::const_iterator it = domain_index_.find(id);
        return (it != domain_index_.end()) ? &domains_[it->second] : nullptr;
    }

    DomainState* domain_by_id(domain_id_t id) {
        return const_cast<DomainState*>(static_cast<const TieredPool*>(this)->domain_by_id(id));
    }

    T* ptr_for_slot(DomainState& d, std::size_t slot_index) const noexcept {
        return reinterpret_cast<T*>(static_cast<std::byte*>(d.mapping) + slot_index * d.stride);
    }

    /*
    Pool relocation / management functions
    */

    std::optional<SlotReservation> reserve_free_slot(tier_id_t tier_id) {
        if (tier_id >= tiers_.size()) return std::nullopt;

        for (domain_id_t id : tiers_[tier_id].domains) {
            DomainState* d = domain_by_id(id);
            if (d && !d->free_slots.empty()) {
                const std::size_t slot = d->free_slots.back();
                d->free_slots.pop_back();
                return SlotReservation{d, slot};
            }
        }
        return std::nullopt;
    }

    void release_slot(DomainState& d, std::size_t slot_index) {
        d.free_slots.push_back(slot_index);
    }

    std::optional<SlotReservation> reserve_slot_with_eviction(tier_id_t tier_id) {
        if (std::optional<SlotReservation> slot = reserve_free_slot(tier_id)) return slot;
        return auto_evict_lru(tier_id) ? reserve_free_slot(tier_id) : std::nullopt;
    }

    void lru_insert_mru(record_id_t id) {
        Record& record = records_[id];
        TierState& tier_state = tiers_[static_cast<tier_id_t>(record.tier)];

        record.lru_prev = tier_state.lru_tail;
        record.lru_next = invalid_record;

        if (tier_state.lru_tail != invalid_record) records_[tier_state.lru_tail].lru_next = id;
        else tier_state.lru_head = id;
        
        tier_state.lru_tail = id;
        record.in_lru = true;
    }

    void lru_remove(record_id_t id) {
        Record& record = records_[id];
        if (!record.in_lru) return;
        
        TierState& tier_state = tiers_[static_cast<tier_id_t>(record.tier)];

        if (record.lru_prev != invalid_record) records_[record.lru_prev].lru_next = record.lru_next;
        else tier_state.lru_head = record.lru_next;

        if (record.lru_next != invalid_record) records_[record.lru_next].lru_prev = record.lru_prev;
        else tier_state.lru_tail = record.lru_prev;

        record.lru_prev = invalid_record;
        record.lru_next = invalid_record;
        record.in_lru = false;
    }

    void touch(record_id_t id) {
        Record& record = records_[id];
        if (!record.evictable || !record.in_lru || tiers_[static_cast<tier_id_t>(record.tier)].lru_tail == id) return;
        
        lru_remove(id);
        lru_insert_mru(id);
    }

    std::optional<record_id_t> lru_victim(tier_id_t tier_id) {
        if (tier_id >= tiers_.size() || tiers_[tier_id].lru_head == invalid_record) return std::nullopt;
        return tiers_[tier_id].lru_head;
    }

    bool auto_evict_lru(tier_id_t tier_id) {
        if (tier_id + 1 >= tiers_.size()) return false;
        std::optional<record_id_t> victim = lru_victim(tier_id);
        return victim ? migrate_impl(*victim, tier_id + 1) : false; 
    }

    static void relocate_object(T* dst, T* src) noexcept {
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(static_cast<void*>(dst), static_cast<void*>(src), sizeof(T));
        } else {
            std::construct_at(dst, std::move(*src));
        }
        std::destroy_at(src);
    }

    void move_to_slot(record_id_t id, SlotReservation dst) {
        Record& record = records_[id];
        DomainState* src_domain = domain_by_id(static_cast<domain_id_t>(record.domain));
        assert(src_domain && dst.domain);

        if (record.in_lru) lru_remove(id);

        T* dst_ptr = ptr_for_slot(*dst.domain, dst.slot_index);
        relocate_object(dst_ptr, record.ptr);

        release_slot(*src_domain, static_cast<std::size_t>(record.slot_index));
        --src_domain->size;
        ++dst.domain->size;

        record.ptr = dst_ptr;
        record.tier = dst.domain->tier_id;
        record.domain = dst.domain->domain_id;
        record.slot_index = dst.slot_index;

        if (record.evictable) lru_insert_mru(id);
    }

    bool migrate_impl(record_id_t id, tier_id_t target_tier) {
        Record& record = records_[id];
        const tier_id_t current_tier = static_cast<tier_id_t>(record.tier);
        
        if (current_tier == target_tier) return true;
        std::optional<SlotReservation> dst = reserve_slot_with_eviction(target_tier);
        
        if (!dst) return false; 
        move_to_slot(id, *dst);

        return true;
    }

    std::vector<TierState> tiers_;
    std::vector<DomainState> domains_;
    std::unordered_map<domain_id_t, std::size_t> domain_index_;

    std::vector<Record> records_;
    std::vector<record_id_t> free_record_ids_;
};

} // namespace HotPotato

#endif // HOTPOTATO_TIERED_POOL_HPP
