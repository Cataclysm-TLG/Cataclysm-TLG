#include "item_wakeup.h"

#include <map>

#include "json.h"
#include "type_id.h"

void item_wakeup_manager::schedule_or_update( int64_t /*uid*/, time_point /*when*/,
        item_wakeup_kind /*kind*/, item_locator_hint /*hint*/ )
{
}

void item_wakeup_manager::cancel( int64_t /*uid*/, item_wakeup_kind /*kind*/ )
{
}

void item_wakeup_manager::cancel_all( int64_t /*uid*/ )
{
}

void item_wakeup_manager::rebuild_for_item( item &/*it*/, item_locator_hint /*hint*/ )
{
}

bool item_wakeup_manager::is_scheduled( int64_t /*uid*/, item_wakeup_kind /*kind*/ ) const
{
    return false;
}

std::optional<time_point> item_wakeup_manager::get( int64_t /*uid*/,
        item_wakeup_kind /*kind*/ ) const
{
    return std::nullopt;
}

std::optional<scheduled_wakeup_info> item_wakeup_manager::peek_next_wakeup() const
{
    return std::nullopt;
}

std::vector<scheduled_wakeup_info> item_wakeup_manager::dump() const
{
    return {};
}

item_wakeup_manager::stats item_wakeup_manager::get_stats() const
{
    return stats_;
}

void item_wakeup_manager::process( time_point /*now*/ )
{
}

void item_wakeup_manager::clear()
{
    entries_.clear();
    stats_ = stats{};
}

void item_wakeup_manager::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "version", 1 );
    jsout.member( "wakeups" );
    jsout.start_array();
    jsout.end_array();
    jsout.end_object();
}

void item_wakeup_manager::deserialize( const JsonObject &/*jo*/ )
{
}

#ifdef CATA_TESTS
namespace
{
std::map<itype_id, item_wakeup_test_handler> &test_handler_registry()
{
    static std::map<itype_id, item_wakeup_test_handler> registry;
    return registry;
}
}  // namespace

void register_test_wakeup_handler( const itype_id &id, item_wakeup_test_handler fn )
{
    test_handler_registry()[id] = fn;
}

void clear_test_wakeup_handlers()
{
    test_handler_registry().clear();
}
#endif
