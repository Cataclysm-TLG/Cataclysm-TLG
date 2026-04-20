#include "explosion.h" // IWYU pragma: associated
#include "fragment_cloud.h" // IWYU pragma: associated

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bodypart.h"
#include "calendar.h"
#include "cata_utility.h"
#include "character.h"
#include "color.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "current_map.h"
#include "damage.h"
#include "debug.h"
#include "enums.h"
#include "field_type.h"
#include "flag.h"
#include "flexbuffer_json-inl.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "game_constants.h"
#include "generic_factory.h"
#include "item.h"
#include "item_factory.h"
#include "item_location.h"
#include "itype.h"
#include "line.h"
#include "make_static.h"
#include "map.h"
#include "map_iterator.h"
#include "mapdata.h"
#include "math_defines.h"
#include "mdarray.h"
#include "messages.h"
#include "mongroup.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "options.h"
#include "point.h"
#include "projectile.h"
#include "rng.h"
#include "shadowcasting.h"
#include "sounds.h"
#include "translations.h"
#include "trap.h"
#include "type_id.h"
#include "units.h"
#include "vehicle.h"
#include "vpart_position.h"

static const ammo_effect_str_id ammo_effect_NULL_SOURCE( "NULL_SOURCE" );

static const damage_type_id damage_bash( "bash" );
static const damage_type_id damage_bullet( "bullet" );
static const damage_type_id damage_heat( "heat" );

static const efftype_id effect_blind( "blind" );
static const efftype_id effect_deaf( "deaf" );
static const efftype_id effect_emp( "emp" );
static const efftype_id effect_stunned( "stunned" );
static const efftype_id effect_teleglow( "teleglow" );

static const flag_id json_flag_ACTIVATE_ON_PLACE( "ACTIVATE_ON_PLACE" );
static const fault_id fault_emp_reboot( "fault_emp_reboot" );

static const furn_str_id furn_f_machinery_electronic( "f_machinery_electronic" );

static const itype_id fuel_type_none( "null" );
static const itype_id itype_e_handcuffs( "e_handcuffs" );
static const itype_id itype_mininuke_act( "mininuke_act" );

static const json_character_flag json_flag_EMP_IMMUNE( "EMP_IMMUNE" );
static const json_character_flag json_flag_GLARE_RESIST( "GLARE_RESIST" );

static const mongroup_id GROUP_NETHER( "GROUP_NETHER" );

static const species_id species_ROBOT( "ROBOT" );

static const ter_str_id ter_t_card_industrial( "t_card_industrial" );
static const ter_str_id ter_t_card_military( "t_card_military" );
static const ter_str_id ter_t_card_reader_broken( "t_card_reader_broken" );
static const ter_str_id ter_t_card_science( "t_card_science" );
static const ter_str_id ter_t_door_metal_locked( "t_door_metal_locked" );
static const ter_str_id ter_t_floor( "t_floor" );
static const ter_str_id ter_t_open_air( "t_open_air" );

static const trait_id trait_LEG_TENT_BRACE( "LEG_TENT_BRACE" );
static const trait_id trait_PER_SLIME( "PER_SLIME" );
static const trait_id trait_PER_SLIME_OK( "PER_SLIME_OK" );

static const trap_str_id tr_goo( "tr_goo" );
static const trap_str_id tr_portal( "tr_portal" );

// Global to smuggle data into shrapnel_calc() function without replicating it across entire map.
// Mass in kg
static float fragment_mass = 0.0001f;
// Cross-sectional area in cm^2
static float fragment_area = 0.00001f;
// Minimum velocity resulting in skin perforation according to https://www.ncbi.nlg->m.nih.gov/pubmed/7304523
static constexpr float MIN_EFFECTIVE_VELOCITY = 70.0f;
// Pretty arbitrary minimum density.  1/100 chance of a fragment passing through the given square.
static constexpr float MIN_FRAGMENT_DENSITY = 0.001f;


//reads shrapnel data as object or int
class shrapnel_reader : public generic_typed_reader<shrapnel_reader>
{
    public:
        shrapnel_data get_next( const JsonValue &val ) const {
            shrapnel_data ret;
            if( val.test_int() ) {
                ret.casing_mass = val.get_int();
                ret.recovery = 0;
                ret.drop = fuel_type_none;
                return ret;
            } else if( val.test_object() ) {
                ret.deserialize( val.get_object() );
                return ret;
            }
            val.throw_error( "shrapnel_reader element must be int or object" );
            return ret;
        }
};

void explosion_data::deserialize( const JsonObject &jo )
{
    mandatory( jo, was_loaded, "power", power );
    optional( jo, was_loaded, "distance_factor", distance_factor, 0.75f );
    optional( jo, was_loaded, "max_noise", max_noise, 90000000 );
    optional( jo, was_loaded, "fire", fire );
    optional( jo, was_loaded, "shrapnel", shrapnel, shrapnel_reader{} );
}

void shrapnel_data::deserialize( const JsonObject &jo )
{
    mandatory( jo, was_loaded, "casing_mass", casing_mass );
    optional( jo, was_loaded, "fragment_mass", fragment_mass, 0.08 ); //differs from header?
    optional( jo, was_loaded, "recovery", recovery );
    optional( jo, was_loaded, "drop", drop, itype_id::NULL_ID() );
}
namespace explosion_handler
{

int ballistic_damage( float velocity, float mass )
{
    // Damage is square root of Joules, dividing by 2000 because it's dividing by 2 and
    // converting mass from grams to kg. The initial term is simply a scaling factor.
    return 4.0 * std::sqrt( ( velocity * velocity * mass ) / 2000.0 );
}
// Calculate cross-sectional area of a steel sphere in cm^2 based on mass of fragment.
static float mass_to_area( const float mass )
{
    // Density of steel in g/cm^3
    constexpr float steel_density = 7.85f;
    float fragment_volume = ( mass / 1000.0 ) / steel_density;
    float fragment_radius = std::cbrt( ( fragment_volume * 3.0f ) / ( 4.0f * M_PI ) );
    return fragment_radius * fragment_radius * M_PI;
}

// Approximate Gurney constant for Composition B and C (in m/s instead of the usual km/s).
// Source: https://en.wikipedia.org/wiki/Gurney_equations#Gurney_constant_and_detonation_velocity
static constexpr double TYPICAL_GURNEY_CONSTANT = 2700.0;
float gurney_spherical( const double charge, const double mass )
{
    return static_cast<float>( std::pow( ( mass / charge ) + ( 3.0 / 5.0 ),
                                         -0.5 ) * TYPICAL_GURNEY_CONSTANT );
}

// (C1001) Compiler Internal Error on Visual Studio 2015 with Update 2
static void do_blast( map *m, const Creature *source, const tripoint_bub_ms &p, const float power,
                      const float distance_factor, const bool fire )
{
    const float tile_dist = 1.0f;
    const float diag_dist = M_SQRT2 * tile_dist;
    const float zlev_dist = 2.0f; // Penalty for going up/down
    // 7 3 5
    // 1 . 2
    // 6 4 8
    // 9 and 10 are up and down
    static constexpr std::array<int, 10> x_offset = { -1, 1,  0, 0,  1, -1, -1, 1, 0, 0 };
    static constexpr std::array<int, 10> y_offset = { 0, 0, -1, 1, -1,  1, -1, 1, 0, 0 };
    static constexpr std::array<int, 10> z_offset = { 0, 0,  0, 0,  0,  0,  0, 0, 1, -1 };
    const size_t max_index = 10;

    constexpr int MIN_Z = -10;
    constexpr int MAX_Z = 10;

    // Cache EMPTY_SPACE floor tiles so we know where to try to draw ramps.
    std::set<tripoint_bub_ms> crater_tiles;
    auto record_if_empty_space = [&]( const tripoint_bub_ms & tp ) {
        if( m->inbounds( tp ) &&
            m->ter( tp )->has_flag( "EMPTY_SPACE" ) ) {
            crater_tiles.insert( tp );
        }
    };

    m->bash( p, fire ? power : ( 2 * power ), true, false, false );
    record_if_empty_space( p );

    std::priority_queue< std::pair<float, tripoint_bub_ms>, std::vector< std::pair<float, tripoint_bub_ms> >, pair_greater_cmp_first >
    open;
    std::set<tripoint_bub_ms> closed;
    std::set<tripoint_bub_ms> bashed{ p };
    std::map<tripoint_bub_ms, float> dist_map;
    open.emplace( 0.0f, p );
    dist_map[p] = 0.0f;
    // Find all points to blast.
    while( !open.empty() ) {
        // Add some randomness to make it look cooler.
        const float distance = open.top().first * rng_float( 1.0f, 1.2f );
        const tripoint_bub_ms pt = open.top().second;
        open.pop();

        if( closed.count( pt ) != 0 ) {
            continue;
        }

        closed.insert( pt );

        float force = power * std::pow( distance_factor, distance );
        if( force <= 1.0f ) {
            continue;
        }

        if( m->coverage( pt ) > 0 && pt != p ) {
            const ter_id ter_here = m->ter( pt );
            const ter_t &ter_obj = ter_here.obj();
            const furn_id furn_here = m->furn( pt );
            int furniture_coverage = 0;
            int furn_attenuation = 0;
            int coverage = 0;
            int attenuation = 0;
            if( furn_here ) {
                const furn_t &furn_obj = furn_here.obj();
                furniture_coverage = furn_obj.coverage;
                if( furniture_coverage > 0 ) {
                    furn_attenuation = rng( furn_obj.bash->str_min, furn_obj.bash->str_max );
                }
            }
            coverage = ter_obj.coverage;
            if( ter_obj.bash ) {
                attenuation = rng( ter_obj.bash->str_min, ter_obj.bash->str_max );
            }
            int total_coverage = std::min( 100, furniture_coverage + coverage );
            if( total_coverage > 0 ) {
                attenuation = ( furn_attenuation * furniture_coverage + attenuation * coverage ) / total_coverage;
                attenuation = std::clamp( attenuation, 0, 100 );
            }
            force *= 1 - attenuation / 100;
        }
        // Don't propagate further.
        if( ( m->impassable( pt ) && pt != p ) || force == 0 ) {
            continue;
        }

        // Those will be used for making "shaped charges"
        // Don't check up/down (for now) - this will make 2D/3D balancing easier
        int empty_neighbors = 0;
        for( size_t i = 0; i < 8; i++ ) {
            tripoint_bub_ms dest( pt + tripoint_rel_ms( x_offset[i], y_offset[i], z_offset[i] ) );
            if( closed.count( dest ) == 0 && m->valid_move( pt, dest, false, true ) ) {
                empty_neighbors++;
            }
        }

        empty_neighbors = std::max( 1, empty_neighbors );
        // Iterate over all neighbors. Bash all of them, propagate to some.
        for( size_t i = 0; i < max_index; i++ ) {
            tripoint_bub_ms dest( pt + tripoint_rel_ms( x_offset[i], y_offset[i], z_offset[i] ) );
            if( closed.count( dest ) != 0 || !m->inbounds( dest ) ||
                m->obstructed_by_vehicle_rotation( pt, dest ) ) {
                continue;
            }

            if( bashed.count( dest ) == 0 ) {
                // Up to 200% bonus for shaped charge
                // But not if the explosion is fiery, then only half the force and no bonus.
                const float bash_force = !fire ?
                                         force + ( 2 * force / empty_neighbors ) :
                                         force / 2;

                if( z_offset[i] == 0 ) {
                    // Horizontal - no floor bashing.
                    // Fire remains false for these bash() calls as that bool is for fire ONLY,
                    // not fiery explosions.
                    m->bash( dest, bash_force, true, false, false, false, nullptr );
                    record_if_empty_space( dest );
                } else if( z_offset[i] > 0 ) {
                    // Bash upward through floor.
                    if( dest.z() <= MAX_Z ) {
                        m->bash( dest, bash_force, true, false, true, false, nullptr );
                        record_if_empty_space( dest );
                    }
                } else {
                    if( pt.z() > MIN_Z ) {
                        m->bash( pt, bash_force, true, false, true, false, nullptr );
                        record_if_empty_space( pt );
                    }
                }
            }

            float next_dist = distance;
            next_dist += ( x_offset[i] == 0 || y_offset[i] == 0 ) ? tile_dist : diag_dist;
            if( z_offset[i] != 0 ) {
                if( !m->valid_move( pt, dest, false, true ) ) {
                    continue;
                }
                next_dist += zlev_dist;
            }

            if( dist_map.count( dest ) == 0 || dist_map[dest] > next_dist ) {
                open.emplace( next_dist, dest );
                dist_map[dest] = next_dist;
            }
        }
    }

    // Finalize crater geometry after blast completes.
    if( !crater_tiles.empty() ) {
        std::map<int, std::set<tripoint_bub_ms>> crater_tiles_by_z;
        for( const auto &pt : crater_tiles ) {
            crater_tiles_by_z[pt.z()].insert( pt );
        }

        for( auto it = crater_tiles_by_z.begin(); it != crater_tiles_by_z.end(); ++it ) {
            m->finalize_crater_ramps( it->second, it->first );
        }
    }

    // Draw the explosion, but only if the explosion center is within the reality bubble.
    map &bubble_map = reality_bubble();
    if( bubble_map.inbounds( m->get_abs( p ) ) ) {
        std::map<tripoint_bub_ms, nc_color> explosion_colors;
        for( const tripoint_bub_ms &pt : closed ) {
            const tripoint_bub_ms bubble_pos( bubble_map.get_bub( m->get_abs( pt ) ) );

            if( !bubble_map.inbounds( bubble_pos ) ) {
                continue;
            }
            if( m->impassable( pt ) ) {
                continue;
            }

            const float force = power * std::pow( distance_factor, dist_map.at( pt ) );
            nc_color col = c_red;
            if( force < 10 ) {
                col = c_white;
            } else if( force < 30 ) {
                col = c_yellow;
            }

            explosion_colors[bubble_pos] = col;
        }

        draw_custom_explosion( explosion_colors );
    }

    creature_tracker &creatures = get_creature_tracker();
    Creature *mutable_source = source == nullptr ? nullptr : creatures.creature_at( source->pos_abs() );
    for( const tripoint_bub_ms &pt : closed ) {
        const float force = power * std::pow( distance_factor, dist_map.at( pt ) );
        if( force < 1.0f ) {
            continue;
        }

        if( m->has_items( pt ) ) {
            m->smash_items( pt, force, _( "force of the explosion" ) );
        }

        if( fire ) {
            int intensity = ( force > 50.0f ) + ( force > 100.0f );
            if( force > 10.0f || x_in_y( force, 10.0f ) ) {
                intensity++;
            }
            m->add_field( pt, fd_fire, intensity );
        }

        if( const optional_vpart_position vp = m->veh_at( pt ) ) {
            vp->vehicle().damage( m[0], vp->part_index(), force,
                                  fire ? damage_heat : damage_bash, false );
        }

        const tripoint_abs_ms pt_abs = m->get_abs( pt );
        Creature *critter = creatures.creature_at( pt_abs, true );
        if( critter == nullptr ) {
            continue;
        }

        add_msg_debug( debugmode::DF_EXPLOSION, "Blast hits %s with force %.1f", critter->disp_name(),
                       force );

        Character *pl = critter->as_character();
        if( pl == nullptr ) {
            add_msg_debug(
                debugmode::DF_EXPLOSION,
                "EXP dist_map=%.2f force=%.3f",
                dist_map.at( pt ),
                force
            );
            const double dmg = std::max( force - critter->get_armor_type( damage_bash,
                                         bodypart_id( "torso" ) ) / 2.0, 0.0 );
            /* 0.25 here is a magic number that gives us a decent chance to simply not take damage.
               Explosions are highly unpredictable and it's not uncommon for people to be miraculously
               unscathed, even very close to a blast. */
            const int actual_dmg = std::max( 0.0, rng_float( ( -dmg * 0.25 ) / critter->ranged_target_size(),
                                             dmg * 3 ) );
            critter->apply_damage( mutable_source, bodypart_id( "torso" ), actual_dmg );
            critter->check_dead_state( m );
            add_msg_debug( debugmode::DF_EXPLOSION, "Blast hits %s for %d damage", critter->disp_name(),
                           actual_dmg );
            continue;
        }

        if( bubble_map.inbounds( pt_abs ) ) {
            pl->add_msg_player_or_npc( m_bad, _( "You're caught in the explosion!" ),
                                       _( "<npcname> is caught in the explosion!" ) );
        }

        struct blastable_part {
            bodypart_id bp;
            float low_mul = 0.0f;
            float high_mul = 0.0f;
            float armor_mul = 0.0f;
        };

        static const std::array<blastable_part, 6> blast_parts = { {
                { bodypart_id( "torso" ), 2.0f, 3.0f, 0.5f },
                { bodypart_id( "head" ),  2.0f, 3.0f, 0.5f },
                // Hit limbs harder so it hurts more without being totally deadly.
                { bodypart_id( "leg_l" ), 2.0f, 3.5f, 0.4f },
                { bodypart_id( "leg_r" ), 2.0f, 3.5f, 0.4f },
                { bodypart_id( "arm_l" ), 2.0f, 3.5f, 0.4f },
                { bodypart_id( "arm_r" ), 2.0f, 3.5f, 0.4f },
            }
        };

        for( const blastable_part &blp : blast_parts ) {
            add_msg_debug(
                debugmode::DF_EXPLOSION,
                "EXP dist_map=%.2f force=%.3f",
                dist_map.at( pt ),
                force
            );
            const int part_dam = rng( force * blp.low_mul, force * blp.high_mul );
            const std::string hit_part_name = body_part_name_accusative( blp.bp );
            // FIXME: Hardcoded damage type
            const damage_instance dmg_instance = damage_instance( damage_bash, part_dam, 0, blp.armor_mul );
            const dealt_damage_instance result = pl->deal_damage( mutable_source, blp.bp, dmg_instance );
            const int res_dmg = result.total_damage();

            add_msg_debug( debugmode::DF_EXPLOSION, "%s for %d raw, %d actual", hit_part_name, part_dam,
                           res_dmg );
            if( res_dmg > 0 ) {
                pl->add_msg_if_player( m_bad, _( "Your %s is hit for %d damage!" ), hit_part_name, res_dmg );
            }
        }
    }
}


static std::vector<tripoint_bub_ms> shrapnel( map *m, const Creature *source,
        const tripoint_bub_ms &src, int power,
        int casing_mass, float per_fragment_mass, int range = -1 )
{
    // The gurney equation wants the total mass of the casing.
    const float fragment_velocity = gurney_spherical( power, casing_mass );
    fragment_mass = per_fragment_mass;
    fragment_area = mass_to_area( fragment_mass );
    int fragment_count = casing_mass / fragment_mass;

    // Contains all tiles reached by fragments.
    std::vector<tripoint_bub_ms> distrib;

    projectile proj;
    proj.speed = fragment_velocity;
    proj.range = range;
    proj.proj_effects.insert( ammo_effect_NULL_SOURCE );

    struct local_caches {
        cata::mdarray<fragment_cloud, point_bub_ms> obstacle_cache;
        cata::mdarray<fragment_cloud, point_bub_ms> visited_cache;
    };

    std::unique_ptr<local_caches> caches = std::make_unique<local_caches>();
    cata::mdarray<fragment_cloud, point_bub_ms> &obstacle_cache = caches->obstacle_cache;
    cata::mdarray<fragment_cloud, point_bub_ms> &visited_cache = caches->visited_cache;
    cata::mdarray<diagonal_blocks, point_bub_ms, MAPSIZE_X, MAPSIZE_Y> &blocked_cache = m->access_cache(
                src.z() ).vehicle_obstructed_cache;


    // TODO: Calculate range based on max effective range for projectiles.
    // Basically bisect between 0 and map diameter using shrapnel_calc().
    // Need to update shadowcasting to support limiting range without adjusting initial distance.
    const tripoint_range<tripoint_bub_ms> area = m->points_on_zlevel( src.z() );

    m->build_obstacle_cache( area.min(), area.max() + tripoint::south_east, obstacle_cache );

    // Shadowcasting normally ignores the origin square,
    // so apply it manually to catch monsters standing on the explosive.
    // This "blocks" some fragments, but does not apply deceleration.
    fragment_cloud initial_cloud = accumulate_fragment_cloud( obstacle_cache[src.x()][src.y()],
    { fragment_velocity, static_cast<float>( fragment_count ) }, 1 );
    visited_cache[src.x()][src.y()] = initial_cloud;
    visited_cache[src.x()][src.y()].density = static_cast<float>( fragment_count / 2.0 );

    castLightAll<fragment_cloud, fragment_cloud, shrapnel_calc, shrapnel_check,
                 update_fragment_cloud, accumulate_fragment_cloud>
                 ( visited_cache, obstacle_cache, blocked_cache, src.xy(),
                   0, initial_cloud );

    creature_tracker &creatures = get_creature_tracker();
    Creature *mutable_source = source == nullptr ? nullptr : creatures.creature_at( source->pos_abs() );
    // Now visited_caches are populated with density and velocity of fragments.
    for( const tripoint_bub_ms &target : area ) {
        fragment_cloud &cloud = visited_cache[target.x()][target.y()];
        if( cloud.density <= MIN_FRAGMENT_DENSITY ||
            cloud.velocity <= MIN_EFFECTIVE_VELOCITY ) {
            continue;
        }
        float passing_fraction = 0.0f;
        float blocked_fraction = 1.0f;
        float blocked_velocity = cloud.velocity;
        distrib.emplace_back( target );
        int damage = ballistic_damage( cloud.velocity, fragment_mass );
        // Scale damage for obstacles using obstacle_cache
        float coverage = 1.0f -
                         obstacle_cache[target.x()][target.y()].density;  // Fraction of the tile blocked. 1.0f = all, 0.0f = none.
        float attenuation =
            obstacle_cache[target.x()][target.y()].velocity; // Fraction of velocity retained for blocked fragments.
        if( coverage > 0.0f && rng_float( 0.f, 1.f ) > coverage ) {
            int partial_damage = static_cast<int>( damage * coverage * rng_float( cloud.density, 1.0f ) );
            if( optional_vpart_position vp = m->veh_at( target ) ) {
                vp->vehicle().damage( m[0], vp->part_index(), partial_damage );
            } else {
                m->bash( target, partial_damage, true );
            }
            passing_fraction = 1.0f - coverage;
            blocked_fraction = coverage;
            // Velocity for the blocked fragments
            blocked_velocity = cloud.velocity * attenuation;
        }
        const tripoint_abs_ms abs_target = m->get_abs( target );
        Creature *critter = creatures.creature_at( abs_target );

        if( damage > 0 && critter && !critter->is_dead_state() ) {
            std::poisson_distribution<> d( cloud.density );

            // TODO: Instead of dividing by 2 here, divide density's effects by 2 across the board, as it was written with 1m cube in mind and not 2 1m cubes stacked vertically

            int hits = d( rng_get_engine() ) / 2;
            double size = critter->ranged_target_size() * 10;
            double size_1 = rng_float( 1.0, 6.0 - size );
            // Add 1 to creature size as it's just very easy to get hit by shrapnel.
            double size_2 = rng_float( size, std::min( 5.0, size + 1.0 ) );
            // We need a potentially negative RNG here otherwise everything will always get hit, but let's bound it for sanity's sake.
            float lower_bound = -hits / size_1;
            float upper_bound = hits * size_2;
            float mid = ( lower_bound + upper_bound ) / 2;
            float range = ( upper_bound - lower_bound ) / 2;

            hits = rng( static_cast<int>( mid - range / 2 ), static_cast<int>( mid + range / 2 ) );
            hits = std::max( 0, hits );
            dealt_projectile_attack frag;
            frag.proj = proj;
            frag.shrapnel = true;
            frag.proj.speed = cloud.velocity;
            frag.proj.impact = damage_instance( damage_bullet, damage );

            for( int i = 0; i < hits; ++i ) {
                // Skew the missed_by because shrapnel was behaving too much like aimed shots instead of random destruction.
                float base_rng = rng_float( 0.0f, 1.0f );
                float min_rng = 0.05f;
                float max_rng = 1.0f;
                float skew = 0.12f;

                frag.missed_by = min_rng + ( max_rng - min_rng ) * std::pow( base_rng, skew );
                critter->deal_projectile_attack( m, mutable_source, frag, frag.missed_by, false );

                add_msg_debug( debugmode::DF_EXPLOSION,
                               "Shrapnel hit %s at %d m/s at a distance of %d",
                               critter->disp_name(),
                               frag.proj.speed,
                               trig_dist( src, target ) );
                add_msg_debug( debugmode::DF_EXPLOSION,
                               "Shrapnel dealt %d damage", frag.dealt_dam.total_damage() );

                if( critter->is_dead_state() ) {
                    break;
                }
            }

            auto it = frag.targets_hit[critter];
            if( reality_bubble().inbounds( abs_target ) ) {
                multi_projectile_hit_message( critter, it.first, it.second,
                                              n_gettext( "bomb fragment", "bomb fragments", it.first ) );
            }
        }
        // Resolve effects of cover on the cloud after everything else so that creatures aren't protected by cover they're standing on.
        if( coverage > 0.0f ) {
            passing_fraction = 1.0f - coverage;
            blocked_fraction = coverage;
            float velocity_proportion = obstacle_cache[target.x()][target.y()].velocity / 1.2f;
            // Reduce velocity and density for blocked fragments.
            blocked_velocity = cloud.velocity * velocity_proportion;
            // New cloud velocity = weighted combination of passing + blocked fragments.
            cloud.velocity = cloud.velocity * passing_fraction + blocked_velocity * blocked_fraction;
            // Reduce cloud density relative to coverage and attenuation.
            cloud.density = cloud.density * passing_fraction + cloud.density * blocked_fraction *
                            velocity_proportion;
        }
    }

    return distrib;
}

void explosion( const Creature *source, const tripoint_bub_ms &p, float power, float factor,
                bool fire,
                int casing_mass, float frag_mass )
{
    explosion_data data;
    data.power = power;
    data.distance_factor = factor;
    data.fire = fire;
    data.shrapnel.casing_mass = casing_mass;
    data.shrapnel.fragment_mass = frag_mass;
    explosion( source, p, data );
}

// Blocks activation of maps loaded by process_explosions. Activation would trigger a recursive
// activation of whatever caused the explosion triggering the map loading, leading to a recursive
// death spiral. Activation could also trigger further explosions leading to a cascade effect which
// is also undesirable. Such explosions should not be triggered recursively, but by a direct action.
static bool process_explosions_in_progress = false;

bool explosion_processing_active()
{
    return process_explosions_in_progress;
}

void explosion( const Creature *source, const tripoint_bub_ms &p, const explosion_data &ex )
{
    _explosions.emplace_back( source, get_map().get_abs( p ), ex );
}

void explosion( const Creature *source, map *here, const tripoint_bub_ms &p,
                const explosion_data &ex )
{
    _explosions.emplace_back( source, here->get_abs( p ), ex );
}

void _make_explosion( map *m, const Creature *source, const tripoint_bub_ms &p,
                      const explosion_data &ex )
{
    map &bubble_map = reality_bubble();

    if( bubble_map.inbounds( m->get_abs( p ) ) ) {
        tripoint_bub_ms bubble_pos = bubble_map.get_bub( m->get_abs( p ) );
        int noise = ex.power * ( ex.fire ? 2 : 10 );
        noise = ( noise > ex.max_noise ) ? ex.max_noise : noise;

        if( noise >= 30 ) {
            sounds::sound( bubble_pos, noise, sounds::sound_t::combat, _( "a huge explosion!" ), false,
                           "explosion",
                           "huge" );
        } else if( noise >= 4 ) {
            sounds::sound( bubble_pos, noise, sounds::sound_t::combat, _( "an explosion!" ), false, "explosion",
                           "default" );
        } else if( noise > 0 ) {
            sounds::sound( bubble_pos, 3, sounds::sound_t::combat, _( "a loud pop!" ), false, "explosion",
                           "small" );
        }
    }

    if( ex.distance_factor >= 1.0f ) {
        debugmsg( "called game::explosion with factor >= 1.0 (infinite size)" );
    } else if( ex.distance_factor > 0.0f && ex.power > 0.0f ) {
        /* Power in json is grams of TNT equivalent. Here we divide it by eight
           to get a reasonable damage result from pure blast (not shrapnel) with
           that in mind. */
        do_blast( m, source, p, ex.power / 8.0, ex.distance_factor, ex.fire );
    }

    const shrapnel_data &shr = ex.shrapnel;
    if( shr.casing_mass > 0 ) {
        std::vector<tripoint_bub_ms> shrapnel_locations = shrapnel( m, source, p, ex.power, shr.casing_mass,
                shr.fragment_mass );

        // If explosion drops shrapnel...
        if( shr.recovery > 0 && !shr.drop.is_null() ) {

            // Extract only passable tiles affected by shrapnel
            std::vector<tripoint_bub_ms> tiles;
            for( const tripoint_bub_ms &e : shrapnel_locations ) {
                if( m->passable( e ) ) {
                    tiles.push_back( e );
                }
            }
            const itype *fragment_drop = item_controller->find_template( shr.drop );
            int qty = shr.casing_mass * std::min( 1.0, shr.recovery / 100.0 ) /
                      to_gram( fragment_drop->weight );
            // Truncate to a random selection
            std::shuffle( tiles.begin(), tiles.end(), rng_get_engine() );
            tiles.resize( std::min( static_cast<int>( tiles.size() ), qty ) );

            for( const tripoint_bub_ms &e : tiles ) {
                m->add_item_or_charges( e, item( shr.drop, calendar::turn, item::solitary_tag{} ) );
            }
        }
    }
}

void flashbang( const tripoint_bub_ms &p, bool player_immune )
{
    draw_explosion( p, 8, c_white );
    Character &player_character = get_player_character();
    int dist = trig_dist( player_character.pos_bub(), p );
    map &here = get_map();
    if( dist <= 8 && !player_immune ) {
        if( !player_character.has_flag( STATIC( json_character_flag( "IMMUNE_HEARING_DAMAGE" ) ) ) ) {
            player_character.add_effect( effect_deaf, time_duration::from_turns( 40 - dist * 4 ) );
        }
        if( here.sees( player_character.pos_bub(), p, 8 ) ) {
            int flash_mod = 0;
            if( player_character.has_trait( trait_PER_SLIME ) ) {
                if( one_in( 2 ) ) {
                    flash_mod = 3; // Yay, you weren't looking!
                }
            } else if( player_character.has_trait( trait_PER_SLIME_OK ) ) {
                flash_mod = 8; // Just retract those and extrude fresh eyes
            } else if( player_character.has_flag( json_flag_GLARE_RESIST ) ) {
                flash_mod = 6;
            } else if( player_character.worn_with_flag( STATIC( flag_id( "BLIND" ) ) ) ||
                       player_character.worn_with_flag( STATIC( flag_id( "FLASH_PROTECTION" ) ) ) ) {
                flash_mod = 3; // Not really proper flash protection, but better than nothing
            }
            player_character.add_env_effect( effect_blind, bodypart_id( "eyes" ), ( 12 - flash_mod - dist ) / 2,
                                             time_duration::from_turns( 10 - dist ) );
        }
    }
    for( monster &critter : g->all_monsters() ) {
        if( critter.type->in_species( species_ROBOT ) ) {
            continue;
        }
        // TODO: can the following code be called for all types of creatures
        dist = trig_dist( critter.pos_bub(), p );
        if( dist <= 8 ) {
            if( dist <= 4 ) {
                critter.add_effect( effect_stunned, time_duration::from_turns( 10 - dist ) );
            }
            if( critter.has_flag( mon_flag_SEES ) && here.sees( critter.pos_bub(), p, 8 ) ) {
                critter.add_effect( effect_blind, time_duration::from_turns( 18 - dist ) );
            }
            if( critter.has_flag( mon_flag_HEARS ) ) {
                critter.add_effect( effect_deaf, time_duration::from_turns( 60 - dist * 4 ) );
            }
        }
    }
    sounds::sound( p, 12, sounds::sound_t::combat, _( "a huge boom!" ), false, "misc", "flashbang" );
    // TODO: Blind/deafen NPC
}

void shockwave( const tripoint_bub_ms &p, int radius, int force, int stun, int dam_mult,
                bool ignore_player )
{
    draw_explosion( p, radius, c_blue );

    sounds::sound( p, force * force * dam_mult / 2, sounds::sound_t::combat, _( "Crack!" ), false,
                   "misc", "shockwave" );

    for( monster &critter : g->all_monsters() ) {
        if( critter.posz() != p.z() ) {
            continue;
        }
        if( trig_dist( critter.pos_bub(), p ) <= radius ) {
            add_msg( _( "%s is caught in the shockwave!" ), critter.name() );
            g->knockback( p, critter.pos_bub(), force, stun, dam_mult );
        }
    }
    // TODO: combine the two loops and the case for avatar using all_creatures()
    for( npc &guy : g->all_npcs() ) {
        if( guy.posz() != p.z() ) {
            continue;
        }
        if( trig_dist( guy.pos_bub(),
                       p ) <= radius && !ignore_player &&
            ( !guy.has_trait( trait_LEG_TENT_BRACE ) ||
              !guy.worn.is_barefoot() ) ) {
            add_msg( _( "%s is caught in the shockwave!" ), guy.get_name() );
            g->knockback( p, guy.pos_bub(), force, stun, dam_mult );
        }
    }
    Character &player_character = get_player_character();
    if( trig_dist( player_character.pos_bub(),
                   p ) <= radius && !ignore_player &&
        ( !player_character.has_trait( trait_LEG_TENT_BRACE ) ||
          !player_character.worn.is_barefoot() ) ) {
        add_msg( m_bad, _( "You're caught in the shockwave!" ) );
        g->knockback( p, player_character.pos_bub(), force, stun, dam_mult );
    }
}

void scrambler_blast( const tripoint_bub_ms &p )
{
    if( monster *const mon_ptr = get_creature_tracker().creature_at<monster>( p ) ) {
        monster &critter = *mon_ptr;
        if( critter.has_flag( mon_flag_ELECTRONIC ) ) {
            critter.make_friendly();
        }
        add_msg( m_warning, _( "The %s sparks and begins searching for a target!" ),
                 critter.name() );
    }
}

void emp_blast( const tripoint_bub_ms &p )
{
    map &here = get_map();

    Character &player_character = get_player_character();
    const bool sight = player_character.sees( here, p );
    if( here.has_flag( ter_furn_flag::TFLAG_CONSOLE, p ) ) {
        if( sight ) {
            add_msg( _( "The %s is rendered non-functional!" ), here.tername( p ) );
        }
        here.furn_set( p, furn_f_machinery_electronic );
        return;
    }
    // TODO: More terrain effects.
    const ter_id &t = here.ter( p );
    if( t == ter_t_card_science || t == ter_t_card_military ||
        t == ter_t_card_industrial ) {
        int rn = rng( 1, 100 );
        if( rn > 92 || rn < 40 ) {
            if( sight ) {
                add_msg( _( "The card reader is rendered non-functional." ) );
            }
            here.ter_set( p, ter_t_card_reader_broken );
        }
        if( rn > 80 ) {
            if( sight ) {
                add_msg( _( "The nearby doors slide open!" ) );
            }
            for( const tripoint_bub_ms &pos : here.points_in_radius( p, 3 ) ) {
                if( here.ter( pos ) == ter_t_door_metal_locked ) {
                    here.ter_set( pos, ter_t_floor );
                }
            }
        }
        if( rn >= 40 && rn <= 80 ) {
            if( sight ) {
                add_msg( _( "Nothing happens." ) );
            }
        }
    }
    if( monster *const mon_ptr = get_creature_tracker().creature_at<monster>( p ) ) {
        monster &critter = *mon_ptr;
        if( critter.has_flag( mon_flag_ELECTRONIC ) ) {
            int deact_chance = 0;
            const itype_id mon_item_id = critter.type->revert_to_itype;
            switch( critter.get_size() ) {
                case creature_size::tiny:
                    deact_chance = 6;
                    break;
                case creature_size::small:
                    deact_chance = 3;
                    break;
                default:
                    // Currently not used, I have no idea what chances bigger bots should have,
                    // Maybe export this to json?
                    break;
            }
            if( !mon_item_id.is_empty() && deact_chance != 0 && one_in( deact_chance ) ) {
                if( sight ) {
                    add_msg( _( "The %s beeps erratically and deactivates!" ), critter.name() );
                }
                here.add_item_or_charges( p, critter.to_item() );
                for( auto &ammodef : critter.ammo ) {
                    if( ammodef.second > 0 ) {
                        here.spawn_item( p, ammodef.first, 1, ammodef.second, calendar::turn );
                    }
                }
                g->remove_zombie( critter );
            } else {
                if( sight ) {
                    add_msg( _( "The EMP blast fries the %s!" ), critter.name() );
                }
                int dam = dice( 10, 10 );
                critter.apply_damage( nullptr, bodypart_id( "torso" ), dam );
                critter.check_dead_state( &here );
                if( !critter.is_dead() && one_in( 6 ) ) {
                    critter.make_friendly();
                }
            }
        } else if( critter.has_flag( mon_flag_ELECTRIC_FIELD ) ) {
            if( !critter.has_effect( effect_emp ) ) {
                if( sight ) {
                    add_msg( m_good, _( "The %s's electrical field momentarily goes out!" ), critter.name() );
                }
                critter.add_effect( effect_emp, 3_minutes );
            } else if( critter.has_effect( effect_emp ) ) {
                int dam = dice( 3, 5 );
                if( sight ) {
                    add_msg( m_good, _( "The %s's disabled electrical field reverses polarity!" ),
                             critter.name() );
                    add_msg( m_good, _( "It takes %d damage." ), dam );
                }
                critter.add_effect( effect_emp, 1_minutes );
                critter.apply_damage( nullptr, bodypart_id( "torso" ), dam );
                critter.check_dead_state( &here );
            }
        } else if( sight ) {
            add_msg( _( "The %s is unaffected by the EMP blast." ), critter.name() );
        }
    }
    if( player_character.pos_bub() == p ) {
        if( player_character.get_power_level() > 0_kJ &&
            !player_character.has_flag( json_flag_EMP_IMMUNE ) ) {
            add_msg( m_bad, _( "The EMP blast drains your power." ) );
            int max_drain = ( player_character.get_power_level() > 1000_kJ ? 1000 : units::to_kilojoule(
                                  player_character.get_power_level() ) );
            player_character.mod_power_level( units::from_kilojoule( static_cast<std::int64_t>( -rng(
                                                  1 + max_drain / 3, max_drain ) ) ) );
        }
        // TODO: More effects?
        //e-handcuffs effects
        item_location weapon = player_character.get_wielded_item();
        if( weapon && weapon->typeId() == itype_e_handcuffs && weapon->charges > 0 ) {
            weapon->unset_flag( STATIC( flag_id( "NO_UNWIELD" ) ) );
            weapon->charges = 0;
            weapon->active = false;
            add_msg( m_good, _( "The %s on your wrists spark briefly, then release your hands!" ),
                     weapon->tname() );
        }

        for( item_location &it : player_character.all_items_loc() ) {
            // Render any electronic stuff in player's possession non-functional
            if( it->has_flag( flag_ELECTRONIC ) && !it->is_broken() &&
                !player_character.has_flag( json_flag_EMP_IMMUNE ) ) {
                add_msg( m_bad, _( "The electromagnetic pulse fries your %s!" ), it->tname() );
                it->deactivate();
                item &electronic_item = *it.get_item();
                // Items in inventory might be shielded by even thin clothing, so slightly lower chance.
                if( one_in( 5 ) ) {
                    electronic_item.set_random_fault_of_type( "shorted" );
                } else {
                    electronic_item.set_fault( fault_emp_reboot );
                }
            }
        }
    }

    for( item &it : here.i_at( p ) ) {
        // Render any electronic stuff on the ground non-functional
        if( it.has_flag( flag_ELECTRONIC ) && !it.is_broken() ) {
            if( sight ) {
                add_msg( _( "The electromagnetic pulse fries the %s!" ), it.tname() );
            }
            it.deactivate();
            if( one_in( 4 ) ) {
                it.set_random_fault_of_type( "shorted", true, false );
            } else {
                it.set_fault( fault_emp_reboot, true, false );
            }
            // map::make_active adds the item to the active item processing list, so that it can reboot without further interaction.
            item_location loc = item_location( map_cursor( p ), &it );
            here.make_active( loc );
        }
    }
    // TODO: Drain NPC energy reserves
}

void nuke( const tripoint_abs_omt &p )
{
    tinymap tmpmap;
    tmpmap.load( p, false );

    item mininuke( itype_mininuke_act );
    mininuke.set_flag( json_flag_ACTIVATE_ON_PLACE );
    tmpmap.add_item( { SEEX - 1, SEEY - 1, 0 }, mininuke );

    tmpmap.save();
}

void resonance_cascade( const tripoint_bub_ms &p )
{
    Character &player_character = get_player_character();
    const time_duration maxglow = time_duration::from_turns( 100 - 5 * trig_dist( p,
                                  player_character.pos_bub() ) );
    if( maxglow > 0_turns ) {
        const time_duration minglow = std::max( 0_turns, time_duration::from_turns( 60 - 5 * trig_dist( p,
                                                player_character.pos_bub() ) ) );
        player_character.add_effect( effect_teleglow, rng( minglow, maxglow ) * 100 );
    }
    int startx = p.x() < 8 ? 0 : p.x() - 8;
    int endx = p.x() + 8 >= SEEX * 3 ? SEEX * 3 - 1 : p.x() + 8;
    int starty = p.y() < 8 ? 0 : p.y() - 8;
    int endy = p.y() + 8 >= SEEY * 3 ? SEEY * 3 - 1 : p.y() + 8;
    tripoint_bub_ms dest( startx, starty, p.z() );
    map &here = get_map();
    for( int &i = dest.x(); i <= endx; i++ ) {
        for( int &j = dest.y(); j <= endy; j++ ) {
            switch( rng( 1, 80 ) ) {
                case 1:
                case 2:
                    emp_blast( dest );
                    break;
                case 3:
                case 4:
                case 5:
                    for( int k = i - 1; k <= i + 1; k++ ) {
                        for( int l = j - 1; l <= j + 1; l++ ) {
                            field_type_id type = fd_null;
                            switch( rng( 1, 7 ) ) {
                                case 1:
                                    type = fd_blood;
                                    break;
                                case 2:
                                    type = fd_bile;
                                    break;
                                case 3:
                                case 4:
                                    type = fd_slime;
                                    break;
                                case 5:
                                    type = fd_fire;
                                    break;
                                case 6:
                                case 7:
                                    type = fd_nuke_gas;
                                    break;
                            }
                            if( !one_in( 3 ) ) {
                                tripoint_bub_ms rng_field = { k, l, p.z() };
                                here.add_field( rng_field, type, 3 );
                            }
                        }
                    }
                    break;
                case  6:
                case  7:
                case  8:
                case  9:
                case 10:
                    here.trap_set( dest, tr_portal );
                    break;
                case 11:
                case 12:
                    here.trap_set( dest, tr_goo );
                    break;
                case 13:
                case 14:
                case 15: {
                    std::vector<MonsterGroupResult> spawn_details =
                        MonsterGroupManager::GetResultFromGroup( GROUP_NETHER );
                    for( const MonsterGroupResult &mgr : spawn_details ) {
                        g->place_critter_at( mgr.name, dest );
                    }
                }
                break;
                case 16:
                case 17:
                case 18:
                    here.destroy( dest );
                    break;
                case 19:
                    explosion( &player_character, dest, rng( 1, 10 ), rng( 0, 1 ) * rng( 0, 6 ), one_in( 4 ) );
                    break;
                default:
                    break;
            }
        }
    }
}

void process_explosions()
{
    if( _explosions.empty() ) {
        return;
    }

    // Need to copy and clear this vector before processing the explosions.
    // Part of processing in `_make_explosion` is handing out shrapnel damage,
    // which might kill monsters. That might have all sorts of consequences,
    // such as running eocs, loading new maps (via eoc) or other explosions
    // being added. There is therefore a chance that we might recursively
    // enter this function again during explosion processing, and we need to
    // guard against references becoming invalidated either by items being
    // added to the vector, or us clearing it here.
    std::vector<queued_explosion> explosions_copy( _explosions );
    _explosions.clear();

    for( const queued_explosion &ex : explosions_copy ) {
        const int safe_range = ex.data.safe_range();
        map  *bubble_map = &reality_bubble();
        const tripoint_bub_ms bubble_pos( bubble_map->get_bub( ex.pos ) );

        if( bubble_pos.x() - safe_range < 0 || bubble_pos.x() + safe_range > MAPSIZE_X ||
            bubble_pos.y() - safe_range < 0 || bubble_pos.y() + safe_range > MAPSIZE_Y ) {
            map m;
            const tripoint_abs_sm origo( project_to<coords::sm>( ex.pos ) - point_rel_sm{ HALF_MAPSIZE, HALF_MAPSIZE} );
            // Create a map centered around the explosion point to allow an explosion with a radius of up to 5 submaps
            // to be created without being cut off by the map's boundary. That also means there is no need for the map
            // to actually overlap the reality bubble, so a large explosion can be detonated without blowing up the PC
            // or have a vehicle run into a crater suddenly appearing just in front of it.
            process_explosions_in_progress = true;
            m.load( origo, true, false );
            swap_map swap( m );
            m.spawn_monsters( true, true );
            g->load_npcs( &m );
            process_explosions_in_progress = false;
            _make_explosion( &m, ex.source, m.get_bub( ex.pos ), ex.data );
            m.process_falling();
        } else {
            _make_explosion( bubble_map, ex.source, bubble_map->get_bub( ex.pos ), ex.data );
        }
    }
}

} // namespace explosion_handler

// This is only ever used to zero the cloud values, which is what makes it work.
fragment_cloud &fragment_cloud::operator=( const float &value )
{
    velocity = value;
    density = value;

    return *this;
}

bool fragment_cloud::operator==( const fragment_cloud &that ) const
{
    return velocity == that.velocity && density == that.density;
}

bool operator<( const fragment_cloud &us, const fragment_cloud &them )
{
    return us.density < them.density && us.velocity < them.velocity;
}

// Projectile velocity in air. See https://fas.org/man/dod-101/navy/docs/es310/warheads/Warheads.htm
// for a writeup of this exact calculation.
fragment_cloud shrapnel_calc( const fragment_cloud &initial,
                              const fragment_cloud &cloud,
                              const int &distance )
{
    // SWAG coefficient of drag.
    constexpr float Cd = 1.5f;
    fragment_cloud new_cloud;
    new_cloud.velocity = initial.velocity * std::exp( -cloud.velocity * ( (
                             Cd * fragment_area * distance ) /
                         ( 2.0f * fragment_mass ) ) );
    // Two effects, the accumulated proportion of blocked fragments,
    // and the inverse-square dilution of fragments with distance.
    new_cloud.density = ( initial.density * cloud.density ) / ( distance * distance );
    return new_cloud;
}
bool shrapnel_check( const fragment_cloud &cloud, const fragment_cloud &intensity )
{
    return cloud.density > 0.0 && intensity.velocity > MIN_EFFECTIVE_VELOCITY &&
           intensity.density > MIN_FRAGMENT_DENSITY;
}

void update_fragment_cloud( fragment_cloud &update, const fragment_cloud &new_value, quadrant )
{
    update = std::max( update, new_value );
}

fragment_cloud accumulate_fragment_cloud( const fragment_cloud &cumulative_cloud,
        const fragment_cloud &current_cloud, const int &distance )
{
    // Velocity is the cumulative and continuous decay of speed,
    // so it is accumulated the same way as light attenuation.
    // Density is the accumulation of discrete attenuation events encountered in the traversed squares,
    // so each term is added to the series via multiplication.
    return fragment_cloud( ( ( distance - 1 ) * cumulative_cloud.velocity + current_cloud.velocity ) /
                           distance,
                           cumulative_cloud.density * current_cloud.density );
}

float explosion_data::expected_range( float ratio ) const
{
    if( power <= 0.0f || distance_factor >= 1.0f || distance_factor <= 0.0f ) {
        return 0.0f;
    }

    // The 1.1 is because actual power drops at roughly that rate
    return std::log( ratio ) / std::log( distance_factor / 1.1f );
}

float explosion_data::power_at_range( float dist ) const
{
    if( power <= 0.0f || distance_factor >= 1.0f || distance_factor <= 0.0f ) {
        return 0.0f;
    }

    // The 1.1 is because actual power drops at roughly that rate
    return power * std::pow( distance_factor / 1.1f, dist );
}

int explosion_data::safe_range() const
{
    const float ratio = 1 / power / 2;
    return expected_range( ratio ) + 1;
}
