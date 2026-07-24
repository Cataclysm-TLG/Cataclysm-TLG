#include "caution.h"

#include <algorithm>
#include <string>

#include "bodypart.h"
#include "calendar.h"
#include "character.h"
#include "messages.h"
#include "options.h"
#include "output.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"

static const efftype_id effect_bite( "bite" );
static const efftype_id effect_bleed( "bleed" );
static const efftype_id effect_grabbed( "grabbed" );
static const efftype_id effect_infected( "infected" );

namespace
{

// Transient per-session state.  Deliberately not serialized: after loading a
// save the worst case is one repeated warning.
struct caution_state {
    time_point next_speed_warn = calendar::turn_zero;
    time_point next_hurt_warn = calendar::turn_zero;
    time_point next_bleed_warn = calendar::turn_zero;
    time_point next_bite_warn = calendar::turn_zero;
    time_point next_infect_warn = calendar::turn_zero;
    time_point next_surround_warn = calendar::turn_zero;
    time_point attack_confirmed_until = calendar::turn_zero;
    time_point danger_since = calendar::turn_zero;
    bool in_danger = false;
    bool grab_warned = false;
    bool stamina_warned = false;
    bool torso_warned = false;
    bool head_warned = false;
    int pain_tier_seen = 0;
    time_point last_check = calendar::turn_zero;

    void reset() {
        *this = caution_state();
    }
};

caution_state state;

bool alert_enabled( const std::string &toggle )
{
    return get_option<bool>( "CAUTION_MODE" ) && get_option<bool>( toggle );
}

bool speed_impaired( const Character &you )
{
    return you.get_speed() < get_option<int>( "CAUTION_SPEED_THRESHOLD" );
}

void yell( const std::string &msg )
{
    add_msg( m_bad, msg );
    popup( msg );
}

int pain_tier( const Character &you )
{
    const int pain = you.get_perceived_pain();
    if( pain >= 60 ) {
        return 3;
    } else if( pain >= 40 ) {
        return 2;
    } else if( pain >= 20 ) {
        return 1;
    }
    return 0;
}

} // namespace

namespace caution
{

void check_turn( const Character &you )
{
    if( !get_option<bool>( "CAUTION_MODE" ) ) {
        return;
    }

    const time_point now = calendar::turn;
    // Time moved backwards: a new game or an older save was loaded.
    if( now < state.last_check ) {
        state.reset();
    }
    state.last_check = now;

    const int hostiles_near = static_cast<int>( you.get_hostile_creatures( 15 ).size() );

    if( alert_enabled( "CAUTION_SPEED_WARN" ) && hostiles_near > 0 &&
        speed_impaired( you ) && now >= state.next_speed_warn ) {
        yell( string_format(
                  _( "SLOW DOWN!  Your speed is %d and there are enemies nearby.  Stop autopiloting and reassess." ),
                  you.get_speed() ) );
        const int cooldown = std::max( 3, get_option<int>( "CAUTION_WARN_COOLDOWN" ) -
                                       hostiles_near * 2 );
        state.next_speed_warn = now + time_duration::from_seconds( cooldown );
    }

    if( alert_enabled( "CAUTION_GRAB_WARN" ) ) {
        if( you.has_effect( effect_grabbed ) ) {
            if( !state.grab_warned ) {
                state.grab_warned = true;
                yell( _( "You were just grabbed!  Slow down!  There is no rush." ) );
            }
        } else {
            state.grab_warned = false;
        }
    }

    if( alert_enabled( "CAUTION_STAMINA_WARN" ) ) {
        const int pct = get_option<int>( "CAUTION_STAMINA_PCT" );
        const int stamina_pct = 100 * you.get_stamina() / std::max( 1, you.get_stamina_max() );
        if( hostiles_near > 0 && stamina_pct < pct && !state.stamina_warned ) {
            state.stamina_warned = true;
            yell( _( "You're winded — you swing slower and dodge worse.  Disengage now, don't try to finish the fight." ) );
        } else if( stamina_pct > pct + 10 ) {
            state.stamina_warned = false;
        }
    }

    if( alert_enabled( "CAUTION_BLEED_WARN" ) ) {
        if( you.has_effect( effect_bleed ) ) {
            if( now >= state.next_bleed_warn ) {
                yell( _( "You are bleeding.  Stop and bandage it — ten seconds and a rag beat bleeding out." ) );
                state.next_bleed_warn = now + 20_seconds;
            }
        } else {
            state.next_bleed_warn = now;
        }
    }

    if( alert_enabled( "CAUTION_BITE_WARN" ) ) {
        if( you.has_effect( effect_bite ) ) {
            if( now >= state.next_bite_warn ) {
                yell( _( "You've been bitten!  Clean and dress that wound — you have hours, not days, before it infects." ) );
                state.next_bite_warn = now + 5_minutes;
            }
        } else {
            state.next_bite_warn = now;
        }
        if( you.has_effect( effect_infected ) ) {
            if( now >= state.next_infect_warn ) {
                yell( _( "The wound is INFECTED.  Antibiotics or antiseptic, now.  This kills in about a day." ) );
                state.next_infect_warn = now + 1_minutes;
            }
        } else {
            state.next_infect_warn = now;
        }
    }

    if( alert_enabled( "CAUTION_SURROUND_WARN" ) && now >= state.next_surround_warn &&
        you.get_hostile_creatures( 4 ).size() >= 3 ) {
        yell( _( "You're being flanked — get to a chokepoint or run.  Full health means nothing when you're surrounded." ) );
        state.next_surround_warn = now + 10_seconds;
    }

    if( alert_enabled( "CAUTION_PAIN_WARN" ) ) {
        const int tier = pain_tier( you );
        if( tier > state.pain_tier_seen ) {
            if( tier == 1 ) {
                yell( _( "Pain is creeping in — your speed and stats are starting to suffer." ) );
            } else if( tier == 2 ) {
                yell( _( "You are in serious pain — reflexes and speed are dulled.  Painkillers, then reconsider this fight." ) );
            } else {
                yell( _( "You are in agony and badly impaired.  Whatever you're doing, it can wait.  Get safe and rest." ) );
            }
        }
        state.pain_tier_seen = tier;
    }

    if( alert_enabled( "CAUTION_LIMB_WARN" ) ) {
        const int pct = get_option<int>( "CAUTION_LIMB_PCT" );
        const bodypart_id torso( "torso" );
        const bodypart_id head( "head" );
        const int torso_pct = 100 * you.get_part_hp_cur( torso ) /
                              std::max( 1, you.get_part_hp_max( torso ) );
        const int head_pct = 100 * you.get_part_hp_cur( head ) /
                             std::max( 1, you.get_part_hp_max( head ) );
        if( torso_pct < pct && !state.torso_warned ) {
            state.torso_warned = true;
            yell( _( "Your torso is wrecked — one bad hit ends this.  Why are you still here?" ) );
        } else if( torso_pct > pct + 10 ) {
            state.torso_warned = false;
        }
        if( head_pct < pct && !state.head_warned ) {
            state.head_warned = true;
            yell( _( "Your head is badly hurt — the next hit could be the last.  Disengage." ) );
        } else if( head_pct > pct + 10 ) {
            state.head_warned = false;
        }
    }

    if( alert_enabled( "CAUTION_FIGHT_WARN" ) ) {
        if( hostiles_near > 0 ) {
            if( !state.in_danger ) {
                state.in_danger = true;
                state.danger_since = now;
            }
            const time_duration limit =
                time_duration::from_seconds( get_option<int>( "CAUTION_FIGHT_SECONDS" ) );
            if( now - state.danger_since >= limit ) {
                yell( _( "You've been in danger a long time — check your HP, your stamina, your exits.  Is this still worth it?" ) );
                // Re-warn two minutes later if the danger persists.
                state.danger_since = now - limit + 2_minutes;
            }
        } else {
            state.in_danger = false;
        }
    }
}

bool confirm_attack( const Character &you )
{
    if( !alert_enabled( "CAUTION_ATTACK_CONFIRM" ) || !speed_impaired( you ) ) {
        return true;
    }
    const time_point now = calendar::turn;
    if( now < state.attack_confirmed_until ) {
        return true;
    }
    if( query_yn( _( "Your speed is %d — you are in no shape to trade blows.  Attack anyway?" ),
                  you.get_speed() ) ) {
        state.attack_confirmed_until = now + 20_seconds;
        return true;
    }
    return false;
}

void on_avatar_hurt( const Character &you )
{
    if( !alert_enabled( "CAUTION_HURT_WARN" ) || !speed_impaired( you ) ) {
        return;
    }
    const time_point now = calendar::turn;
    if( now < state.next_hurt_warn ) {
        return;
    }
    state.next_hurt_warn = now + 5_seconds;
    yell( _( "You were hurt, are you sure you want to keep fighting?" ) );
}

} // namespace caution
