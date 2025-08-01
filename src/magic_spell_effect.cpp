#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iosfwd>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "character.h"
#include "character_martial_arts.h"
#include "colony.h"
#include "color.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "damage.h"
#include "debug.h"
#include "effect_on_condition.h"
#include "enums.h"
#include "explosion.h"
#include "field.h"
#include "field_type.h"
#include "flag.h"
#include "fungal_effects.h"
#include "game.h"
#include "item.h"
#include "kill_tracker.h"
#include "line.h"
#include "magic.h"
#include "magic_spell_effect_helpers.h"
#include "magic_teleporter_list.h"
#include "magic_ter_furn_transform.h"
#include "map.h"
#include "map_iterator.h"
#include "messages.h"
#include "mongroup.h"
#include "monster.h"
#include "monstergenerator.h"
#include "morale.h"
#include "mtype.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "pimpl.h"
#include "point.h"
#include "projectile.h"
#include "ret_val.h"
#include "rng.h"
#include "string_formatter.h"
#include "teleport.h"
#include "timed_event.h"
#include "translations.h"
#include "type_id.h"
#include "units.h"
#include "vehicle.h"
#include "vpart_position.h"

static const damage_type_id damage_acid( "acid" );

static const efftype_id effect_airborne( "airborne" );
static const efftype_id effect_corroding( "corroding" );
static const efftype_id effect_jumping( "jumping" );
static const efftype_id effect_invisibility( "invisibility" );
static const efftype_id effect_teleglow( "teleglow" );

static const flag_id json_flag_FIT( "FIT" );

static const json_character_flag json_flag_ETHEREAL( "ETHEREAL" );
static const json_character_flag json_flag_PRED1( "PRED1" );
static const json_character_flag json_flag_PRED2( "PRED2" );
static const json_character_flag json_flag_PRED3( "PRED3" );
static const json_character_flag json_flag_PRED4( "PRED4" );

static const morale_type morale_killed_monster( "morale_killed_monster" );
static const morale_type morale_pyromania_nofire( "morale_pyromania_nofire" );
static const morale_type morale_pyromania_startfire( "morale_pyromania_startfire" );

static const mtype_id mon_blob( "mon_blob" );
static const mtype_id mon_blob_brain( "mon_blob_brain" );
static const mtype_id mon_blob_large( "mon_blob_large" );
static const mtype_id mon_blob_small( "mon_blob_small" );
static const mtype_id mon_generator( "mon_generator" );

static const species_id species_HALLUCINATION( "HALLUCINATION" );
static const species_id species_SLIME( "SLIME" );

static const trait_id trait_PACIFIST( "PACIFIST" );
static const trait_id trait_PSYCHOPATH( "PSYCHOPATH" );
static const trait_id trait_PYROMANIA( "PYROMANIA" );

namespace spell_detail
{
struct line_iterable {
    const std::vector<point> &delta_line;
    point cur_origin;
    point delta;
    size_t index;

    line_iterable( const point &origin, const point &delta, const std::vector<point> &dline )
        : delta_line( dline ), cur_origin( origin ), delta( delta ), index( 0 ) {}

    point get() const {
        return cur_origin + delta_line[index];
    }
    // Move forward along point set, wrap around and move origin forward if necessary
    void next() {
        index = ( index + 1 ) % delta_line.size();
        cur_origin = cur_origin + delta * ( index == 0 );
    }
    // Move back along point set, wrap around and move origin backward if necessary
    void prev() {
        cur_origin = cur_origin - delta * ( index == 0 );
        index = ( index + delta_line.size() - 1 ) % delta_line.size();
    }
    void reset( const point &origin ) {
        cur_origin = origin;
        index = 0;
    }
};
// Orientation of point C relative to line AB
static int side_of( const point &a, const point &b, const point &c )
{
    int cross = ( b.x - a.x ) * ( c.y - a.y ) - ( b.y - a.y ) * ( c.x - a.x );
    return ( cross > 0 ) - ( cross < 0 );
}
// Tests if point c is between or on lines (a0, a0 + d) and (a1, a1 + d)
static bool between_or_on( const point &a0, const point &a1, const point &d, const point &c )
{
    return side_of( a0, a0 + d, c ) != 1 && side_of( a1, a1 + d, c ) != -1;
}
// Builds line until obstructed or outside of region bound by near and far lines. Stores result in set
static void build_line( spell_detail::line_iterable line, const tripoint_bub_ms &source,
                        const point &delta, const point &delta_perp, bool ( *test )( const tripoint_bub_ms & ),
                        std::set<tripoint_bub_ms> &result )
{
    while( between_or_on( point_zero, delta, delta_perp, line.get() ) ) {
        if( !test( source + line.get() ) ) {
            break;
        }
        result.emplace( source + line.get() );
        line.next();
    }
}
} // namespace spell_detail

void spell_effect::short_range_teleport( const spell &sp, Creature &caster,
        const tripoint_bub_ms &target )
{
    const bool safe = !sp.has_flag( spell_flag::UNSAFE_TELEPORT );
    const bool target_teleport = sp.has_flag( spell_flag::TARGET_TELEPORT );
    if( target_teleport ) {
        if( sp.aoe( caster ) == 0 ) {
            teleport::teleport_to_point( caster, target, safe, false );
            return;
        }

        std::set<tripoint_bub_ms> potential_targets = calculate_spell_effect_area( sp, target, caster );
        tripoint_bub_ms where = random_entry( potential_targets );
        teleport::teleport_to_point( caster, where, safe, false );
        return;
    }
    const int min_distance = sp.range( caster );
    const int max_distance = sp.range( caster ) + sp.aoe( caster );
    if( min_distance > max_distance || min_distance < 0 || max_distance < 0 ) {
        debugmsg( "ERROR: Teleport argument(s) invalid" );
        return;
    }
    teleport::teleport( caster, min_distance, max_distance, safe, false );
}

static void swap_pos( Creature &caster, const tripoint_bub_ms &target )
{
    Creature *const critter = get_creature_tracker().creature_at<Creature>( target );
    critter->setpos( caster.pos() );
    caster.setpos( target );

    //update map in case a monster swapped positions with the player
    Character &you = get_player_character();
    get_map().vertical_shift( you.posz() );
    g->update_map( you );
}

void spell_effect::pain_split( const spell &sp, Creature &caster, const tripoint_bub_ms & )
{
    Character *you = caster.as_character();
    if( you == nullptr ) {
        return;
    }
    sp.make_sound( caster.pos_bub(), caster );
    add_msg( m_info, _( "Your injuries even out." ) );
    int num_limbs = 0; // number of limbs effected (broken don't count)
    int total_hp = 0; // total hp among limbs

    for( const std::pair<const bodypart_str_id, bodypart> &elem : you->get_body() ) {
        if( elem.first == bodypart_str_id::NULL_ID() ) {
            continue;
        }
        num_limbs++;
        total_hp += elem.second.get_hp_cur();
    }
    const int hp_each = total_hp / num_limbs;
    you->set_all_parts_hp_cur( hp_each );
}

static bool in_spell_aoe( const tripoint_bub_ms &start, const tripoint_bub_ms &end,
                          const int &radius, const bool ignore_walls )
{
    if( rl_dist( start, end ) > radius ) {
        return false;
    }
    if( ignore_walls ) {
        return true;
    }
    if( start.x() == end.x() && start.y() == end.y() && start.z() == end.z() ) {
        return true;
    }
    map &here = get_map();
    const std::vector<tripoint_bub_ms> trajectory = line_to( start, end );
    for( const tripoint_bub_ms &pt : trajectory ) {
        if( here.coverage( pt ) > 0 && rng( 1, 100 ) < here.coverage( pt ) ) {
            return false;
        }
    }
    return true;
}

std::set<tripoint_bub_ms> spell_effect::spell_effect_blast( const override_parameters &params,
        const tripoint_bub_ms &, const tripoint_bub_ms &target )
{
    std::set<tripoint_bub_ms> targets;
    // TODO: Make this breadth-first
    for( const tripoint_bub_ms &potential_target : get_map().points_in_radius( target,
            params.aoe_radius ) ) {
        if( in_spell_aoe( target, potential_target, params.aoe_radius, params.ignore_walls ) ) {
            targets.emplace( potential_target );
        }
    }
    return targets;
}

static std::set<tripoint_bub_ms> spell_effect_cone_range_override(
    const spell_effect::override_parameters &params, const tripoint_bub_ms &source,
    const tripoint_bub_ms &target )
{
    std::set<tripoint_bub_ms> targets;
    const units::angle initial_angle = coord_to_angle( source.raw(), target.raw() );
    const units::angle half_width = units::from_degrees( params.aoe_radius / 2.0 );
    const units::angle start_angle = initial_angle - half_width;
    const units::angle end_angle = initial_angle + half_width;
    std::set<tripoint_bub_ms> end_points;
    for( units::angle angle = start_angle; angle <= end_angle; angle += 1_degrees ) {
        for( int range = 1; range <= params.range; range++ ) {
            tripoint potential;
            calc_ray_end( angle, range, source.raw(), potential );
            if( params.ignore_walls ) {
                targets.emplace( potential );
            } else {
                end_points.emplace( potential );
            }
        }
    }
    if( !params.ignore_walls ) {
        map &here = get_map();
        for( const tripoint_bub_ms &ep : end_points ) {
            std::vector<tripoint_bub_ms> trajectory = line_to( source, ep );
            for( const tripoint_bub_ms &tp : trajectory ) {
                if( here.coverage( tp ) == 0 || rng( 1, 100 ) > here.coverage( tp ) ) {
                    targets.emplace( tp );
                } else {
                    break;
                }
            }
        }
    }
    // we don't want to hit ourselves in the blast!
    targets.erase( source );
    return targets;
}

std::set<tripoint_bub_ms> spell_effect::spell_effect_cone( const override_parameters &params,
        const tripoint_bub_ms &source, const tripoint_bub_ms &target )
{
    // cones go all the way to end (if they don't hit an obstacle)
    return spell_effect_cone_range_override( params, source, target );
}

static bool test_always_true( const tripoint_bub_ms & )
{
    return true;
}

static bool test_coverage( const tripoint_bub_ms &p )
{
    return get_map().coverage( p ) == 0 || rng( 1, 100 ) > get_map().coverage( p );
}

std::set<tripoint_bub_ms> spell_effect::spell_effect_line( const override_parameters &params,
        const tripoint_bub_ms &source, const tripoint_bub_ms &target )
{
    const point delta = ( target - source ).xy().raw();
    const int dist = square_dist( point_zero, delta );
    // Early out to prevent unnecessary calculations
    if( dist == 0 ) {
        return std::set<tripoint_bub_ms>();
    }
    // Clockwise Perpendicular of Delta vector
    const point delta_perp( -delta.y, delta.x );

    const point abs_delta = delta.abs();
    // Primary axis of delta vector
    const point axis_delta = abs_delta.x > abs_delta.y ? point( delta.x, 0 ) : point( 0, delta.y );
    // Clockwise Perpendicular of axis vector
    const point cw_perp_axis( -axis_delta.y, axis_delta.x );
    const point unit_cw_perp_axis( sgn( cw_perp_axis.x ), sgn( cw_perp_axis.y ) );
    // bias leg length toward cw side if uneven
    int ccw_len = params.aoe_radius / 2;
    int cw_len = params.aoe_radius - ccw_len;

    // is delta aligned with, cw, or ccw of primary axis
    int delta_side = spell_detail::side_of( point_zero, axis_delta, delta );

    bool ( *test )( const tripoint_bub_ms & ) = params.ignore_walls ? test_always_true : test_coverage;

    // Canonical path from source to target, offset to local space
    std::vector<point> path_to_target = line_to( point_zero, delta );
    // Remove endpoint,
    path_to_target.pop_back();
    // and insert startpoint. Path is now prepared for wrapped iteration
    path_to_target.insert( path_to_target.begin(), point_zero );

    spell_detail::line_iterable base_line( point_zero, delta, path_to_target );

    std::set<tripoint_bub_ms> result;

    // Add midline points (source -> target )
    spell_detail::build_line( base_line, source, delta, delta_perp, test, result );

    // Add cw and ccw legs
    if( delta_side == 0 ) { // delta is already axis aligned, only need straight lines
        // cw leg
        for( const point &p : line_to( point_zero, unit_cw_perp_axis * cw_len ) ) {
            base_line.reset( p );
            if( !test( source + p ) ) {
                break;
            }

            spell_detail::build_line( base_line, source, delta, delta_perp, test, result );
        }
        // ccw leg
        for( const point &p : line_to( point_zero, unit_cw_perp_axis * -ccw_len ) ) {
            base_line.reset( p );
            if( !test( source + p ) ) {
                break;
            }

            spell_detail::build_line( base_line, source, delta, delta_perp, test, result );
        }
    } else if( delta_side == 1 ) { // delta is cw of primary axis
        // ccw leg is behind perp axis
        for( const point &p : line_to( point_zero, unit_cw_perp_axis * -ccw_len ) ) {
            base_line.reset( p );

            // forward until in
            while( spell_detail::side_of( point_zero, delta_perp, base_line.get() ) == 1 ) {
                base_line.next();
            }
            if( !test( source + p ) ) {
                break;
            }
            spell_detail::build_line( base_line, source, delta, delta_perp, test, result );
        }
        // cw leg is before perp axis
        for( const point &p : line_to( point_zero, unit_cw_perp_axis * cw_len ) ) {
            base_line.reset( p );

            // move back
            while( spell_detail::side_of( point_zero, delta_perp, base_line.get() ) != 1 ) {
                base_line.prev();
            }
            base_line.next();
            if( !test( source + p ) ) {
                break;
            }
            spell_detail::build_line( base_line, source, delta, delta_perp, test, result );
        }
    } else if( delta_side == -1 ) { // delta is ccw of primary axis
        // ccw leg is before perp axis
        for( const point &p : line_to( point_zero, unit_cw_perp_axis * -ccw_len ) ) {
            base_line.reset( p );

            // move back
            while( spell_detail::side_of( point_zero, delta_perp, base_line.get() ) != 1 ) {
                base_line.prev();
            }
            base_line.next();
            if( !test( source + p ) ) {
                break;
            }
            spell_detail::build_line( base_line, source, delta, delta_perp, test, result );
        }
        // cw leg is behind perp axis
        for( const point &p : line_to( point_zero, unit_cw_perp_axis * cw_len ) ) {
            base_line.reset( p );

            // forward until in
            while( spell_detail::side_of( point_zero, delta_perp, base_line.get() ) == 1 ) {
                base_line.next();
            }
            if( !test( source + p ) ) {
                break;
            }
            spell_detail::build_line( base_line, source, delta, delta_perp, test, result );
        }
    }

    result.erase( source );
    return result;
}

// spells do not reduce in damage the further away from the epicenter the targets are
// rather they do their full damage in the entire area of effect
std::set<tripoint_bub_ms> calculate_spell_effect_area( const spell &sp,
        const tripoint_bub_ms &target, const Creature &caster )
{
    // the actual target that the spell will hit.
    tripoint_bub_ms epicenter( target );
    // stop short if we hit a wall, if the spell has a projectile
    if( sp.shape() == spell_shape::blast && !sp.has_flag( spell_flag::NO_PROJECTILE ) ) {
        map &here = get_map();
        std::vector<tripoint_bub_ms> trajectory = line_to( caster.pos_bub(), target );
        for( std::vector<tripoint_bub_ms>::iterator iter = trajectory.begin(); iter != trajectory.end();
             iter++ ) {
            if( here.impassable( *iter ) ) {
                if( iter != trajectory.begin() ) {
                    epicenter = *( iter - 1 );
                } else {
                    epicenter = *iter;
                }
                break;
            }
        }
    }

    std::set<tripoint_bub_ms> targets = { tripoint_bub_ms( epicenter ) }; // initialize with epicenter
    // TODO: Why is this even here?
    if( sp.aoe( caster ) < 1 && ( sp.shape() != spell_shape::line &&
                                  sp.shape() != spell_shape::blast ) ) {
        return targets;
    }

    targets = sp.effect_area( caster.pos_bub(), target, caster );
    for( std::set<tripoint_bub_ms>::iterator it = targets.begin(); it != targets.end(); ) {
        if( !sp.is_valid_target( caster, *it ) ) {
            it = targets.erase( it );
        } else {
            ++it;
        }
    }

    return targets;
}

static std::set<tripoint_bub_ms> spell_effect_area( const spell &sp, const tripoint_bub_ms &target,
        const Creature &caster )
{
    // calculate spell's effect area
    std::set<tripoint_bub_ms> targets = calculate_spell_effect_area( sp, target, caster );
    if( !sp.has_flag( spell_flag::NO_EXPLOSION_SFX ) ) {
        // Draw the explosion
        std::map<tripoint, nc_color> explosion_colors;
        for( const tripoint_bub_ms &pt : targets ) {
            explosion_colors[pt.raw()] = sp.damage_type_color();
        }

        std::string exp_name = "explosion_" + sp.id().str();

        explosion_handler::draw_custom_explosion( get_player_character().pos(), explosion_colors,
                exp_name );
    }
    return targets;
}

static void splash_target( const tripoint_bub_ms &target, const spell &sp, Creature &caster )
{
    if( sp.liquid_volume( caster ) < 1 ) {
        debugmsg( "LIQUID flagged spell cast with liquid volume < 1" );
        return;
    }
    creature_tracker &creatures = get_creature_tracker();
    Character *const guy = creatures.creature_at<Character>( target );
    if( sp.get_spell_type()->affected_bps.none() ) {
        bodypart_id bp = guy->random_body_part( false );
        guy->worn.splash_attack( *guy, sp, caster, bp );
    } else {
        body_part_set parts = sp.get_spell_type()->affected_bps;
        for( auto iter = parts.begin(); iter != parts.end(); ) {
            bodypart_str_id bp = *iter;
            guy->worn.splash_attack( *guy, sp, caster, bp );
            iter++;
        }
    }
}

static void add_effect_to_target( const tripoint_bub_ms &target, const spell &sp, Creature &caster )
{
    const int dur_moves = sp.duration( caster );
    const int effect_intensity = sp.effect_intensity( caster );
    time_duration dur_td = time_duration::from_moves( dur_moves );
    creature_tracker &creatures = get_creature_tracker();
    Creature *const critter = creatures.creature_at<Creature>( target );
    Character *const guy = creatures.creature_at<Character>( target );
    efftype_id spell_effect( sp.effect_data() );
    bool bodypart_effected = false;
    if( guy ) {
        // If random_effect_part, grab a random part and affect it.
        if( sp.has_flag( spell_flag::RANDOM_EFFECT_PART ) && sp.bps_affected() == 0 ) {
            const bodypart_id bp = static_cast<const bodypart_id>( guy->get_random_body_part( true ) );
            if( sp.has_flag( spell_flag::LIQUID ) ) {
                splash_target( target, sp, caster );
                bodypart_effected = true;
            } else if( sp.has_flag( spell_flag::TOUCH ) && !guy->has_flag( json_flag_ETHEREAL ) ) {
                float hit_chance = guy->worn.coverage_with_flags_exclude( bp, { flag_AURA, flag_SEMITANGIBLE, flag_PERSONAL } );
                if( hit_chance < rng( 1, 100 ) ) {
                    guy->add_effect( spell_effect, dur_td, bp, sp.has_flag( spell_flag::PERMANENT ), effect_intensity );
                }
                bodypart_effected = true;
            } else {
                guy->add_effect( spell_effect, dur_td, bp, sp.has_flag( spell_flag::PERMANENT ), effect_intensity );
                bodypart_effected = true;
            }
            // Part isn't random, so hit all listed parts, if any are listed.
        } else {
            for( const bodypart_id &bp : guy->get_all_body_parts() ) {
                if( sp.bp_is_affected( bp.id() ) ) {
                    if( sp.has_flag( spell_flag::LIQUID ) ) {
                        splash_target( target, sp, caster );
                        bodypart_effected = true;
                    } else if( sp.has_flag( spell_flag::TOUCH ) && !guy->has_flag( json_flag_ETHEREAL ) ) {
                        float hit_chance = guy->worn.coverage_with_flags_exclude( bp, { flag_AURA, flag_SEMITANGIBLE, flag_PERSONAL } );
                        if( hit_chance < rng( 1, 100 ) ) {
                            guy->add_effect( spell_effect, dur_td, bp, sp.has_flag( spell_flag::PERMANENT ), effect_intensity );
                        }
                        bodypart_effected = true;
                    } else {
                        guy->add_effect( spell_effect, dur_td, bp, sp.has_flag( spell_flag::PERMANENT ), effect_intensity );
                        bodypart_effected = true;
                    }
                }
            }
        }
    }
    // Either no parts were listed or this was a monster. Either way, just apply the effect to them generally.
    if( !bodypart_effected ) {
        // A lazy way to deal with acid for now.
        if( spell_effect == effect_corroding ) {
            dur_td -= 1_seconds * critter->as_monster()->get_armor_type( damage_acid, bodypart_id( "torso" ) );
            if( dur_td < 1_seconds ) {
                return;
            }
        }
        critter->add_effect( spell_effect, dur_td );
    }
}

static void damage_targets( const spell &sp, Creature &caster,
                            const std::set<tripoint_bub_ms> &targets )
{
    map &here = get_map();
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &target : targets ) {
        if( !sp.is_valid_target( caster, target ) ) {
            continue;
        }
        sp.make_sound( target, caster );
        sp.create_field( target, caster );
        bool dodgeable = sp.has_flag( spell_flag::DODGEABLE );
        bool liquid = sp.has_flag( spell_flag::LIQUID );
        bool liquid_damage_target = ( sp.has_flag( spell_flag::LIQUID_DAMAGE_TARGET ) );
        if( sp.has_flag( spell_flag::IGNITE_FLAMMABLE ) && here.is_flammable( target ) ) {
            here.add_field( target, fd_fire, 1, 10_minutes );

            Character &player_character = get_player_character();
            if( player_character.has_trait( trait_PYROMANIA ) &&
                !player_character.has_morale( morale_pyromania_startfire ) ) {
                player_character.add_msg_if_player( m_good,
                                                    _( "You feel a surge of euphoria as flames burst out!" ) );
                player_character.add_morale( morale_pyromania_startfire, 15, 15, 8_hours, 6_hours );
                player_character.rem_morale( morale_pyromania_nofire );
            }
        }
        Creature *const cr = creatures.creature_at<Creature>( target );
        if( !cr ) {
            continue;
        }

        dealt_projectile_attack atk = sp.get_projectile_attack( target, *cr, caster );
        const float spell_accuracy = static_cast<float>( sp.accuracy( caster ) );
        if( cr->is_underwater() && liquid ) {
            caster.add_msg_if_player( m_bad,
                                      _( "The liquid is harmlessly dispersed into the surrounding water." ) );
            continue;
        }
        if( dodgeable ) {
            const float dodge_training = sp.dodge_training( caster );
            if( cr->dodge_check( spell_accuracy, dodge_training ) ) {
                if( !cr->is_monster() ) {
                    cr->add_msg_player_or_npc( "You dodge out of the way!", "%s dodges out of the way!" );
                } else {
                    if( !cr->has_effect( effect_invisibility ) ) {
                        add_msg_if_player_sees( cr->pos(), m_bad, _( "%1$s dodges out of the way!" ),
                                                cr->disp_name( false, true ) );
                    }
                }
                cr->on_dodge( &caster, spell_accuracy, dodge_training );
                continue;
            }
        }
        // Liquid attacks add their effects to characters via splash_target further down
        if( !sp.effect_data().empty() && ( !liquid || cr->is_monster() ) ) {
            add_effect_to_target( target, sp, caster );
        }
        if( sp.damage( caster ) > 0 || ( liquid && !cr->is_monster() ) ) {
            bool no_dodge_mitigation = sp.has_flag( spell_flag::NO_DODGE_MITIGATION );
            bool no_block_mitigation = sp.has_flag( spell_flag::NO_BLOCK_MITIGATION );
            // calculate damage mitigation from various sources
            // 5% per point (linear) ranging from 0-33%, capped at block score
            // skip if the attack was dodgeable as we'd have evaded it already
            double damage_mitigation_multiplier = 1.0;

            if( !no_block_mitigation ) {
                const int spell_block = cr->get_block_bonus();
                if( spell_block - spell_accuracy > 0 &&
                    !dodgeable ) {
                    const int roll = std::round( rng( 1, 20 ) );
                    damage_mitigation_multiplier -= ( 1 - 0.05 * std::max( roll, spell_block ) ) / 3.0;
                }
            }

            if( !no_dodge_mitigation && !dodgeable && cr->dodge_check( spell_accuracy ) ) {
                const int spell_dodge = cr->get_dodge() - spell_accuracy;
                const int roll = std::round( rng( 1, 20 ) );
                damage_mitigation_multiplier -= ( 1 - 0.05 * std::max( roll, spell_dodge ) ) / 3.0;
            }

            if( const int spell_resist = cr->get_spell_resist() - spell_accuracy > 0 &&
                                         !sp.has_flag( spell_flag::NON_MAGICAL ) ) {
                const int roll = std::round( rng( 1, 20 ) );
                damage_mitigation_multiplier -= ( 1 - 0.05 * std::max( roll, spell_resist ) ) / 3.0;
            }

            // If it's a liquid attack and the target is a character, splash_target will handle the rest
            if( liquid && !cr->is_monster() ) {
                splash_target( target, sp, caster );
                continue;
            }

            for( damage_unit &val : atk.proj.impact.damage_units ) {
                val.amount *= damage_mitigation_multiplier;
            }
            // If the target is a characteer:
            if( cr->as_character() != nullptr && ( liquid_damage_target || !liquid ) ) {
                int multishot = sp.get_amount_of_projectiles( caster );
                std::vector<bodypart_id> target_bdpts = cr->get_all_body_parts( get_body_part_flags::only_main );

                if( sp.bps_affected() > 0 ) {
                    for( auto it = target_bdpts.begin(); it != target_bdpts.end(); ) {
                        if( !sp.bp_is_affected( it->id() ) ) {
                            it = target_bdpts.erase( it );
                        } else {
                            ++it;
                        }
                    }
                }

                if( multishot > 1 ) {
                    for( damage_unit &val : atk.proj.impact.damage_units ) {
                        val.amount = roll_remainder( val.amount / multishot );
                    }
                    for( int i = 0; i < multishot; ++i ) {
                        cr->deal_projectile_attack( cr, atk, true );
                    }
                } else if( sp.has_flag( spell_flag::SPLIT_DAMAGE ) ) {
                    int amount_of_bp = target_bdpts.size();
                    for( damage_unit &val : atk.proj.impact.damage_units ) {
                        val.amount = roll_remainder( val.amount / amount_of_bp );
                    }
                    for( bodypart_id bp_id : target_bdpts ) {
                        cr->deal_damage( cr, bp_id, atk.proj.impact );
                    }
                } else if( sp.has_flag( spell_flag::PERCENTAGE_DAMAGE ) ) {
                    for( bodypart_id bp_id : target_bdpts ) {
                        for( damage_unit &val : atk.proj.impact.damage_units ) {
                            val.amount = roll_remainder( cr->get_hp( bp_id ) * sp.damage( caster ) / 100.0 );
                            cr->deal_damage( cr, bp_id, atk.proj.impact );
                        }
                    }
                } else {
                    cr->deal_projectile_attack( &caster, atk, true );
                }
            }
            // If the target is a monster:
            if( cr->as_monster() != nullptr && ( liquid_damage_target || !liquid ) ) {
                for( damage_unit &val : atk.proj.impact.damage_units ) {
                    if( sp.has_flag( spell_flag::PERCENTAGE_DAMAGE ) ) {
                        val.amount = cr->get_hp() * sp.damage( caster ) / 100.0;
                    }
                }
                cr->deal_projectile_attack( &caster, atk, true );
            }
        } else if( sp.damage( caster ) < 0 ) {
            sp.heal( target, caster );
            add_msg_if_player_sees( cr->pos(), m_good, _( "%s wounds are closing up!" ),
                                    cr->disp_name( true ) );
        }

        // handling DOTs here
        if( cr->as_character() != nullptr ) {
            std::vector<bodypart_id> target_bdpts = cr->get_all_body_parts( get_body_part_flags::only_main );

            if( sp.bps_affected() > 0 ) {
                for( auto it = target_bdpts.begin(); it != target_bdpts.end(); ) {
                    if( !sp.bp_is_affected( it->id() ) ) {
                        it = target_bdpts.erase( it );
                    } else {
                        ++it;
                    }
                }
            }

            if( sp.has_flag( spell_flag::PERCENTAGE_DAMAGE ) ) {
                for( bodypart_id bpid : target_bdpts ) {
                    damage_over_time_data foo = sp.damage_over_time( { bpid }, caster );
                    foo.amount = cr->get_hp( bpid ) * foo.amount / 100.0;
                    cr->add_damage_over_time( foo );
                }
            } else if( sp.has_flag( spell_flag::SPLIT_DAMAGE ) ) {
                damage_over_time_data dot_data = sp.damage_over_time( target_bdpts, caster );
                dot_data.amount /= target_bdpts.size();
                cr->add_damage_over_time( dot_data );
            } else if( sp.bps_affected() > 0 ) {
                cr->add_damage_over_time( sp.damage_over_time( target_bdpts, caster ) );
            } else {
                cr->add_damage_over_time( sp.damage_over_time( { cr->get_random_body_part() }, caster ) );
            }
        } else {
            cr->add_damage_over_time( sp.damage_over_time( { body_part_bp_null }, caster ) );
        }
    }
}

void spell_effect::attack( const spell &sp, Creature &caster, const tripoint_bub_ms &epicenter )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, epicenter, caster );
    damage_targets( sp, caster, area );
    if( sp.has_flag( spell_flag::SWAP_POS ) ) {
        swap_pos( caster, epicenter );
    }
    const double bash_scaling = sp.bash_scaling( caster );
    if( bash_scaling > 0 ) {
        ::map &here = get_map();
        for( const tripoint_bub_ms &potential_target : area ) {
            if( !sp.is_valid_target( caster, potential_target ) ) {
                continue;
            }
            // the bash already makes noise, so no need for spell::make_sound()
            here.bash( potential_target, sp.damage( caster ) * bash_scaling,
                       sp.has_flag( spell_flag::SILENT ) );
        }
    }
}

static void magical_polymorph( monster &victim, Creature &caster, const spell &sp )
{
    mtype_id new_id = mtype_id( sp.effect_data() );

    if( sp.has_flag( spell_flag::POLYMORPH_GROUP ) ) {
        const mongroup_id group_id( sp.effect_data() );
        new_id = MonsterGroupManager::GetRandomMonsterFromGroup( group_id );
    }

    // if effect_str is empty, we become a random monster of close difficulty
    if( new_id.is_empty() ) {
        int victim_diff = victim.type->difficulty;
        const std::vector<mtype> &mtypes = MonsterGenerator::generator().get_all_mtypes();
        for( int difficulty_variance = 1; difficulty_variance < 2048; difficulty_variance *= 2 ) {
            unsigned int random_entry = rng( 0, mtypes.size() );
            unsigned int iter = random_entry + 1;
            while( iter != random_entry && new_id.is_empty() ) {
                if( iter >= mtypes.size() ) {
                    iter = 0;
                }
                if( ( mtypes[iter].id != victim.type->id ) && ( std::abs( mtypes[iter].difficulty - victim_diff )
                        <= difficulty_variance ) ) {
                    if( !mtypes[iter].in_species( species_HALLUCINATION ) &&
                        mtypes[iter].id != mon_generator ) {
                        new_id = mtypes[iter].id;
                        break;
                    }
                }
                iter++;
            }
        }
    }

    if( !new_id.is_valid() ) {
        debugmsg( "magical_polymorph called with an invalid monster id" );
        return;
    }

    add_msg_if_player_sees( victim, _( "The %s transforms into a %s." ),
                            victim.type->nname(), new_id->nname() );
    victim.poly( new_id );

    if( sp.has_flag( spell_flag::FRIENDLY_POLY ) ) {
        if( caster.as_character() ) {
            victim.friendly = -1;
        } else {
            victim.make_ally( *caster.as_monster() );
        }
    }
}

void spell_effect::targeted_polymorph( const spell &sp, Creature &caster,
                                       const tripoint_bub_ms &target )
{
    //we only target monsters for now.
    if( monster *const victim = get_creature_tracker().creature_at<monster>( target ) ) {
        if( victim->get_hp() < sp.damage( caster ) ) {
            magical_polymorph( *victim, caster, sp );
            return;
        }
    }
    //victim had high hp, or isn't a monster.
    caster.add_msg_if_player( m_bad, _( "Your target resists transformation." ) );
}

area_expander::area_expander() : frontier( area_node_comparator( area ) )
{
}

// Check whether we have already visited this node.
int area_expander::contains( const tripoint_bub_ms &pt ) const
{
    return area_search.count( pt ) > 0;
}

// Adds node to a search tree. Returns true if new node is allocated.
bool area_expander::enqueue( const tripoint_bub_ms &from, const tripoint_bub_ms &to, float cost )
{
    if( contains( to ) ) {
        // We will modify existing node if its cost is lower.
        int index = area_search[to];
        node &node = area[index];
        if( cost < node.cost ) {
            node.from = from;
            node.cost = cost;
        }
        return false;
    }
    int index = area.size();
    area.push_back( {to, from, cost} );
    frontier.push( index );
    area_search[to] = index;
    return true;
}

// Run wave propagation
int area_expander::run( const tripoint_bub_ms &center )
{
    enqueue( center, center, 0.0 );

    static constexpr std::array<int, 8> x_offset = {{ -1, 1,  0, 0,  1, -1, -1, 1  }};
    static constexpr std::array<int, 8> y_offset = {{  0, 0, -1, 1, -1,  1, -1, 1  }};

    // Number of nodes expanded.
    int expanded = 0;

    map &here = get_map();
    while( !frontier.empty() ) {
        int best_index = frontier.top();
        frontier.pop();

        for( size_t i = 0; i < 8; i++ ) {
            node &best = area[best_index];
            const tripoint_bub_ms &pt = best.position + point( x_offset[ i ], y_offset[ i ] );

            if( here.coverage( pt ) > 0 && rng( 1, 100 ) < here.coverage( pt ) ) {
                continue;
            }

            float center_range = static_cast<float>( rl_dist( center, pt ) );
            if( max_range > 0 && center_range > max_range ) {
                continue;
            }

            if( max_expand > 0 && expanded > max_expand && contains( pt ) ) {
                continue;
            }

            float delta_range = trig_dist( best.position, pt );

            if( enqueue( best.position, pt, best.cost + delta_range ) ) {
                expanded++;
            }
        }
    }
    return expanded;
}

// Sort nodes by its cost.
void area_expander::sort_ascending()
{
    // Since internal caches like 'area_search' and 'frontier' use indexes inside 'area',
    // these caches will be invalidated.
    std::sort( area.begin(), area.end(),
    []( const node & a, const node & b )  -> bool {
        return a.cost < b.cost;
    } );
}

void area_expander::sort_descending()
{
    // Since internal caches like 'area_search' and 'frontier' use indexes inside 'area',
    // these caches will be invalidated.
    std::sort( area.begin(), area.end(),
    []( const node & a, const node & b ) -> bool {
        return a.cost > b.cost;
    } );
}

static void move_items( map &here, const tripoint_bub_ms &from, const tripoint_bub_ms &to )
{
    for( const item &it : here.i_at( from ) ) {
        here.add_item( to, it );
    }
    here.i_clear( from );
}

static void move_field( map &here, const tripoint_bub_ms &from, const tripoint_bub_ms &to )
{
    field &src_field = here.field_at( from );
    std::map<field_type_id, int> moving_fields;
    for( const std::pair<const field_type_id, field_entry> &fd : src_field ) {
        if( fd.first.is_valid() && !fd.first.id().is_null() ) {
            const int intensity = fd.second.get_field_intensity();
            moving_fields.emplace( fd.first, intensity );
        }
    }
    for( const std::pair<const field_type_id, int> &fd : moving_fields ) {
        here.remove_field( from, fd.first );
        here.set_field_intensity( to, fd.first, fd.second );
    }
}

// Moving all objects from one point to another by the power of magic.
static void spell_move( const spell &sp, const Creature &caster,
                        const tripoint_bub_ms &from, const tripoint_bub_ms &to )
{
    if( from == to ) {
        return;
    }

    // Moving creatures
    const bool can_target_self = sp.is_valid_target( spell_target::self );
    const bool can_target_ally = sp.is_valid_target( spell_target::ally );
    const bool can_target_hostile = sp.is_valid_target( spell_target::hostile );
    const bool can_target_creature = can_target_self || can_target_ally || can_target_hostile;

    if( can_target_creature ) {
        if( Creature *victim = get_creature_tracker().creature_at<Creature>( from ) ) {
            Creature::Attitude cr_att = victim->attitude_to( get_player_character() );
            bool valid = cr_att != Creature::Attitude::FRIENDLY && can_target_hostile;
            if( victim == &caster ) {
                valid = can_target_self;
            } else {
                valid |= cr_att == Creature::Attitude::FRIENDLY && can_target_ally;
                valid |= victim == &caster && can_target_self;
            }
            if( valid ) {
                victim->knock_back_to( to.raw() );
            }
        }
    }

    // Moving items
    if( sp.is_valid_target( spell_target::item ) ) {
        move_items( get_map(), from, to );
    }

    // Moving fields.
    if( sp.is_valid_target( spell_target::field ) ) {
        move_field( get_map(), from, to );
    }
}

static std::pair<field, tripoint_bub_ms> spell_remove_field( const spell &sp,
        const field_type_id &target_field_type_id,
        const tripoint_bub_ms &center,
        const Creature &caster )
{
    ::map &here = get_map();
    area_expander expander;

    expander.max_range = sp.aoe( caster );
    expander.run( center );
    expander.sort_ascending();

    field field_removed = field();
    tripoint_bub_ms field_position = tripoint_bub_ms();

    bool did_field_removal = false;

    for( const area_expander::node &node : expander.area ) {
        if( node.from == node.position ) {
            continue;
        }

        field &target_field = here.field_at( node.position );
        for( const std::pair<const field_type_id, field_entry> &fd : target_field ) {
            if( fd.first.is_valid() && !fd.first.id().is_null() &&
                fd.second.get_field_type() == target_field_type_id ) {
                field_removed = target_field;
                field_position = node.position;
                here.remove_field( node.position, target_field_type_id );
                did_field_removal = true;
            }
        }

        if( did_field_removal ) {
            // only remove one field in case of multiple
            break;
        }
    }

    return std::pair<field, tripoint_bub_ms> {field_removed, field_position};
}

static void handle_remove_fd_fatigue_field( const std::pair<field, tripoint_bub_ms>
        &fd_fatigue_field,
        Creature &caster )
{
    for( const std::pair<const field_type_id, field_entry> &fd : std::get<0>( fd_fatigue_field ) ) {
        const int &intensity = fd.second.get_field_intensity();
        const translation &intensity_name = fd.second.get_intensity_level().name;
        const tripoint_bub_ms &field_position = std::get<1>( fd_fatigue_field );
        const bool sees_field = caster.sees( field_position );

        switch( intensity ) {
            case 1:

                if( sees_field ) {
                    caster.add_msg_if_player( m_neutral, _( "The %s fades.  You feel strange." ), intensity_name );
                } else {
                    caster.add_msg_if_player( m_neutral, _( "You suddenly feel strange." ) );
                }

                caster.add_effect( effect_teleglow, 1_hours );
                break;
            case 2:
                if( sees_field ) {
                    caster.add_msg_if_player( m_neutral, _( "The %s dissipates.  You feel strange." ), intensity_name );
                } else {
                    caster.add_msg_if_player( m_neutral, _( "You suddenly feel strange." ) );
                }

                caster.add_effect( effect_teleglow, 5_hours );
                break;
            case 3:
                std::string message_prefix = "A nearby";

                if( caster.sees( field_position ) ) {
                    message_prefix = "The";
                }

                caster.add_msg_if_player( m_bad,
                                          _( "%s %s pulls you in as it closes and ejects you violently!" ),
                                          message_prefix, intensity_name );
                caster.as_character()->hurtall( 10, nullptr );
                caster.add_effect( effect_teleglow, 630_minutes );
                teleport::teleport( caster );
                break;
        }
        break;
    }
}

void spell_effect::remove_field( const spell &sp, Creature &caster, const tripoint_bub_ms &center )
{
    const field_type_id &target_field_type_id = field_type_id( sp.effect_data() );
    std::pair<field, tripoint_bub_ms> field_removed = spell_remove_field( sp, target_field_type_id,
            center, caster );

    for( const std::pair<const field_type_id, field_entry> &fd : std::get<0>( field_removed ) ) {
        if( fd.first.is_valid() && !fd.first.id().is_null() ) {
            sp.make_sound( caster.pos_bub(), caster );

            if( fd.first.id() == fd_fatigue ) {
                handle_remove_fd_fatigue_field( field_removed, caster );
            } else {
                caster.add_msg_if_player( m_neutral, _( "The %s dissipates." ),
                                          fd.second.get_intensity_level().name );
            }
        }
    }
}

void spell_effect::area_pull( const spell &sp, Creature &caster, const tripoint_bub_ms &center )
{
    area_expander expander;

    expander.max_range = sp.aoe( caster );
    expander.run( center );
    expander.sort_ascending();

    for( const area_expander::node &node : expander.area ) {
        if( node.from == node.position ) {
            continue;
        }

        spell_move( sp, caster, node.position, node.from );
    }
    sp.make_sound( caster.pos_bub(), caster );
}

void spell_effect::area_push( const spell &sp, Creature &caster, const tripoint_bub_ms &center )
{
    area_expander expander;

    expander.max_range = sp.aoe( caster );
    expander.run( center );
    expander.sort_descending();

    for( const area_expander::node &node : expander.area ) {
        if( node.from == node.position ) {
            continue;
        }

        spell_move( sp, caster, node.from, node.position );
    }
    sp.make_sound( caster.pos_bub(), caster );
}

static void character_push_effects( Creature *caster, Character &guy, tripoint_bub_ms &push_dest,
                                    const int push_distance, const std::vector<tripoint_bub_ms> &push_vec )
{
    int dist_left = std::abs( push_distance );
    tripoint_bub_ms old_pushed_point = guy.pos_bub();
    for( const tripoint_bub_ms &pushed_point : push_vec ) {
        if( get_map().impassable( pushed_point ) ) {
            guy.hurtall( dist_left * 4, caster );
            push_dest = old_pushed_point;
            break;
        } else if( get_creature_tracker().creature_at( pushed_point ) ) {
            push_dest = old_pushed_point;
            break;
        } else {
            dist_left--;
            old_pushed_point = pushed_point;
        }
    }
    guy.setpos( push_dest );
}

void spell_effect::directed_push( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    // this group of variables is for deferring movement of the avatar
    int pushed_distance = 0;
    tripoint_bub_ms push_to;
    std::vector<tripoint_bub_ms> pushed_vec;
    bool player_pushed = false;

    ::map &here = get_map();

    // whether it's push or pull, so how the multimap is sorted
    // -1 is push and 1 is pull
    const int sign = sp.damage( caster ) > 0 ? -1 : 1;

    std::multimap<int, tripoint_bub_ms> targets_ordered_by_range;
    for( const tripoint_bub_ms &pt : area ) {
        targets_ordered_by_range.emplace( sign * rl_dist( pt, caster.pos_bub() ), pt );
    }

    creature_tracker &creatures = get_creature_tracker();
    for( const std::pair<const int, tripoint_bub_ms> &pair : targets_ordered_by_range ) {
        const tripoint_bub_ms &push_point = pair.second;
        const units::angle angle = coord_to_angle( caster.pos_bub(), target );
        // positive is push, negative is pull
        int push_distance = sp.damage( caster );
        const int prev_distance = rl_dist( caster.pos_bub(), target );
        if( push_distance < 0 ) {
            push_distance = std::max( -std::abs( push_distance ), -std::abs( prev_distance ) );
        }
        if( push_distance == 0 ) {
            continue;
        }

        tripoint_bub_ms push_dest;
        calc_ray_end( angle, push_distance, push_point, push_dest );
        const std::vector<tripoint_bub_ms> push_vec = line_to( push_point, push_dest );

        const Creature *critter = creatures.creature_at<Creature>( push_point );
        if( critter != nullptr ) {
            const Creature::Attitude attitude_to_target =
                caster.attitude_to( *creatures.creature_at<Creature>( push_point ) );

            monster *mon = creatures.creature_at<monster>( push_point );
            npc *guy = creatures.creature_at<npc>( push_point );

            if( ( sp.is_valid_target( spell_target::self ) && push_point == caster.pos_bub() ) ||
                ( attitude_to_target == Creature::Attitude::FRIENDLY &&
                  sp.is_valid_target( spell_target::ally ) ) ||
                ( ( attitude_to_target == Creature::Attitude::HOSTILE ||
                    attitude_to_target == Creature::Attitude::NEUTRAL ) &&
                  sp.is_valid_target( spell_target::hostile ) ) ) {
                if( creatures.creature_at<avatar>( push_point ) ) {
                    // defer this because this absolutely must be done last in order not to mess up our calculations
                    player_pushed = true;
                    pushed_distance = push_distance;
                    push_to = push_dest;
                    pushed_vec = push_vec;
                } else if( mon ) {
                    int dist_left = std::abs( push_distance );
                    tripoint_bub_ms old_pushed_push_point = push_point;
                    for( const tripoint_bub_ms &pushed_push_point : push_vec ) {
                        if( get_map().impassable( pushed_push_point ) ) {
                            mon->apply_damage( &caster, bodypart_id(), dist_left * 10 );
                            push_dest = old_pushed_push_point;
                            break;
                        } else if( creatures.creature_at( pushed_push_point ) ) {
                            push_dest = old_pushed_push_point;
                            break;
                        } else {
                            dist_left--;
                            old_pushed_push_point = pushed_push_point;
                        }
                    }
                    mon->setpos( push_dest );
                } else if( guy ) {
                    character_push_effects( &caster, *guy, push_dest, push_distance, push_vec );
                }
            }
        }

        if( sp.is_valid_target( spell_target::item ) && here.has_items( push_point ) ) {
            move_items( here, push_point, push_dest );
        }

        if( sp.is_valid_target( spell_target::field ) ) {
            move_field( here, push_point, push_dest );
        }
    }

    // deferred avatar pushing
    if( player_pushed ) {
        character_push_effects( &caster, get_avatar(), push_to, pushed_distance, pushed_vec );
    }
}

void spell_effect::spawn_ethereal_item( const spell &sp, Creature &caster, const tripoint_bub_ms & )
{
    if( !caster.is_avatar() ) {
        debugmsg( "Spells that spawn items are only supported for the avatar, not for %s.",
                  caster.disp_name() );
        return;
    }

    std::vector<item> granted;

    int count = std::max( 1, sp.damage( caster ) );
    for( int i = 0; i < count; i++ ) {
        if( sp.has_flag( spell_flag::SPAWN_GROUP ) ) {
            std::vector<item> group_items = item_group::items_from( item_group_id( sp.effect_data() ),
                                            calendar::turn );
            granted.insert( granted.end(), group_items.begin(), group_items.end() );
        } else {
            granted.emplace_back( sp.effect_data(), calendar::turn );
        }
    }

    avatar &player_character = get_avatar();
    for( item &it : granted ) {
        // Spawned items are ethereal unless permanent and max level. Comestibles are never ethereal.
        if( !it.is_comestible() && !sp.has_flag( spell_flag::PERMANENT_ALL_LEVELS ) &&
            !( sp.has_flag( spell_flag::PERMANENT ) && sp.is_max_level( caster ) ) ) {
            it.set_var( "ethereal", to_turns<int>( sp.duration_turns( caster ) ) );
            it.ethereal = true;
        }

        if( it.ethereal && player_character.is_wearing( it.typeId() ) ) {
            // Ethereal equipment already exists so just update its duration
            item *existing_item = player_character.item_worn_with_id( it.typeId() );
            existing_item->set_var( "ethereal", to_turns<int>( sp.duration_turns( caster ) ) );
        } else if( player_character.can_wear( it ).success() ) {
            it.set_flag( json_flag_FIT );
            player_character.wear_item( it, false );
        } else if( !player_character.has_wield_conflicts( it ) &&
                   !player_character.martial_arts_data->keep_hands_free && //No wield if hands free
                   player_character.wield( it, 0 ) ) {
            // nothing to do
        } else {
            player_character.i_add( it );
        }
    }
    sp.make_sound( caster.pos_bub(), caster );
}

void spell_effect::recover_energy( const spell &sp, Creature &caster,
                                   const tripoint_bub_ms &target )
{
    // this spell is not appropriate for healing
    const int healing = sp.damage( caster );
    const std::string energy_source = sp.effect_data();
    // current limitation is that Character does not have stamina or power_level members
    Character *you = get_creature_tracker().creature_at<Character>( target );
    if( !you ) {
        return;
    }

    if( energy_source == "MANA" ) {
        you->magic->mod_mana( *you, healing );
    } else if( energy_source == "STAMINA" ) {
        you->mod_stamina( healing );
    } else if( energy_source == "FATIGUE" ) {
        // fatigue is backwards
        you->mod_fatigue( -healing );
    } else if( energy_source == "BIONIC" ) {
        if( healing > 0 ) {
            you->mod_power_level( units::from_kilojoule( static_cast<std::int64_t>( healing ) ) );
        } else {
            you->mod_stamina( healing );
        }
    } else if( energy_source == "PAIN" ) {
        // pain is backwards
        if( sp.has_flag( spell_flag::PAIN_NORESIST ) ) {
            you->mod_pain_noresist( -healing );
        } else {
            you->mod_pain( -healing );
        }
    } else if( energy_source == "HEALTH" ) {
        you->mod_livestyle( healing );
    } else {
        debugmsg( "Invalid effect_str %s for spell %s", energy_source, sp.name() );
    }
    sp.make_sound( caster.pos_bub(), caster );
}

void spell_effect::timed_event( const spell &sp, Creature &caster, const tripoint_bub_ms & )
{
    const std::map<std::string, timed_event_type> timed_event_map{
        { "help", timed_event_type::HELP },
        { "spawn_wyrms", timed_event_type::SPAWN_WYRMS },
        { "amigara", timed_event_type::AMIGARA },
        { "roots_die", timed_event_type::ROOTS_DIE },
        { "dim", timed_event_type::DIM },
        { "artifact_light", timed_event_type::ARTIFACT_LIGHT }
    };

    timed_event_type spell_event = timed_event_type::NONE;

    const auto iter = timed_event_map.find( sp.effect_data() );
    if( iter != timed_event_map.cend() ) {
        spell_event = iter->second;
    }

    sp.make_sound( caster.pos_bub(), caster );
    get_timed_events().add( spell_event, calendar::turn + sp.duration_turns( caster ) );
}

static bool is_summon_friendly( const spell &sp )
{
    const bool hostile = sp.has_flag( spell_flag::HOSTILE_SUMMON );
    bool friendly = !hostile;
    if( sp.has_flag( spell_flag::HOSTILE_50 ) ) {
        friendly = friendly && rng( 0, 1000 ) < 500;
    }
    return friendly;
}

static bool add_summoned_mon( const tripoint_bub_ms &pos, const time_duration &time,
                              const spell &sp, Creature &caster )
{
    std::string monster_id = sp.effect_data();

    // Spawn a monster from a group, or a specific monster ID
    if( sp.has_flag( spell_flag::SPAWN_GROUP ) ) {
        const mongroup_id group_id( sp.effect_data() );
        monster_id = MonsterGroupManager::GetRandomMonsterFromGroup( group_id ).str();
    }

    const mtype_id mon_id( monster_id );
    monster *const mon_ptr = g->place_critter_at( mon_id, pos );

    if( !mon_ptr ) {
        return false;
    }
    const bool permanent = sp.has_flag( spell_flag::PERMANENT );
    monster &spawned_mon = *mon_ptr;
    if( is_summon_friendly( sp ) ) {
        spawned_mon.friendly = INT_MAX;
    } else {
        spawned_mon.friendly = 0;
    }
    if( !permanent ) {
        spawned_mon.set_summon_time( time );
    }
    spawned_mon.no_extra_death_drops = !sp.has_flag( spell_flag::SPAWN_WITH_DEATH_DROPS );
    spawned_mon.no_corpse_quiet = sp.has_flag( spell_flag::NO_CORPSE_QUIET );
    spawned_mon.set_summoner( &caster );
    return true;
}

void spell_effect::spawn_summoned_monster( const spell &sp, Creature &caster,
        const tripoint_bub_ms &target )
{
    std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    // this should never be negative, but this'll keep problems from happening
    size_t num_mons = std::abs( sp.damage( caster ) );
    const time_duration summon_time = sp.duration_turns( caster );
    while( num_mons > 0 && !area.empty() ) {
        const size_t mon_spot = rng( 0, area.size() - 1 );
        auto iter = area.begin();
        std::advance( iter, mon_spot );
        if( add_summoned_mon( *iter, summon_time, sp, caster ) ) {
            num_mons--;
            sp.make_sound( *iter, caster );
        } else {
            debugmsg( "failed to place monster" );
        }
        // whether or not we succeed in spawning a monster, we don't want to try this tripoint again
        area.erase( iter );
    }
}

void spell_effect::spawn_summoned_vehicle( const spell &sp, Creature &caster,
        const tripoint_bub_ms &target )
{
    ::map &here = get_map();
    if( here.veh_at( target ) ) {
        caster.add_msg_if_player( m_bad, _( "There is already a vehicle there." ) );
        return;
    }
    if( vehicle *veh = here.add_vehicle( sp.summon_vehicle_id(), target, -90_degrees,
                                         100, 0, false ) ) {
        veh->unlock();
        veh->magic = true;
        if( !sp.has_flag( spell_flag::PERMANENT ) ) {
            veh->summon_time_limit = sp.duration_turns( caster );
        }
        if( caster.as_character() ) {
            veh->set_owner( *caster.as_character() );
        }
    }
}

void spell_effect::recharge_vehicle( const spell &sp, Creature &caster,
                                     const tripoint_bub_ms &target )
{
    ::map &here = get_map();
    optional_vpart_position v_part_pos = here.veh_at( target );
    if( !v_part_pos ) {
        caster.add_msg_if_player( m_bad, _( "There's no battery there." ) );
        return;
    }
    vehicle &veh = v_part_pos->vehicle();
    if( sp.damage( caster ) >= 0 ) {
        veh.charge_battery( sp.damage( caster ), false );
    } else {
        veh.discharge_battery( -sp.damage( caster ), false );
    }
}

void spell_effect::translocate( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    avatar *you = caster.as_avatar();
    if( you == nullptr ) {
        return;
    }
    you->translocators.translocate( spell_effect_area( sp, target, caster ) );
}

void spell_effect::none( const spell &sp, Creature &, const tripoint_bub_ms & )
{
    debugmsg( "ERROR: %s has invalid spell effect.", sp.name() );
}

void spell_effect::transform_blast( const spell &sp, Creature &caster,
                                    const tripoint_bub_ms &target )
{
    ter_furn_transform_id transform( sp.effect_data() );
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    for( const tripoint_bub_ms &location : area ) {
        if( one_in( sp.damage( caster ) ) ) {
            transform->transform( get_map(), location );
        }
    }
}

void spell_effect::noise( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    sp.make_sound( target, sp.damage( caster ) );
}

void spell_effect::vomit( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &potential_target : area ) {
        if( !sp.is_valid_target( caster, potential_target ) ) {
            continue;
        }
        Character *const ch = creatures.creature_at<Character>( potential_target );
        if( !ch ) {
            continue;
        }
        sp.make_sound( target, caster );
        ch->vomit();
    }
}

void spell_effect::pull_to_caster( const spell &sp, Creature &caster,
                                   const tripoint_bub_ms &target )
{
    caster.longpull( sp.name(), target );
}

void spell_effect::explosion( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    explosion_handler::explosion( &caster, target.raw(), sp.damage( caster ), sp.aoe( caster ) / 10.0,
                                  true );
}

void spell_effect::flashbang( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    explosion_handler::flashbang( target.raw(), caster.is_avatar() &&
                                  !sp.is_valid_target( spell_target::self ) );
}

void spell_effect::mod_moves( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &potential_target : area ) {
        if( !sp.is_valid_target( caster, potential_target ) ) {
            continue;
        }
        Creature *critter = creatures.creature_at<Creature>( potential_target );
        if( !critter ) {
            continue;
        }
        sp.make_sound( potential_target, caster );
        critter->mod_moves( sp.damage( caster ) );
    }
}

void spell_effect::map( const spell &sp, Creature &caster, const tripoint_bub_ms & )
{
    const avatar *you = caster.as_avatar();
    if( !you ) {
        // revealing the map only makes sense for the avatar
        return;
    }
    const tripoint_abs_omt center = you->global_omt_location();
    overmap_buffer.reveal( center.xy(), sp.aoe( caster ), center.z() );
}

void spell_effect::morale( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    if( sp.effect_data().empty() ) {
        debugmsg( "ERROR: %s must have a valid morale_type as effect_str.  None specified.",
                  sp.id().c_str() );
        return;
    }
    if( !morale_type( sp.effect_data() ).is_valid() ) {
        debugmsg( "ERROR: %s must have a valid morale_type as effect_str.  %s is invalid.", sp.id().c_str(),
                  sp.effect_data() );
        return;
    }
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &potential_target : area ) {
        Character *player_target;
        if( !( sp.is_valid_target( caster, potential_target ) &&
               ( player_target = creatures.creature_at<Character>( potential_target ) ) ) ) {
            continue;
        }
        player_target->add_morale( morale_type( sp.effect_data() ), sp.damage( caster ), 0,
                                   sp.duration_turns( caster ),
                                   sp.duration_turns( caster ) / 10, false );
        sp.make_sound( potential_target, caster );
    }
}

void spell_effect::charm_monster( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &potential_target : area ) {
        if( !sp.is_valid_target( caster, potential_target ) ) {
            continue;
        }
        monster *mon = creatures.creature_at<monster>( potential_target );
        if( !mon ) {
            continue;
        }
        sp.make_sound( potential_target, caster );
        if( ( mon->friendly == 0 || ( mon->friendly != 0 && sp.has_flag( spell_flag::RECHARM ) ) ) &&
            mon->get_hp() <= sp.damage( caster ) ) {
            mon->unset_dest();
            mon->friendly += sp.duration( caster ) / 100;
        }
    }
}

void spell_effect::revive( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    ::map &here = get_map();
    const species_id spec( sp.effect_data() );
    for( const tripoint_bub_ms &aoe : area ) {
        for( item &corpse : here.i_at( aoe ) ) {
            const mtype *mt = corpse.get_mtype();
            if( !( corpse.is_corpse() && corpse.can_revive() && corpse.active &&
                   mt->has_flag( mon_flag_REVIVES ) && !mt->has_flag( mon_flag_DORMANT ) && mt->in_species( spec ) &&
                   !mt->has_flag( mon_flag_NO_NECRO ) ) ) {
                continue;
            }
            if( g->revive_corpse( aoe.raw(), corpse ) ) {
                here.i_rem( aoe, &corpse );
                break;
            }
        }
    }
}

// identical to above, but checks for REVIVES && DORMANT flag. Ignores NO_NECRO.
void spell_effect::revive_dormant( const spell &sp, Creature &caster,
                                   const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    ::map &here = get_map();
    const species_id spec( sp.effect_data() );
    for( const tripoint_bub_ms &aoe : area ) {
        for( item &corpse : here.i_at( aoe ) ) {
            const mtype *mt = corpse.get_mtype();
            if( !( corpse.is_corpse() && corpse.can_revive() && corpse.active &&
                   mt->has_flag( mon_flag_REVIVES ) && mt->has_flag( mon_flag_DORMANT ) && mt->in_species( spec ) ) ) {
                continue;
            }
            // relaxed revive with radius.
            if( g->revive_corpse( aoe.raw(), corpse, 3 ) ) {
                here.i_rem( aoe, &corpse );
                break;
            }
        }
    }
}

void spell_effect::add_trap( const spell &sp, Creature &, const tripoint_bub_ms &target )
{
    ::map &here = get_map();
    const trap_id tr_id( sp.effect_data() );
    if( here.tr_at( target ) == tr_null ) {
        here.trap_set( target, tr_id );
    }
}

void spell_effect::upgrade( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &aoe : area ) {
        monster *mon = creatures.creature_at<monster>( aoe );
        if( mon != nullptr && rng( 1, 10000 ) < sp.damage( caster ) ) {
            mon->allow_upgrade();
            mon->try_upgrade( false );
        }
    }
}

void spell_effect::guilt( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    if( !caster.is_monster() ) {
        // only monsters cause the guilt morale effect
        return;
    }
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &aoe : area ) {
        Character *guilt_target = creatures.creature_at<Character>( aoe );
        if( guilt_target == nullptr ) {
            continue;
        }
        // there used to be a MAX_GUILT_DISTANCE here, but the spell's range will do this instead.
        monster &z = *caster.as_monster();
        const int kill_count = g->get_kill_tracker().guilt_kill_count( z.type->id );
        // this is when the player stops caring altogether.
        const int max_kills = sp.damage( caster );
        // this determines how strong the morale penalty will be
        const int guilt_mult = sp.get_effective_level();

        // different message as we kill more of the same monster
        std::string msg;
        game_message_type msgtype = m_bad; // default guilt message type
        std::map<int, std::string> guilt_thresholds;
        guilt_thresholds[ ceil( max_kills * 0.25 ) ] = _( "You feel awful about killing %s." );
        guilt_thresholds[ ceil( max_kills * 0.5 ) ] = _( "You feel remorse for killing %s." );
        guilt_thresholds[ ceil( max_kills * 0.75 ) ] = _( "You feel guilty for killing %s." );
        guilt_thresholds[max_kills] = _( "You feel uneasy about killing %s." );

        Character &guy = *guilt_target;
        if( guy.has_trait( trait_PSYCHOPATH ) ||
            guy.has_flag( json_flag_PRED3 ) || guy.has_flag( json_flag_PRED4 ) ) {
            // specially immune.
            return;
        }

        if( kill_count >= max_kills ) {
            // player no longer cares
            if( kill_count == max_kills ) {
                //~ Message after killing a lot of monsters which would normally affect the morale negatively. %s is the monster name, it most likely will be pluralized.
                guy.add_msg_if_player( m_good, _( "After killing so many bloody %s you no longer care "
                                                  "about their deaths anymore." ), z.name( max_kills ) );
            }
            return;
        } else if( guy.has_flag( json_flag_PRED1 ) ||
                   guy.has_flag( json_flag_PRED2 ) ) {
            msg = _( "Culling the weak is distasteful, but necessary." );
            msgtype = m_neutral;
        } else {
            for( const std::pair<const int, std::string> &guilt_threshold : guilt_thresholds ) {
                if( kill_count < guilt_threshold.first ) {
                    msg = guilt_threshold.second;
                    break;
                }
            }
        }

        guy.add_msg_if_player( msgtype, msg, z.name() );

        float killRatio = static_cast<float>( kill_count ) / max_kills;
        int moraleMalus = -5 * guilt_mult * ( 1.0 - killRatio );
        const int maxMalus = -250 * ( 1.0 - killRatio );
        const time_duration duration = sp.duration_turns( caster ) * ( 1.0 - killRatio );
        const time_duration decayDelay = 3_minutes * ( 1.0 - killRatio );

        bool shared_species = false;
        if( caster.is_dead_state() && caster.get_killer() != nullptr ) {
            for( const species_id &specie : caster.as_monster()->type->species ) {
                if( guy.in_species( specie ) ) {
                    shared_species = true;
                }
            }
        } else if( z.type->in_species( species_id( sp.effect_data() ) ) ) {
            shared_species = true;
        }
        // killing your own kind hurts your soul more
        if( shared_species ) {
            moraleMalus *= 2;
        }
        if( guy.has_trait( trait_PACIFIST ) ) {
            moraleMalus *= 5;
        }
        // cullers feel less bad about killing
        else if( guy.has_flag( json_flag_PRED1 ) ) {
            moraleMalus /= 4;
        }
        // hunters feel less bad about killing
        else if( guy.has_flag( json_flag_PRED2 ) ) {
            moraleMalus /= 5;
        }
        guy.add_morale( morale_killed_monster, moraleMalus, maxMalus, duration, decayDelay );
    }
}

void spell_effect::remove_effect( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &aoe : area ) {
        if( Creature *critter = creatures.creature_at( aoe ) ) {
            critter->remove_effect( efftype_id( sp.effect_data() ) );
        }
    }
}

void spell_effect::emit( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    for( const tripoint_bub_ms &aoe : area ) {
        get_map().emit_field( aoe.raw(), emit_id( sp.effect_data() ) );
    }
}

void spell_effect::fungalize( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    fungal_effects fe;
    for( const tripoint_bub_ms &aoe : area ) {
        fe.fungalize( aoe, &caster, sp.damage( caster ) / 10000.0 );
    }
}

void spell_effect::mutate( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &potential_target : area ) {
        if( !sp.is_valid_target( caster, potential_target ) ) {
            continue;
        }
        Character *guy = creatures.creature_at<Character>( potential_target );
        if( !guy ) {
            continue;
        }
        // 10000 represents 100.00% to increase granularity without swapping everything to a float
        if( sp.damage( caster ) < rng( 1, 10000 ) ) {
            // chance failure! but keep trying for other targets
            continue;
        }
        if( sp.effect_data().empty() ) {
            guy->mutate();
        } else {
            if( sp.has_flag( spell_flag::MUTATE_TRAIT ) ) {
                guy->mutate_towards( trait_id( sp.effect_data() ) );
            } else {
                guy->mutate_category( mutation_category_id( sp.effect_data() ) );
            }
        }
        sp.make_sound( potential_target, caster );
    }
}

void spell_effect::bash( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    ::map &here = get_map();
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );
    for( const tripoint_bub_ms &potential_target : area ) {
        if( !sp.is_valid_target( caster, potential_target ) ) {
            continue;
        }
        // the bash already makes noise, so no need for spell::make_sound()
        here.bash( potential_target, sp.damage( caster ), sp.has_flag( spell_flag::SILENT ) );
    }
}

void spell_effect::dash( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    const tripoint_bub_ms source = caster.pos_bub();
    const std::vector<tripoint_bub_ms> trajectory_local = line_to( source, target );
    ::map &here = get_map();
    // uses abs() coordinates
    std::vector<tripoint_abs_ms> trajectory;
    trajectory.reserve( trajectory_local.size() );
    for( const tripoint_bub_ms &local_point : trajectory_local ) {
        trajectory.push_back( here.getglobal( local_point ) );
    }
    avatar *caster_you = caster.as_avatar();
    auto walk_point = trajectory.begin();
    if( here.bub_from_abs( *walk_point ) == source ) {
        ++walk_point;
    }
    // save the amount of moves the caster has so we can restore them after the dash
    const int cur_moves = caster.get_moves();
    creature_tracker &creatures = get_creature_tracker();
    // Use a bool here so that we know that we're only airborne because of this spell.
    bool jumping = false;
    if( sp.has_flag( spell_flag::AIRBORNE ) && !caster.has_effect( effect_airborne ) ) {
        int jump_vert = 1 + std::abs( source.z() - target.z() );
        caster.add_effect( effect_airborne, 1_seconds, true );
        caster.add_effect( effect_jumping, 1_seconds, true, jump_vert );
        jumping = true;
    }
    while( walk_point != trajectory.end() ) {
        if( caster_you != nullptr ) {
            // Check if this is the second-to-last tile
            if( jumping && ( walk_point + 1 ) == trajectory.end() ) {
                caster.remove_effect( effect_airborne );
            }
            if( creatures.creature_at( here.bub_from_abs( *walk_point ) ) ||
                !g->walk_move( here.bub_from_abs( *walk_point ), false ) ) {
                if( walk_point != trajectory.begin() ) {
                    --walk_point;
                }
                break;
            } else if( walk_point != trajectory.begin() ) {
                sp.create_field( here.bub_from_abs( *( walk_point - 1 ) ), caster );
                g->draw_ter();
            }
        }
        ++walk_point;
    }
    if( jumping ) {
        // Redundant effect removal for safety's sake.
        caster.remove_effect( effect_airborne );
        caster.remove_effect( effect_jumping );
    }
    caster.set_moves( cur_moves );
    tripoint_bub_ms far_target;
    calc_ray_end( coord_to_angle( source, target ), sp.aoe( caster ),
                  here.bub_from_abs( *walk_point ),
                  far_target );

    spell_effect::override_parameters params( sp, caster );
    params.range = sp.aoe( caster );
    const std::set<tripoint_bub_ms> hit_area = spell_effect_cone_range_override( params, source,
            far_target );
    damage_targets( sp, caster, hit_area );
}

void spell_effect::banishment( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    int total_dam = sp.damage( caster );
    if( total_dam <= 0 ) {
        debugmsg( "ERROR: Banishment has negative or 0 damage value" );
    }

    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );

    std::vector<monster *> target_mons;
    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &potential_target : area ) {
        if( !sp.is_valid_target( caster, potential_target ) ) {
            continue;
        }
        // you can't banish npcs.
        if( monster *mon = creatures.creature_at<monster>( potential_target ) ) {
            target_mons.push_back( mon );
        }
    }

    if( target_mons.empty() ) {
        return;
    }

    for( monster *mon : target_mons ) {
        int overflow = mon->get_hp() - total_dam;
        // reset overflow in case we have more monsters to do
        total_dam = 0;
        while( overflow > 0 ) {
            int caster_total_hp = 0;
            int unbroken_parts = 0;
            for( const bodypart_id &part : caster.get_all_body_parts( get_body_part_flags::only_main ) ) {
                const int cur_part_hp = caster.get_part_hp_cur( part );
                if( cur_part_hp != 0 ) {
                    caster_total_hp += cur_part_hp;
                    unbroken_parts++;
                }
            }
            // we wannt to leave 1 hp on each already unbroken limb
            caster_total_hp -= unbroken_parts;
            if( overflow > caster_total_hp ) {
                caster.add_msg_if_player( m_bad, _( "Banishment failed, you are too weak!" ) );
                return;
            } else {
                // can change if a part has less hp than this
                float damage_per_part = static_cast<float>( overflow ) / static_cast<float>( unbroken_parts );
                int parts_checked = 0;

                for( const bodypart_id &part : caster.get_all_body_parts( get_body_part_flags::only_main ) ) {
                    const int cur_part_hp = caster.get_part_hp_cur( part );
                    if( cur_part_hp > std::ceil( damage_per_part ) ) {
                        const int rolled_dam = roll_remainder( damage_per_part );
                        caster.mod_part_hp_cur( part, -rolled_dam );
                        overflow -= rolled_dam;
                    } else {
                        caster.mod_part_hp_cur( part, -( cur_part_hp - 1 ) );
                        overflow -= cur_part_hp - 1;
                        damage_per_part = static_cast<float>( overflow ) /
                                          static_cast<float>( unbroken_parts - parts_checked );
                    }
                    parts_checked++;
                }
            }
        }

        caster.add_msg_if_player( m_good, string_format( _( "%s banished." ), mon->name() ) );
        // banished monsters take their stuff with them
        mon->death_drops = false;
        mon->die( &caster );
    }
}

void spell_effect::effect_on_condition( const spell &sp, Creature &caster,
                                        const tripoint_bub_ms &target )
{
    const std::set<tripoint_bub_ms> area = spell_effect_area( sp, target, caster );

    creature_tracker &creatures = get_creature_tracker();
    for( const tripoint_bub_ms &potential_target : area ) {
        if( !sp.is_valid_target( caster, potential_target ) ) {
            continue;
        }
        Creature *victim = creatures.creature_at<Creature>( potential_target );
        dialogue d( victim ? get_talker_for( victim ) : nullptr, get_talker_for( caster ) );
        d.amend_callstack( string_format( "Spell: %s Caster: %s", sp.id().c_str(), caster.disp_name() ) );
        effect_on_condition_id eoc = effect_on_condition_id( sp.effect_data() );
        if( eoc->type == eoc_type::ACTIVATION ) {
            eoc->activate( d );
        } else {
            debugmsg( "Must use an activation eoc for a spell.  If you don't want the effect_on_condition to happen on its own (without the spell being cast), remove the recurrence min and max.  Otherwise, create a non-recurring effect_on_condition for this spell with its condition and effects, then have a recurring one queue it." );
        }
    }
}

void spell_effect::slime_split_on_death( const spell &sp, Creature &caster,
        const tripoint_bub_ms &target )
{
    sp.make_sound( target, caster );
    int mass = caster.get_speed_base();
    monster *caster_monster = dynamic_cast<monster *>( &caster );
    if( caster_monster && caster_monster->type->id == mon_blob_brain ) {
        mass += mass;
    }
    const int radius = sp.aoe( caster );
    std::vector<tripoint_bub_ms> pts = closest_points_first( caster.pos_bub(), radius );
    std::vector<monster *> summoned_slimes;
    const bool permanent = sp.has_flag( spell_flag::PERMANENT );
    // Make sure the creature has enough mass to create new slimes
    if( mass >= mon_blob_small->speed / 2 ) {
        for( const tripoint_bub_ms &dest : pts ) {
            // Fall back to small slimes if no bigger smile is chosen
            mtype_id slime_id = mon_blob_small;
            if( mass > mon_blob_large->speed + 20 && one_in( 3 ) ) {
                // 33% chance of spawning a big slime if enough mass
                slime_id = mon_blob_large;
            } else if( mass > mon_blob->speed + 20 && one_in( 2 ) ) {
                // 50% chance of spawning a slime if enough mass
                slime_id = mon_blob;
            }

            shared_ptr_fast<monster> mon = make_shared_fast<monster>( slime_id );
            mon->ammo = mon->type->starting_ammo;
            if( mon->will_move_to( dest.raw() ) && mon->know_danger_at( dest.raw() ) ) {
                if( monster *const blob = g->place_critter_around( mon, dest.raw(), 0 ) ) {
                    sp.make_sound( dest, caster );
                    if( !permanent ) {
                        blob->set_summon_time( sp.duration_turns( caster ) );
                    }
                    if( caster_monster ) {
                        blob->make_ally( *caster_monster );
                    }
                    const int used_mass = std::min( mass, slime_id->speed - 15 );
                    mass -= used_mass;
                    blob->set_speed_base( used_mass );
                    blob->no_extra_death_drops = !sp.has_flag( spell_flag::SPAWN_WITH_DEATH_DROPS );
                    summoned_slimes.push_back( mon.get() );
                    if( mass < mon_blob_small->speed / 2 ) {
                        break;
                    }
                }
            }
        }
    }

    // Divide remaining mass between summoned slimes
    if( mass > 0 && !summoned_slimes.empty() ) {
        const int mass_per_mob = mass / summoned_slimes.size();
        for( monster *slime : summoned_slimes ) {
            slime->set_speed_base( slime->get_speed_base() + mass_per_mob );
            mass -= mass_per_mob;
        }
    }

    // Last resort: Find a slime nearby that will absorb part of this mass
    if( mass > 3 ) {
        for( const tripoint_bub_ms &dest : pts ) {
            if( monster *mon = get_creature_tracker().creature_at<monster>( dest ) ) {
                if( mon->type->in_species( species_SLIME ) ) {
                    // The mass discarded here should prevent issues when surrounded
                    // by big blobs that keep trading mass and spawning medium slimes
                    // over and over.
                    mon->set_speed_base( mon->get_speed_base() + mass - 3 );
                    return;
                }
            }
        }
    }
}
