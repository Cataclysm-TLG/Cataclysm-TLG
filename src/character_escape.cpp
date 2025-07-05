#include <algorithm>
#include <memory>
#include <vector>

#include "bodypart.h"
#include "calendar.h"
#include "character.h"
#include "character_attire.h"
#include "character_martial_arts.h"
#include "creature_tracker.h"
#include "damage.h"
#include "debug.h"
#include "effect.h"
#include "enums.h"
#include "flag.h"
#include "item.h"
#include "item_pocket.h"
#include "map.h"
#include "map_iterator.h"
#include "martialarts.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "output.h"
#include "pimpl.h"
#include "point.h"
#include "rng.h"
#include "trap.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"

static const character_modifier_id
character_modifier_grab_break_limb_mod( "grab_break_limb_mod" );

static const efftype_id effect_beartrap( "beartrap" );
static const efftype_id effect_crushed( "crushed" );
static const efftype_id effect_downed( "downed" );
static const efftype_id effect_heavysnare( "heavysnare" );
static const efftype_id effect_in_pit( "in_pit" );
static const efftype_id effect_incorporeal( "incorporeal" );
static const efftype_id effect_lightsnare( "lightsnare" );
static const efftype_id effect_webbed( "webbed" );

static const flag_id json_flag_GRAB( "GRAB" );
static const flag_id json_flag_NO_GRAB( "NO_GRAB" );
static const flag_id json_flag_PIT( "PIT" );

static const itype_id itype_rope_6( "rope_6" );
static const itype_id itype_snare_trigger( "snare_trigger" );

static const json_character_flag json_flag_DOWNED_RECOVERY( "DOWNED_RECOVERY" );
static const json_character_flag json_flag_WALL_CLING( "WALL_CLING" );

static const limb_score_id limb_score_balance( "balance" );
static const limb_score_id limb_score_grip( "grip" );
static const limb_score_id limb_score_manip( "manip" );

static const skill_id skill_melee( "melee" );
static const skill_id skill_unarmed( "unarmed" );

static const ter_str_id ter_t_pit( "t_pit" );
static const ter_str_id ter_t_pit_glass( "t_pit_glass" );
static const ter_str_id ter_t_pit_spiked( "t_pit_spiked" );

static const trait_id trait_SLIMY( "SLIMY" );
static const trait_id trait_VISCOUS( "VISCOUS" );

bool Character::can_escape_trap( int difficulty, bool manip = false ) const
{
    int chance = get_arm_str();
    chance *= manip ? get_limb_score( limb_score_manip ) : get_limb_score( limb_score_grip );
    return x_in_y( chance, difficulty );
}

void Character::try_remove_downed()
{

    /** @EFFECT_DEX increases chance to stand up when knocked down */
    /** @EFFECT_ARM_STR increases chance to stand up when knocked down, slightly */
    // Downed reduces balance score to 10% unless resisted, multiply to compensate
    int chance = ( get_dex() + get_arm_str() / 2.0 ) * get_limb_score( limb_score_balance ) * 10.0;
    // Always 2,5% chance to stand up
    chance += has_flag( json_flag_DOWNED_RECOVERY ) ? 20 : 1;
    if( !x_in_y( chance, 40 ) ) {
        add_msg_if_player( _( "You struggle to stand." ) );
    } else {
        add_msg_player_or_npc( m_good,
                               has_flag( json_flag_DOWNED_RECOVERY ) ? _( "You deftly roll to your feet." ) : _( "You stand up." ),
                               has_flag( json_flag_DOWNED_RECOVERY ) ? _( "<npcname> deftly rolls to their feet." ) :
                               _( "<npcname> stands up." ) );
        remove_effect( effect_downed );
    }
}

void Character::try_remove_bear_trap()
{
    /* Real bear traps can't be removed without the proper tools or immense strength; eventually this should
       allow normal players two options: removal of the limb or removal of the trap from the ground
       (at which point the player could later remove it from the leg with the right tools).
       As such we are currently making it a bit easier for players and NPC's to get out of bear traps.
    */
    // If is riding, then despite the character having the effect, it is the mounted creature that escapes.
    if( is_avatar() && is_mounted() ) {
        auto *mon = mounted_creature.get();
        if( mon->type->melee_dice * mon->type->melee_sides >= 18 ) {
            if( x_in_y( mon->type->melee_dice * mon->type->melee_sides, 200 ) ) {
                map &here = get_map();
                mon->remove_effect( effect_beartrap );
                remove_effect( effect_beartrap );
                add_msg( _( "The %s escapes the bear trap!" ), mon->get_name() );

                here.spawn_item( mon->pos(), "beartrap" );
            } else {
                add_msg_if_player( m_bad,
                                   _( "Your %s tries to free itself from the bear trap, but can't get loose!" ), mon->get_name() );
            }
        }
    } else {
        if( can_escape_trap( 100 ) ) {
            remove_effect( effect_beartrap );
            add_msg_player_or_npc( m_good, _( "You free yourself from the bear trap!" ),
                                   _( "<npcname> frees themselves from the bear trap!" ) );
            map &here = get_map();
            here.spawn_item( pos(), "beartrap" );
        } else {
            add_msg_if_player( m_bad,
                               _( "You try to free yourself from the bear trap, but can't get loose!" ) );
        }
    }
}

void Character::try_remove_lightsnare()
{
    if( is_mounted() ) {
        auto *mon = mounted_creature.get();
        if( x_in_y( mon->type->melee_dice * mon->type->melee_sides, 12 ) ) {
            map &here = get_map();
            mon->remove_effect( effect_lightsnare );
            remove_effect( effect_lightsnare );
            add_msg( _( "The %s escapes the light snare!" ), mon->get_name() );
            here.spawn_item( pos(), "light_snare_kit" );
        }
    } else {
        if( can_escape_trap( 12 ) ) {
            map &here = get_map();
            remove_effect( effect_lightsnare );
            add_msg_player_or_npc( m_good, _( "You free yourself from the light snare!" ),
                                   _( "<npcname> frees themselves from the light snare!" ) );
            here.spawn_item( pos(), "light_snare_kit" );
        } else {
            add_msg_if_player( m_bad,
                               _( "You try to free yourself from the light snare, but can't get loose!" ) );
        }
    }
}

void Character::try_remove_heavysnare()
{
    map &here = get_map();
    if( is_mounted() ) {
        auto *mon = mounted_creature.get();
        if( mon->type->melee_dice * mon->type->melee_sides >= 7 ) {
            if( x_in_y( mon->type->melee_dice * mon->type->melee_sides, 32 ) ) {
                mon->remove_effect( effect_heavysnare );
                remove_effect( effect_heavysnare );
                here.spawn_item( pos_bub(), itype_rope_6 );
                here.spawn_item( pos_bub(), itype_snare_trigger );
                add_msg( _( "The %s escapes the heavy snare!" ), mon->get_name() );
            }
        }
    } else {
        if( can_escape_trap( 32 - dex_cur, true ) ) {
            remove_effect( effect_heavysnare );
            add_msg_player_or_npc( m_good, _( "You free yourself from the heavy snare!" ),
                                   _( "<npcname> frees themselves from the heavy snare!" ) );
            item rope( "rope_6", calendar::turn );
            item snare( "snare_trigger", calendar::turn );
            here.add_item_or_charges( pos_bub(), rope );
            here.add_item_or_charges( pos_bub(), snare );
        } else {
            add_msg_if_player( m_bad,
                               _( "You try to free yourself from the heavy snare, but can't get loose!" ) );
        }
    }
}

void Character::try_remove_crushed()
{
    if( can_escape_trap( 100 ) ) {
        remove_effect( effect_crushed );
        add_msg_player_or_npc( m_good, _( "You free yourself from the rubble!" ),
                               _( "<npcname> frees themselves from the rubble!" ) );
    } else {
        add_msg_if_player( m_bad, _( "You try to free yourself from the rubble, but can't get loose!" ) );
    }
}

bool Character::try_remove_grab( bool attacking )
{
    if( is_mounted() ) {
        // Use the same calc as monster::move_effect
        auto *mon = mounted_creature.get();
        if( mon->has_effect_with_flag( json_flag_GRAB ) ) {
            for( const effect &grab : mon->get_effects_with_flag( json_flag_GRAB ) ) {
                const efftype_id effid = grab.get_effect_type()->id;
                if( !x_in_y( mon->type->melee_skill + mon->type->melee_damage.total_damage(),
                             mon->get_effect_int( effid ) ) ) {
                    add_msg( m_bad, _( "Your %s tries to break free, but fails!" ), mon->get_name() );
                    return false;
                } else {
                    add_msg( m_good, _( "Your %s breaks free from the grab!" ), mon->get_name() );
                    mon->remove_effect( effid );
                }
            }
        } else {
            // No chivalrous zombies letting you go after pulling you off your horse
            if( one_in( 4 ) ) {
                add_msg( m_bad, _( "You are pulled from your %s!" ), mon->get_name() );
                forced_dismount();
            }
        }
    } else if( has_effect_with_flag( json_flag_GRAB ) ) {
        // Need to know who's around for targeted removal
        map &here = get_map();
        creature_tracker &creatures = get_creature_tracker();

        // No need to recalculate it in-loop, breaking previous grabs doesn't change skills
        float skill_factor = std::min( 0.8f,
                                       std::max( std::max( static_cast<float>( get_skill_level( skill_melee ) ) / 10, 0.1f ),
                                               std::max( static_cast<float>( get_skill_level( skill_unarmed ) ) / 8, 0.1f ) ) );
        int grab_break_factor = has_grab_break_tec() ? 10 : 0;
        const tripoint_range<tripoint_bub_ms> &surrounding = here.points_in_radius( pos_bub(), 1, 0 );

        // Iterate through all our grabs and attempt to break them one by one
        for( const effect &eff : get_effects_with_flag( json_flag_GRAB ) ) {
            float escape_chance = 1.0f;
            float grabber_roll = static_cast<float>( eff.get_intensity() );
            // We need to figure out which Creature is responsible for this grab early for good messaging
            // For now, one grabber per limb TODO: handle multiple grabbers and decrement intensity
            Creature *grabber = nullptr;
            for( const tripoint_bub_ms loc : surrounding ) {
                Character *guy = creatures.creature_at<Character>( loc );
                if( guy && guy->grab_1.grabbed_part == eff.get_bp() ) {
                    add_msg_debug( debugmode::DF_CHARACTER, "Grabber %s found", guy->disp_name() );
                    grabber = guy;
                    // Because of how turns are processed, we need to let character-initiated
                    // grabs live for at least 1 second before we get to remove them, otherwise
                    // we will instantly break player grabs.
                    time_point start_time = eff.get_start_time();
                    time_duration effect_dur_elapsed = calendar::turn - start_time;
                    int speed_factor = 1 + std::round( 100 / get_speed() );
                    if( to_turns<int>( effect_dur_elapsed ) <= std::max( 1, speed_factor ) ) {
                        continue;
                    }
                    break;
                }
                monster *mon = creatures.creature_at<monster>( loc );
                if( mon && mon->is_grabbing( eff.get_bp().id() ) ) {
                    add_msg_debug( debugmode::DF_MATTACK, "Grabber %s found", mon->name() );
                    grabber = mon;
                    break;
                }
            }
            // Something went wrong, remove grab to be sure and throw an error
            if( grabber == nullptr ) {
                remove_effect( eff.get_id(), eff.get_bp() );
                add_msg_debug( debugmode::DF_CHARACTER, "Orphan grab found and removed" );
                continue;
            }
            // Follower NPCs should almost always assume the player is doing something
            // for a good reason, as long as it's not killing them. Being grabbed is
            // not directly harmful, so they usually won't resist.
            if( !grabber->is_monster() && grabber->as_character()->is_avatar() && as_npc()->is_player_ally() ) {
                continue;
            }

            if( has_effect( effect_incorporeal ) ) {
                if( grabber->is_monster() ) {
                    grabber->as_monster()->remove_grab( eff.get_bp().id() );
                    remove_effect( eff.get_id(), eff.get_bp() );
                    continue;
                } else {
                    grabber->as_character()->grab_1.clear();
                    remove_effect( eff.get_id(), eff.get_bp() );
                    continue;
                }
            }

            // Short out the check when attacking after removing any orphan grabs
            if( attacking ) {
                add_msg_debug( debugmode::DF_CHARACTER,
                               "Grab break check triggered by an attack, only removing orphan grabs allowed" );
                continue;
            }

            bool torn_pocket = false;
            std::vector<item_pocket *> pd = worn.grab_drop_pockets( eff.get_bp() );
            if( !pd.empty() ) {
                // choose an item to be ripped off
                int index = rng( 0, pd.size() - 1 );
                int chance = rng( 0, get_effect_int( eff.get_id(), eff.get_bp() ) );
                int sturdiness = rng( 0, pd[index]->get_pocket_data()->ripoff * 10 );
                add_msg_debug( debugmode::DF_MATTACK, "Tearoff pocket %s chosen, sturdiness %d, tearing chance %d",
                               pd[index]->get_name(), sturdiness, chance );
                // the item is ripped off your character
                if( sturdiness < chance ) {
                    pd[index]->spill_contents( adjacent_tile() );
                    add_msg_player_or_npc( m_bad,
                                           _( "As you struggle to escape the grab something comes loose and falls to the ground!" ),
                                           _( "As <npcname> struggles to escape the grab something comes loose and falls to the ground!" ) );
                    if( is_avatar() ) {
                        popup( _( "As you struggle to escape the grab, something comes loose and falls to the ground!" ) );
                    }
                    torn_pocket = true;
                }
            }

            // Limb factor we check directly
            // Stats might get modified by certain grabby effects, check them to be safe
            float stat_factor = std::max( get_str() / 2, get_dex() / 3 );
            float limb_factor = get_modifier( character_modifier_grab_break_limb_mod );
            escape_chance = ( skill_factor * limb_factor ) * 100 + stat_factor;
            grabber_roll = std::max( grabber_roll, escape_chance ) + rng( 1, 10 );
            escape_chance += grab_break_factor;
            if( has_trait( trait_SLIMY ) || has_trait( trait_VISCOUS ) ) {
                const float slime_factor = worn.clothing_wetness_mult( eff.get_bp() ) * 8;
                // Slime offers an 8% bonus to escaping from a grab on a naked body part.
                // Slime exudes from the skin and will only soak through clothes according to their combined breathability and coverage.
                // Since the attacker is grabbing at the outermost layer, that 8% is multiplied by clothing_wetness_mult for that body part.
                // TODO: Negate this bonus for artificial limbs.
                escape_chance += slime_factor;
                add_msg_debug( debugmode::DF_CHARACTER,
                               "%s is slimy, escape chance increased by %f",
                               eff.get_bp()->name, slime_factor );
            }
            add_msg_debug( debugmode::DF_MATTACK,
                           "Attempting to break grab on %s, grab strength roll %.1f, skill factor %.1f, limb factor %.1f, stat bonus %.1f, grab break bonus %d, escape chance %.1f, final chance %.1f %%",
                           eff.get_bp()->name, grabber_roll, skill_factor, limb_factor, stat_factor, grab_break_factor,
                           escape_chance,
                           escape_chance * 100 / grabber_roll );
            if( torn_pocket ) {
                escape_chance *= 1.5f;
                add_msg_debug( debugmode::DF_MATTACK,
                               "Pocket torn off in the attempt, escape chance increased to %.1f",
                               escape_chance * 100 / eff.get_intensity() );
            }

            // Every attempt burns some stamina - maybe some moves?
            burn_energy_arms( -5 * eff.get_intensity() );
            if( x_in_y( escape_chance, grabber_roll ) ) {
                if( grabber->is_monster() ) {
                    grabber->as_monster()->remove_grab( eff.get_bp().id() );
                    add_msg_debug( debugmode::DF_MATTACK, "Removed grab effect %s from grabber %s",
                                   eff.get_bp()->name, grabber->as_monster()->name() );
                } else {
                    grabber->as_character()->grab_1.clear();
                    add_msg_debug( debugmode::DF_CHARACTER, "Removed grab effect %s from grabber %s",
                                   eff.get_bp()->name, grabber->disp_name() );
                }

                if( grab_break_factor > 0 ) {
                    add_msg_if_player( m_info, martial_arts_data->get_grab_break( *this ).avatar_message.translated(),
                                       grabber->disp_name() );
                } else {
                    add_msg_player_or_npc( m_good, _( "You break %s grab on your %s!" ),
                                           _( "<npcname> breaks %s grab on their %s!" ), grabber->disp_name( true ),
                                           eff.get_bp()->name );
                }
                // Remove only this one grab
                remove_effect( eff.get_id(), eff.get_bp() );
            } else {
                add_msg_player_or_npc( m_bad, _( "You try to break %s grab on your %s, but fail!" ),
                                       _( "<npcname> tries to break out of the grab, but fails!" ), grabber->disp_name( true ),
                                       eff.get_bp()->name );
            }
        }

        // We have attempted to break every grab but have failed to break at least one
        // Attacks only get the abbreviated grab check, grabs don't prevent attacking
        if( has_effect_with_flag( json_flag_GRAB ) && !attacking ) {
            return false;
        }
    }
    return true;
}

void Character::try_remove_webs()
{
    if( is_mounted() ) {
        auto *mon = mounted_creature.get();
        if( x_in_y( mon->type->melee_dice * mon->type->melee_sides,
                    6 * get_effect_int( effect_webbed ) ) ) {
            add_msg( _( "The %s breaks free of the webs!" ), mon->get_name() );
            mon->remove_effect( effect_webbed );
            remove_effect( effect_webbed );
        }
    } else if( can_escape_trap( 6 * get_effect_int( effect_webbed ) ) ) {
        add_msg_player_or_npc( m_good, _( "You free yourself from the webs!" ),
                               _( "<npcname> frees themselves from the webs!" ) );
        remove_effect( effect_webbed );
    } else {
        add_msg_if_player( _( "You try to free yourself from the webs, but can't get loose!" ) );
    }
}

void Character::try_remove_impeding_effect()
{
    for( const effect &eff : get_effects_with_flag( flag_EFFECT_IMPEDING ) ) {
        const efftype_id &eff_id = eff.get_id();
        if( is_mounted() ) {
            auto *mon = mounted_creature.get();
            if( x_in_y( mon->type->melee_dice * mon->type->melee_sides,
                        6 * get_effect_int( eff_id ) ) ) {
                add_msg( _( "The %s breaks free!" ), mon->get_name() );
                mon->remove_effect( eff_id );
                remove_effect( eff_id );
            }
            /** @EFFECT_STR increases chance to escape webs */
        } else if( can_escape_trap( 6 * get_effect_int( eff_id ) ) ) {
            add_msg_player_or_npc( m_good, _( "You free yourself!" ),
                                   _( "<npcname> frees themselves!" ) );
            remove_effect( eff_id );
        } else {
            add_msg_if_player( _( "You try to free yourself, but can't!" ) );
        }
    }
}

bool Character::move_effects( bool attacking, tripoint dest_loc )
{
    if( has_effect( effect_downed ) && !attacking ) {
        try_remove_downed();
        return false;
    }
    if( has_effect( effect_webbed ) ) {
        try_remove_webs();
        return false;
    }
    if( has_effect( effect_lightsnare ) && !attacking ) {
        try_remove_lightsnare();
        return false;

    }
    if( has_effect( effect_heavysnare ) && !attacking ) {
        try_remove_heavysnare();
        return false;
    }
    if( has_effect( effect_beartrap ) && !attacking ) {
        try_remove_bear_trap();
        return false;
    }
    if( has_effect( effect_crushed ) ) {
        try_remove_crushed();
        return false;
    }
    if( has_effect_with_flag( flag_EFFECT_IMPEDING ) ) {
        try_remove_impeding_effect();
        return false;
    }

    // Below this point are things that allow for movement if they succeed

    // Currently we only have one thing that forces movement if you succeed, should we get more
    // than this will need to be reworked to only have success effects if /all/ checks succeed
    if( has_effect( effect_in_pit ) ) {
        map &here = get_map();
        trap trap_here = here.tr_at( pos() );
        trap trap_there = here.tr_at( dest_loc );
        const ter_id target_ter = here.ter( dest_loc );

        // Adjacent pits are contiguous. We're not climbing out, we're just walking around at the bottom.
        if( has_effect( effect_in_pit ) && !trap_there.has_flag( json_flag_PIT ) &&
            target_ter != ter_t_pit && target_ter != ter_t_pit_spiked && target_ter != ter_t_pit_glass ) {
            /** @EFFECT_DEX increases chance to escape pit, slightly.
             * And of course if we can spider climb, we can just casually leave.
             */
            if( !has_flag( json_flag_WALL_CLING ) && !can_escape_trap( 40 - dex_cur / 2 ) ) {
                add_msg_if_player( m_bad, _( "You try to escape the pit, but slip back in." ) );
                return false;
            } else {
                add_msg_player_or_npc( m_good, _( "You escape the pit!" ),
                                       _( "<npcname> escapes the pit!" ) );
                remove_effect( effect_in_pit );
            }
        }
    }
    // Attempt to break grabs, only check for orphan grabs on attack
    if( has_flag( json_flag_GRAB ) ) {
        return try_remove_grab( attacking );
    }
    return true;
}

void Character::wait_effects( bool attacking )
{
    if( has_effect( effect_downed ) ) {
        try_remove_downed();
        return;
    }
    if( has_effect( effect_beartrap ) ) {
        try_remove_bear_trap();
        return;
    }
    if( has_effect( effect_lightsnare ) ) {
        try_remove_lightsnare();
        return;
    }
    if( has_effect( effect_heavysnare ) ) {
        try_remove_heavysnare();
        return;
    }
    if( has_effect( effect_webbed ) ) {
        try_remove_webs();
        return;
    }
    if( has_effect_with_flag( flag_EFFECT_IMPEDING ) ) {
        try_remove_impeding_effect();
        return;
    }
    if( has_flag( json_flag_GRAB ) && !attacking && !try_remove_grab( false ) ) {
        return;
    }
}

