#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bodypart.h"
#include "calendar.h"
#include "cata_utility.h"
#include "character.h"
#include "character_attire.h"
#include "coordinates.h"
#include "debug.h"
#include "effect.h"
#include "map_iterator.h"
#include "messages.h"
#include "morale.h"
#include "pimpl.h"
#include "type_id.h"
#include "units.h"

class item;
struct itype;

static const morale_type morale_perm_fpmode_on( "morale_perm_fpmode_on" );
static const morale_type morale_perm_nomad( "morale_perm_nomad" );

static const trait_id trait_CENOBITE( "CENOBITE" );
static const trait_id trait_NOMAD( "NOMAD" );
static const trait_id trait_NOMAD2( "NOMAD2" );
static const trait_id trait_NOMAD3( "NOMAD3" );

void Character::update_morale()
{
    morale->decay( 1_minutes );
    apply_persistent_morale();
}

void Character::apply_persistent_morale()
{
    // Nomads get a morale penalty if they stay near the same overmap tiles too long.
    if( has_trait( trait_NOMAD ) || has_trait( trait_NOMAD2 ) || has_trait( trait_NOMAD3 ) ) {
        const tripoint_abs_omt ompos = pos_abs_omt();
        float total_time = 0.0f;
        // Check how long we've stayed in any overmap tile within 5 of us.
        const int max_dist = 5;
        for( const tripoint_abs_omt &pos : points_in_radius( ompos, max_dist ) ) {
            const float dist = rl_dist( ompos, pos );
            if( dist > max_dist ) {
                continue;
            }
            const auto iter = overmap_time.find( pos.xy() );
            if( iter == overmap_time.end() ) {
                continue;
            }
            // Count time in own tile fully, tiles one away as 4/5, tiles two away as 3/5, etc.
            total_time += to_moves<float>( iter->second ) * ( max_dist - dist ) / max_dist;
        }
        // Characters with higher tiers of Nomad suffer worse morale penalties, faster.
        int max_unhappiness;
        float min_time;
        float max_time;
        if( has_trait( trait_NOMAD ) ) {
            max_unhappiness = 20;
            min_time = to_moves<float>( 5_days );
            max_time = to_moves<float>( 10_days );
        } else if( has_trait( trait_NOMAD2 ) ) {
            max_unhappiness = 40;
            min_time = to_moves<float>( 60_hours );
            max_time = to_moves<float>( 5_days );
        } else { // traid_NOMAD3
            max_unhappiness = 60;
            min_time = to_moves<float>( 1_days );
            max_time = to_moves<float>( 2_days );
        }
        // The penalty starts at 1 at min_time and scales up to max_unhappiness at max_time.
        const float t = ( total_time - min_time ) / ( max_time - min_time );
        const int pen = std::ceil( lerp_clamped( 0, max_unhappiness, t ) );
        if( pen > 0 ) {
            add_morale( morale_perm_nomad, -pen, -pen, 1_minutes, 1_minutes, true );
        }
    }
}

int Character::get_morale_level() const
{
    return morale->get_level();
}

void Character::add_morale( const morale_type &type, int bonus, int max_bonus,
                            const time_duration &duration, const time_duration &decay_start,
                            bool capped, const itype *item_type )
{
    morale->add( type, bonus, max_bonus, duration, decay_start, capped, item_type );
    if( get_morale_level() <= -25 ) {
        // These two values are used to ding your lifestyle score. Stress is bad for you!
        set_value( "got_to_low_morale", "true" );
    }
    if( get_morale_level() <= -50 ) {
        // These two values are used to ding your lifestyle score. Stress is bad for you!
        set_value( "got_to_very_low_morale", "true" );
    }
}

int Character::has_morale( const morale_type &type ) const
{
    return morale->has( type );
}

void Character::rem_morale( const morale_type &type, const itype *item_type )
{
    morale->remove( type, item_type );
}

void Character::clear_morale()
{
    morale->clear();
}

bool Character::has_morale_to_read() const
{
    return get_morale_level() >= -40;
}

void outfit::check_and_recover_morale( player_morale &test_morale ) const
{
    for( const item &wit : worn ) {
        test_morale.on_item_wear( wit );
    }
}

void Character::check_and_recover_morale()
{
    player_morale test_morale;

    worn.check_and_recover_morale( test_morale );

    for( const trait_id &mut : get_functioning_mutations() ) {
        test_morale.on_mutation_gain( mut );
    }

    for( auto &elem : *effects ) {
        for( std::pair<const bodypart_id, effect> &_effect_it : elem.second ) {
            const effect &e = _effect_it.second;
            test_morale.on_effect_int_change( e.get_id(), e.get_intensity(), e.get_bp() );
        }
    }

    test_morale.on_stat_change( "hunger", get_hunger() );
    test_morale.on_stat_change( "thirst", get_thirst() );
    test_morale.on_stat_change( "fatigue", get_fatigue() );
    test_morale.on_stat_change( "pain", get_pain() );
    test_morale.on_stat_change( "pkill", get_painkiller() );
    test_morale.on_stat_change( "perceived_pain", get_perceived_pain() );
    test_morale.on_stat_change( "radiation", get_rad() );

    apply_persistent_morale();

    if( !morale->consistent_with( test_morale ) ) {
        *morale = player_morale( test_morale ); // Recover consistency
        add_msg_debug( debugmode::DF_CHARACTER, "%s morale was recovered.", disp_name( true ) );
    }
}

void Character::disp_morale()
{
    int equilibrium = calc_focus_equilibrium();

    int fatigue_penalty = 0;
    const int fatigue_cap = focus_equilibrium_fatigue_cap( equilibrium );

    if( fatigue_cap < equilibrium ) {
        fatigue_penalty = equilibrium - fatigue_cap;
        equilibrium = fatigue_cap;
    }

    int pain_penalty = 0;
    if( get_perceived_pain() && !has_trait( trait_CENOBITE ) ) {
        pain_penalty = calc_focus_equilibrium( true ) - equilibrium - fatigue_penalty;
    }

    morale->display( equilibrium, pain_penalty, fatigue_penalty );
}
