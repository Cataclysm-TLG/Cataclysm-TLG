#include "item_wakeup.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include "cata_assert.h"
#include "character_id.h"
#include "debug.h"
#include "flexbuffer_json.h"
#include "item.h"
#include "item_location.h"
#include "item_uid.h"
#include "json.h"
#include "type_id.h"

namespace
{

constexpr int CURRENT_VERSION = 1;

// Stale dispatches before a single noise-control debugmsg fires.
// Reset by clear() and deserialize().
constexpr int STALE_DEBUGMSG_THRESHOLD = 32;

const char *kind_to_string( item_wakeup_kind k )
{
    switch( k ) {
        case item_wakeup_kind::alarm:
            return "alarm";
        case item_wakeup_kind::ready_check:
            return "ready_check";
        case item_wakeup_kind::fail_check:
            return "fail_check";
        case item_wakeup_kind::last:
            break;
    }
    return "invalid";
}

bool string_to_kind( const std::string &s, item_wakeup_kind &out )
{
    if( s == "alarm" ) {
        out = item_wakeup_kind::alarm;
        return true;
    }
    if( s == "ready_check" ) {
        out = item_wakeup_kind::ready_check;
        return true;
    }
    if( s == "fail_check" ) {
        out = item_wakeup_kind::fail_check;
        return true;
    }
    return false;
}

const char *place_to_string( item_locator_hint::place p )
{
    switch( p ) {
        case item_locator_hint::place::map:
            return "map";
        case item_locator_hint::place::vehicle:
            return "vehicle";
        case item_locator_hint::place::character:
            return "character";
        case item_locator_hint::place::unknown:
            break;
    }
    return "unknown";
}

bool string_to_place( const std::string &s, item_locator_hint::place &out )
{
    if( s == "map" ) {
        out = item_locator_hint::place::map;
        return true;
    }
    if( s == "vehicle" ) {
        out = item_locator_hint::place::vehicle;
        return true;
    }
    if( s == "character" ) {
        out = item_locator_hint::place::character;
        return true;
    }
    if( s == "unknown" ) {
        out = item_locator_hint::place::unknown;
        return true;
    }
    return false;
}

void serialize_hint( JsonOut &jsout, const item_locator_hint &hint )
{
    jsout.start_object();
    jsout.member( "where", place_to_string( hint.where ) );
    if( hint.where == item_locator_hint::place::map ) {
        const tripoint_abs_ms *p = std::get_if<tripoint_abs_ms>( &hint.location );
        if( p != nullptr ) {
            jsout.member( "abs_ms", *p );
        }
    } else if( hint.where == item_locator_hint::place::vehicle ) {
        const vehicle_hint *v = std::get_if<vehicle_hint>( &hint.location );
        if( v != nullptr ) {
            jsout.member( "cargo_square", v->cargo_square );
            jsout.member( "part_index", v->part_index );
            jsout.member( "mount_offset", v->mount_offset );
        }
    } else if( hint.where == item_locator_hint::place::character ) {
        const character_id *c = std::get_if<character_id>( &hint.location );
        if( c != nullptr ) {
            jsout.member( "character", c->get_value() );
        }
    }
    jsout.end_object();
}

bool deserialize_hint( const JsonObject &jo, item_locator_hint &hint )
{
    std::string where_str;
    if( !jo.read( "where", where_str ) ) {
        return false;
    }
    if( !string_to_place( where_str, hint.where ) ) {
        return false;
    }
    if( hint.where == item_locator_hint::place::map ) {
        tripoint_abs_ms p;
        if( !jo.read( "abs_ms", p ) ) {
            hint.where = item_locator_hint::place::unknown;
            hint.location = std::monostate{};
            return true;
        }
        hint.location = p;
    } else if( hint.where == item_locator_hint::place::vehicle ) {
        vehicle_hint v;
        if( !jo.read( "cargo_square", v.cargo_square ) ) {
            hint.where = item_locator_hint::place::unknown;
            hint.location = std::monostate{};
            return true;
        }
        jo.read( "part_index", v.part_index );
        jo.read( "mount_offset", v.mount_offset );
        hint.location = v;
    } else if( hint.where == item_locator_hint::place::character ) {
        int64_t cid = 0;
        if( !jo.read( "character", cid ) ) {
            hint.where = item_locator_hint::place::unknown;
            hint.location = std::monostate{};
            return true;
        }
        hint.location = character_id( static_cast<int>( cid ) );
    } else {
        hint.location = std::monostate{};
    }
    return true;
}

std::map<itype_id, item_wakeup_test_handler> &test_handler_registry()
{
    static std::map<itype_id, item_wakeup_test_handler> registry;
    return registry;
}

std::map<itype_id, item_wakeup_test_enumerator> &test_enumerator_registry()
{
    static std::map<itype_id, item_wakeup_test_enumerator> registry;
    return registry;
}

bool processing_active = false;

}  // namespace

void register_test_wakeup_handler( const itype_id &id, item_wakeup_test_handler fn )
{
    test_handler_registry()[id] = fn;
}

void clear_test_wakeup_handlers()
{
    test_handler_registry().clear();
}

void register_test_enumerate_handler( const itype_id &id, item_wakeup_test_enumerator fn )
{
    test_enumerator_registry()[id] = fn;
}

void clear_test_enumerate_handlers()
{
    test_enumerator_registry().clear();
}

std::vector<desired_wakeup> enumerate_scheduled_dispatch( const item &it )
{
    const std::map<itype_id, item_wakeup_test_enumerator> &reg = test_enumerator_registry();
    const auto entry = reg.find( it.typeId() );
    if( entry != reg.end() ) {
        return entry->second( it );
    }
    return {};
}

// Dispatcher called from item::actualize_scheduled.
static void dispatch_actualize( item &it, item_wakeup_kind kind, time_point now )
{
    const std::map<itype_id, item_wakeup_test_handler> &reg = test_handler_registry();
    const auto it_handler = reg.find( it.typeId() );
    if( it_handler != reg.end() ) {
        it_handler->second( it, kind, now );
    }
}

void item_wakeup_manager::schedule_or_update( int64_t uid, time_point when,
        item_wakeup_kind kind, item_locator_hint hint )
{
    auto match = std::find_if( entries_.begin(), entries_.end(),
    [uid, kind]( const entry & e ) {
        return e.uid == uid && e.kind == kind;
    } );
    if( match != entries_.end() ) {
        match->when = when;
        match->hint = hint;
        return;
    }
    entry e;
    e.uid = uid;
    e.kind = kind;
    e.when = when;
    e.hint = hint;
    entries_.push_back( e );
}

void item_wakeup_manager::cancel( int64_t uid, item_wakeup_kind kind )
{
    entries_.erase( std::remove_if( entries_.begin(), entries_.end(),
    [uid, kind]( const entry & e ) {
        return e.uid == uid && e.kind == kind;
    } ), entries_.end() );
}

void item_wakeup_manager::cancel_all( int64_t uid )
{
    entries_.erase( std::remove_if( entries_.begin(), entries_.end(),
    [uid]( const entry & e ) {
        return e.uid == uid;
    } ), entries_.end() );
}

void item_wakeup_manager::rebuild_for_item( item &it, item_locator_hint hint )
{
    const int64_t uid = it.uid().get_value();
    const std::vector<desired_wakeup> desired = it.enumerate_scheduled_wakeups();

    // Cancel kinds the item no longer wants.
    entries_.erase( std::remove_if( entries_.begin(), entries_.end(),
    [uid, &desired]( const entry & e ) {
        if( e.uid != uid ) {
            return false;
        }
        for( const desired_wakeup &d : desired ) {
            if( d.kind == e.kind ) {
                return false;
            }
        }
        return true;
    } ), entries_.end() );

    // Add or update each desired wakeup.
    for( const desired_wakeup &d : desired ) {
        schedule_or_update( uid, d.when, d.kind, hint );
    }
}

bool item_wakeup_manager::is_scheduled( int64_t uid, item_wakeup_kind kind ) const
{
    return std::any_of( entries_.begin(), entries_.end(),
    [uid, kind]( const entry & e ) {
        return e.uid == uid && e.kind == kind;
    } );
}

std::optional<time_point> item_wakeup_manager::get( int64_t uid,
        item_wakeup_kind kind ) const
{
    auto match = std::find_if( entries_.begin(), entries_.end(),
    [uid, kind]( const entry & e ) {
        return e.uid == uid && e.kind == kind;
    } );
    if( match == entries_.end() ) {
        return std::nullopt;
    }
    return match->when;
}

std::optional<scheduled_wakeup_info> item_wakeup_manager::peek_next_wakeup() const
{
    std::vector<scheduled_wakeup_info> all = dump();
    if( all.empty() ) {
        return std::nullopt;
    }
    return all.front();
}

std::vector<scheduled_wakeup_info> item_wakeup_manager::dump() const
{
    std::vector<scheduled_wakeup_info> out;
    out.reserve( entries_.size() );
    for( const entry &e : entries_ ) {
        scheduled_wakeup_info info;
        info.uid = e.uid;
        info.kind = e.kind;
        info.when = e.when;
        info.hint = e.hint;
        out.push_back( info );
    }
    std::sort( out.begin(), out.end(),
    []( const scheduled_wakeup_info & a, const scheduled_wakeup_info & b ) {
        if( a.when != b.when ) {
            return a.when < b.when;
        }
        if( a.uid != b.uid ) {
            return a.uid < b.uid;
        }
        return static_cast<int>( a.kind ) < static_cast<int>( b.kind );
    } );
    return out;
}

item_wakeup_manager::stats item_wakeup_manager::get_stats() const
{
    stats s = stats_;
    s.total_pending = static_cast<int>( entries_.size() );
    return s;
}

void item_wakeup_manager::process( time_point now )
{
    cata_assert( !processing_active );
    processing_active = true;

    // Snapshot expired entries in deterministic order, erase from live queue.
    std::vector<entry> snapshot;
    snapshot.reserve( entries_.size() );
    auto first_pending = std::partition( entries_.begin(), entries_.end(),
    [now]( const entry & e ) {
        return e.when > now;
    } );
    snapshot.assign( first_pending, entries_.end() );
    entries_.erase( first_pending, entries_.end() );
    std::sort( snapshot.begin(), snapshot.end(),
    []( const entry & a, const entry & b ) {
        if( a.when != b.when ) {
            return a.when < b.when;
        }
        if( a.uid != b.uid ) {
            return a.uid < b.uid;
        }
        return static_cast<int>( a.kind ) < static_cast<int>( b.kind );
    } );

    // Dispatch each.  Stale uids increment the counter; threshold breach
    // emits a single debugmsg per manager lifetime (reset by clear()).
    for( const entry &e : snapshot ) {
        item_location loc = find_item_by_uid( e.uid, e.hint );
        if( !loc ) {
            stats_.stale_resolution++;
            stale_seen_uids_++;
            if( stale_seen_uids_ >= STALE_DEBUGMSG_THRESHOLD && !stale_msg_emitted_ ) {
                debugmsg( "item_wakeup_manager: %d stale uids unresolved",
                          stale_seen_uids_ );
                stale_msg_emitted_ = true;
            }
            continue;
        }
        loc->actualize_scheduled( e.kind, now );
    }

    processing_active = false;
}

void item_wakeup_manager::clear()
{
    entries_.clear();
    stats_ = stats{};
    stale_seen_uids_ = 0;
    stale_msg_emitted_ = false;
}

void item_wakeup_manager::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "version", CURRENT_VERSION );
    jsout.member( "wakeups" );
    jsout.start_array();
    std::vector<entry> sorted = entries_;
    std::sort( sorted.begin(), sorted.end(),
    []( const entry & a, const entry & b ) {
        if( a.when != b.when ) {
            return a.when < b.when;
        }
        if( a.uid != b.uid ) {
            return a.uid < b.uid;
        }
        return static_cast<int>( a.kind ) < static_cast<int>( b.kind );
    } );
    for( const entry &e : sorted ) {
        jsout.start_object();
        jsout.member( "uid", e.uid );
        jsout.member( "kind", kind_to_string( e.kind ) );
        jsout.member( "when", e.when );
        jsout.member( "hint" );
        serialize_hint( jsout, e.hint );
        jsout.end_object();
    }
    jsout.end_array();
    jsout.end_object();
}

void item_wakeup_manager::deserialize( const JsonObject &jo )
{
    entries_.clear();
    stats_ = stats{};
    stale_seen_uids_ = 0;
    stale_msg_emitted_ = false;

    int version = 0;
    if( !jo.read( "version", version ) ) {
        debugmsg( "item_wakeup_manager: save without version, treating as v1" );
    } else if( version != CURRENT_VERSION ) {
        debugmsg( "item_wakeup_manager: unknown save version %d, dropping queue",
                  version );
        return;
    }

    if( !jo.has_array( "wakeups" ) ) {
        return;
    }

    std::map<std::pair<int64_t, item_wakeup_kind>, entry> dedup;
    int dropped = 0;
    for( JsonObject row : jo.get_array( "wakeups" ) ) {
        // Consume every field unconditionally so the strict JSON parser
        // does not flag unread members on bad entries.
        row.allow_omitted_members();
        int64_t uid = 0;
        std::string kind_str;
        time_point when;
        item_wakeup_kind kind = item_wakeup_kind::alarm;
        const bool got_uid = row.read( "uid", uid );
        const bool got_kind = row.read( "kind", kind_str );
        const bool got_when = row.read( "when", when );
        item_locator_hint hint;
        if( row.has_object( "hint" ) ) {
            JsonObject hint_obj = row.get_object( "hint" );
            if( !deserialize_hint( hint_obj, hint ) ) {
                hint = item_locator_hint{};
            }
        }
        if( !got_uid || uid <= 0 || !got_kind || !string_to_kind( kind_str, kind )
            || !got_when ) {
            dropped++;
            continue;
        }
        entry e;
        e.uid = uid;
        e.kind = kind;
        e.when = when;
        e.hint = hint;
        // Dedupe by (uid, kind), keeping the latest when.
        auto key = std::make_pair( uid, kind );
        auto existing = dedup.find( key );
        if( existing == dedup.end() ) {
            dedup[key] = e;
        } else {
            stats_.duplicate_event++;
            if( existing->second.when < when ) {
                dedup[key] = e;
            }
        }
    }
    entries_.reserve( dedup.size() );
    for( const auto &kv : dedup ) {
        entries_.push_back( kv.second );
    }
    stats_.dropped_on_load = dropped;
}

// Item-side dispatch entry called from item.cpp.
void actualize_scheduled_dispatch( item &it, item_wakeup_kind kind, time_point now )
{
    dispatch_actualize( it, kind, now );
}
