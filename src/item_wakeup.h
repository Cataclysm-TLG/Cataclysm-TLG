#pragma once
#ifndef CATA_SRC_ITEM_WAKEUP_H
#define CATA_SRC_ITEM_WAKEUP_H

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "calendar.h"
#include "character_id.h"
#include "coordinates.h"
#include "point.h"
#include "type_id.h"

class JsonObject;
class JsonOut;
class item;

// Categorizes what a scheduled wakeup is for.  Distinct kinds for one item
// coexist independently.
enum class item_wakeup_kind : uint8_t {
    alarm = 0,
    ready_check,
    fail_check,
    last
};

// Persisted vehicle hint.  vpart_reference is a runtime handle and must
// not appear on disk; this struct stores only stable values.  The cargo
// square is authoritative; part_index/mount_offset are fallback hints
// that may be stale after vehicle moves or repairs.
struct vehicle_hint {
    tripoint_abs_ms cargo_square;
    int part_index = -1;
    point_rel_ms mount_offset;
};

// Hint for resolving an item by uid.  Drives the lookup order in
// find_item_by_uid; never the source of truth for item identity.
struct item_locator_hint {
    enum class place : uint8_t { map, vehicle, character, unknown };
    place where = place::unknown;
    std::variant<std::monostate, tripoint_abs_ms, vehicle_hint, character_id> location;

    item_locator_hint() = default;
};

// Producer record returned by item::enumerate_scheduled_wakeups.  Items
// describe their desired wakeups by (kind, when); the manager stamps the
// caller-supplied hint when (re)scheduling.
struct desired_wakeup {
    item_wakeup_kind kind;
    time_point when;
};

// Diagnostics row.
struct scheduled_wakeup_info {
    int64_t uid;
    item_wakeup_kind kind;
    time_point when;
    item_locator_hint hint;
};

// Item-targeted wakeup scheduler.  Items remain authoritative for runtime
// state; this queue is advisory and can be reconstructed via
// rebuild_for_item from item state.
class item_wakeup_manager
{
    public:
        struct stats {
            int total_pending = 0;
            int stale_resolution = 0;
            int duplicate_event = 0;
            int dropped_on_load = 0;
        };

        // Insert or replace the (uid, kind) entry.  Newer when always wins.
        void schedule_or_update( int64_t uid, time_point when,
                                 item_wakeup_kind kind, item_locator_hint hint );
        void cancel( int64_t uid, item_wakeup_kind kind );
        void cancel_all( int64_t uid );

        // Reconcile the manager's queue for a single item against the item's
        // own enumerate_scheduled_wakeups().  Items the manager has but the
        // item no longer wants are cancelled; new desires are scheduled.
        // Idempotent.  Hint is stamped onto every (re)scheduled entry.
        void rebuild_for_item( item &it, item_locator_hint hint );

        bool is_scheduled( int64_t uid, item_wakeup_kind kind ) const;
        std::optional<time_point> get( int64_t uid, item_wakeup_kind kind ) const;
        std::optional<scheduled_wakeup_info> peek_next_wakeup() const;
        // Sorted by (when, uid, kind).
        std::vector<scheduled_wakeup_info> dump() const;
        stats get_stats() const;

        // Two-phase, non-reentrant.  Snapshot all entries with when <= now,
        // erase from live queue, then dispatch handlers from the snapshot.
        // Recursion guard asserts process() is not called from a handler.
        // Wakeups newly scheduled for <= now during a handler do NOT fire
        // again in the same pass.
        void process( time_point now );

        // Test isolation only.  Drops every entry and zeroes stats.
        void clear();

        void serialize( JsonOut &jsout ) const;
        void deserialize( const JsonObject &jo );

    private:
        struct entry {
            int64_t uid = 0;
            item_wakeup_kind kind = item_wakeup_kind::alarm;
            time_point when = calendar::before_time_starts;
            item_locator_hint hint;
        };

        std::vector<entry> entries_;
        stats stats_;
};

// Game-owned accessor.
item_wakeup_manager &get_item_wakeups();

// find_item_by_uid is declared in item_location.h.

// Test-only handler registry.  Production builds compile this out.
#ifdef CATA_TESTS
using item_wakeup_test_handler =
    void( * )( item &it, item_wakeup_kind kind, time_point now );
void register_test_wakeup_handler( const itype_id &id, item_wakeup_test_handler fn );
void clear_test_wakeup_handlers();
#endif

#endif // CATA_SRC_ITEM_WAKEUP_H
