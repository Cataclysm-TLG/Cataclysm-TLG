#include "addiction.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "calendar.h"
#include "character.h"
#include "creature.h"
#include "debug.h"
#include "dialogue.h"
#include "effect.h"
#include "effect_on_condition.h"
#include "enums.h"
#include "flexbuffer_json-inl.h"
#include "flexbuffer_json.h"
#include "generic_factory.h"
#include "init.h"
#include "json_error.h"
#include "rng.h"
#include "talker.h"
#include "text_snippets.h"

static const efftype_id effect_amphetamine_eff( "amphetamine_eff" );
static const efftype_id effect_cig( "cig" );
static const efftype_id effect_nausea( "nausea" );
static const efftype_id effect_opioid_eff( "opioid_eff" );
static const efftype_id effect_withdrawal_alcohol( "withdrawal_alcohol" );
static const efftype_id effect_withdrawal_alcohol_detoxed( "withdrawal_alcohol_detoxed" );
static const efftype_id effect_withdrawal_alcohol_timer( "withdrawal_alcohol_timer" );
static const efftype_id effect_withdrawal_nicotine( "withdrawal_nicotine" );
static const efftype_id effect_withdrawal_nicotine_detoxed( "withdrawal_nicotine_detoxed" );
static const efftype_id effect_withdrawal_nicotine_timer( "withdrawal_nicotine_timer" );
static const efftype_id effect_withdrawal_opioid( "withdrawal_opioid" );
static const efftype_id effect_withdrawal_opioid_detoxed( "withdrawal_opioid_detoxed" );
static const efftype_id effect_withdrawal_opioid_timer( "withdrawal_opioid_timer" );
static const efftype_id effect_hallu( "hallu" );
static const efftype_id effect_shakes( "shakes" );

static const morale_type morale_craving_alcohol( "morale_craving_alcohol" );
static const morale_type morale_craving_cannabis( "morale_craving_cannabis" );
static const morale_type morale_craving_cocaine( "morale_craving_cocaine" );
static const morale_type morale_craving_diazepam( "morale_craving_diazepam" );
static const morale_type morale_craving_nicotine( "morale_craving_nicotine" );
static const morale_type morale_craving_opioid( "morale_craving_opioid" );
static const morale_type morale_craving_speed( "morale_craving_speed" );

namespace
{

generic_factory<add_type> add_type_factory( "addiction" );

} // namespace

/** @relates string_id */
template<>
const add_type &string_id<add_type>::obj() const
{
    return add_type_factory.obj( *this );
}

/** @relates string_id */
template<>
bool string_id<add_type>::is_valid() const
{
    return add_type_factory.is_valid( *this );
}

void add_type::load_add_types( const JsonObject &jo, const std::string &src )
{
    add_type_factory.load( jo, src );
}

void add_type::finalize_all()
{
    add_type_factory.finalize();
}

void add_type::reset()
{
    add_type_factory.reset();
}

const std::vector<add_type> &add_type::get_all()
{
    return add_type_factory.get_all();
}

static bool alcohol_add( Character &u, int in )
{
    static time_point last_alc_dream = calendar::turn_zero;
    const bool recent_dream = ( calendar::turn - last_alc_dream < 3_hours );
    const morale_type morale_type = morale_craving_alcohol;

    int timer_int = std::min( in / 3, 3 );

    bool ret = false;
    if( x_in_y( in, 24 ) && !u.in_sleep_state() ) {
        if( !u.has_effect( effect_withdrawal_alcohol_detoxed ) &&
            ( !u.has_effect( effect_withdrawal_alcohol_timer ) ) ) {
            u.add_effect( effect_withdrawal_alcohol_timer, 24_hours * timer_int, false, timer_int );
            ret = true;
        }
    }

    // If you're not drunk, you want to be.
    if( rng( 0, 100 ) < in &&
        ( !u.in_sleep_state() || !recent_dream ) ) {
        const std::string msg_1 =
            ( u.in_sleep_state() ? "addict_alcohol_mild_asleep" : "addict_alcohol_mild_awake" );
        u.add_msg_if_player( m_warning,
                             SNIPPET.random_from_category( msg_1 ).value_or( translation() ).translated() );
        if( u.in_sleep_state() ) {
            last_alc_dream = calendar::turn;
        } else {
            u.add_morale( morale_type, -35, -4 * in, 1_hours, 30_minutes, true );
        }

        ret = true;

        // If you're in active withdrawal, you feel horrible!
    } else if( u.has_effect( effect_withdrawal_alcohol ) && in > 7 && rng( 0, 80 ) < in &&
               ( !u.in_sleep_state() || !recent_dream ) ) {
        const std::string msg_2 =
            ( u.in_sleep_state() ? "addict_alcohol_strong_asleep" : "addict_alcohol_strong_awake" );
        u.add_msg_if_player( m_bad,
                             SNIPPET.random_from_category( msg_2 ).value_or( translation() ).translated() );
        if( u.in_sleep_state() ) {
            last_alc_dream = calendar::turn;
        } else {
            u.add_morale( morale_type, -35, -4 * in, 1_hours, 30_minutes, true );
        }
        ret = true;
    }
    return ret;
}

static bool benzodiazepine_add( Character &u, int in )
{
    static time_point last_dia_dream = calendar::turn_zero;
    const bool recent_dream = ( calendar::turn - last_dia_dream < 3_hours );
    const morale_type morale_type = morale_craving_diazepam;
    bool ret = false;
    if( one_in( 40 ) && rng( 0, 30 ) < in && ( !u.in_sleep_state() || !recent_dream ) ) {
        const std::string msg_1 = ( u.in_sleep_state() ? "addict_diazepam_mild_asleep" :
                                    "addict_diazepam_mild_awake" );
        u.add_msg_if_player( m_warning,
                             SNIPPET.random_from_category( msg_1 ).value_or( translation() ).translated() );
        u.add_morale( morale_type, -35, -4 * in, 1_hours, 30_minutes, true );
        ret = true;
        if( u.in_sleep_state() ) {
            last_dia_dream = calendar::turn;
        }
    } else if( rng( 8, 600 ) < in && ( !u.in_sleep_state() || !recent_dream ) ) {
        const std::string msg_2 = ( u.in_sleep_state() ? "addict_diazepam_strong_asleep" :
                                    "addict_diazepam_strong_awake" );
        u.add_msg_if_player( m_bad,
                             SNIPPET.random_from_category( msg_2 ).value_or( translation() ).translated() );
        u.add_morale( morale_type, -35, -4 * in, 1_hours, 30_minutes, true );
        u.add_effect( effect_shakes, 5_minutes );
        ret = true;
        if( u.in_sleep_state() ) {
            last_dia_dream = calendar::turn;
        }
    } else if( !u.has_effect( effect_hallu ) && rng( 10, 1600 ) < in ) {
        u.add_effect( effect_hallu, 6_hours );
        ret = true;
    }
    return ret;
}

static bool cocaine_add( Character &u, int in )
{
    static time_point last_coke_dream = calendar::turn_zero;
    const bool recent_dream = ( calendar::turn - last_coke_dream < 3_hours );
    const std::string cur_msg = ( u.in_sleep_state() ? "addict_coke_asleep" : "addict_coke_awake" );
    const morale_type morale_type = morale_craving_cocaine;
    bool ret = false;
    auto run_addict_eff = [&ret, &morale_type]( Character & u, int in, const std::string & snippet ) {
        u.add_msg_if_player( m_warning,
                             SNIPPET.random_from_category( snippet ).value_or( translation() ).translated() );
        if( u.in_sleep_state() ) {
            last_coke_dream = calendar::turn;
        } else {
            u.add_morale( morale_type, -20, -4 * in, 1_hours, 30_minutes, true );
        }
        ret = true;
    };

    if( one_in( 1000 - 20 * in ) && ( !u.in_sleep_state() || !recent_dream ) ) {
        run_addict_eff( u, in, cur_msg );
    }
    if( dice( 2, 80 ) <= in && ( !u.in_sleep_state() || !recent_dream ) ) {
        run_addict_eff( u, in, cur_msg );
    }
    return ret;
}

/************** Builtin effects **************/

static bool nicotine_effect( Character &u, addiction &add )
{
    if( u.has_effect( effect_cig ) ) {
        add.sated = 0_turns;
        return false;
    }
    static time_point last_dream = calendar::turn_zero;
    const int in = std::min( 20, add.intensity );
    int timer_int = std::min( in / 3, 3 );

    bool ret = false;
    if( x_in_y( in, 48 ) && !u.in_sleep_state() ) {
        if( !u.has_effect( effect_withdrawal_nicotine_detoxed ) &&
            ( !u.has_effect( effect_withdrawal_nicotine_timer ) ) ) {
            u.add_effect( effect_withdrawal_nicotine_timer, 32_hours * timer_int, false, timer_int );
            ret = true;
        }
    }

    if( x_in_y( in, 60 ) && ( !u.in_sleep_state() || calendar::turn - last_dream > 3_hours ) ) {
        if( u.in_sleep_state() ) {
            last_dream = calendar::turn;
        }
        const bool strong = rng( 0, 10 ) < in;
        const std::string msg =
            !strong ?
            ( u.in_sleep_state() ? "addict_nicotine_mild_asleep" : "addict_nicotine_mild_awake" ) :
            ( u.in_sleep_state() ? "addict_nicotine_strong_asleep" : "addict_nicotine_strong_awake" );
        u.add_msg_if_player( m_warning,
                             SNIPPET.random_from_category( msg ).value_or( translation() ).translated() );
        if( !u.in_sleep_state() ) {
            u.add_morale( morale_craving_nicotine, -15, -3 * in, 1_hours, 30_minutes, true );
        }

        ret = true;
    }

    if( one_in( 1800 - 10 * in ) ) {
        u.mod_fatigue( 1 );
    }
    return ret;
}

static bool cannabis_effect( Character &u, addiction &add )
{
    static time_point last_dream = calendar::turn_zero;
    const int in = std::min( 20, add.intensity );

    bool ret = false;

    if( x_in_y( in, 80 ) && ( !u.in_sleep_state() || calendar::turn - last_dream > 3_hours ) ) {
        if( u.in_sleep_state() ) {
            last_dream = calendar::turn;
        }
        const bool strong = rng( 0, 14 ) < in;
        const std::string msg =
            !strong ?
            ( u.in_sleep_state() ? "addict_cannabis_mild_asleep" : "addict_cannabis_mild_awake" ) :
            ( u.in_sleep_state() ? "addict_cannabis_strong_asleep" : "addict_cannabis_strong_awake" );
        u.add_msg_if_player( m_warning,
                             SNIPPET.random_from_category( msg ).value_or( translation() ).translated() );
        if( !u.in_sleep_state() ) {
            u.add_morale( morale_craving_cannabis, -5, -2 * in, 1_hours, 30_minutes, true );
        }

        ret = true;
    }

    if( one_in( 90 - 3 * in ) ) {
        u.mod_fatigue( -1 );
    }
    if( in > 5 && one_in( 90 - in ) ) {
        u.add_effect( effect_nausea, ( in - 5 ) * 1_minutes );
    }
    return ret;
}

static bool alcohol_effect( Character &u, addiction &add )
{
    const int in = std::min( 20, add.intensity );
    return alcohol_add( u, in );
}

static bool diazepam_effect( Character &u, addiction &add )
{
    const int in = std::min( 20, add.intensity );
    return benzodiazepine_add( u, in );
}

static bool opioid_effect( Character &u, addiction &add )
{
    if( u.has_effect( effect_opioid_eff ) ) {
        add.sated = 0_turns;
        u.remove_effect( effect_shakes );
        return false;
    }

    bool ret;

    const int in = std::min( 20, add.intensity );
    int timer_int = std::min( in / 3, 3 );

    if( x_in_y( in, 24 ) && !u.in_sleep_state() ) {
        if( !u.has_effect( effect_withdrawal_opioid_detoxed ) &&
            ( !u.has_effect( effect_withdrawal_opioid_timer ) ) ) {
            u.add_effect( effect_withdrawal_opioid_timer, 48_hours * timer_int, false, timer_int );
            ret = true;
        }
    }
    static time_point last_dream = calendar::turn_zero;
    if( u.get_pain() < in * 2 ) {
        u.mod_pain( 1 );
    }
    if( one_in( 40 ) && dice( 2, 30 ) < in &&
        ( !u.in_sleep_state() || calendar::turn - last_dream > 4_hours ) ) {
        const std::string msg =
            u.in_sleep_state() ? "addict_opioid_mild_asleep" : "addict_opioid_mild_awake";
        u.add_msg_if_player( m_bad,
                             SNIPPET.random_from_category( msg ).value_or( translation() ).translated() );
        if( u.in_sleep_state() ) {
            last_dream = calendar::turn;
        } else {
            u.add_morale( morale_craving_opioid, -40, -4 * in, 1_hours, 30_minutes, true );
        }
        ret = true;
    } else if( one_in( 40 ) && dice( 2, 30 ) < in &&
               ( !u.in_sleep_state() || calendar::turn - last_dream > 4_hours ) ) {
        const std::string msg =
            u.in_sleep_state() ? "addict_opioid_strong_asleep" : "addict_opioid_strong_awake";
        u.add_msg_if_player( m_bad,
                             SNIPPET.random_from_category( msg ).value_or( translation() ).translated() );
        if( u.in_sleep_state() ) {
            last_dream = calendar::turn;
        } else {
            u.add_morale( morale_craving_opioid, -30, -4 * in, 1_hours, 30_minutes, true );
        }
        ret = true;
    } else if( one_in( 50 ) && dice( 3, 50 ) < in ) {
        u.vomit();
        ret = true;
    }
    return ret;
}

static bool amphetamine_effect( Character &u, addiction &add )
{
    if( u.has_effect( effect_amphetamine_eff ) ) {
        add.sated = 0_turns;
        return false;
    }
    static time_point last_dream = calendar::turn_zero;
    const int in = std::min( add.intensity, 20 );
    bool ret = false;
    if( dice( 2, 100 ) < in && ( !u.in_sleep_state() || calendar::turn - last_dream > 3_hours ) ) {
        const std::string msg =
            u.in_sleep_state() ? "addict_amphetamine_mild_asleep" : "addict_amphetamine_mild_awake";
        u.add_msg_if_player( m_warning,
                             SNIPPET.random_from_category( msg ).value_or( translation() ).translated() );
        if( u.in_sleep_state() ) {
            last_dream = calendar::turn;
        } else {
            u.add_morale( morale_craving_speed, -25, -4 * in, 1_hours, 30_minutes, true );
        }
        ret = true;
    } else if( one_in( 20 ) && dice( 2, 80 ) < in &&
               ( !u.in_sleep_state() || calendar::turn - last_dream > 3_hours ) ) {
        if( u.in_sleep_state() ) {
            last_dream = calendar::turn;
        }
        const std::string msg =
            u.in_sleep_state() ? "addict_amphetamine_strong_asleep" : "addict_amphetamine_strong_awake";
        u.add_msg_if_player( m_warning,
                             SNIPPET.random_from_category( msg ).value_or( translation() ).translated() );
        u.add_morale( morale_craving_speed, -25, -4 * in, 1_hours, 30_minutes, true );
        ret = true;
    } else if( one_in( 50 ) && dice( 2, 100 ) < in &&
               ( !u.in_sleep_state() || calendar::turn - last_dream > 3_hours ) ) {
        if( u.in_sleep_state() ) {
            last_dream = calendar::turn;
        }
        const std::string msg =
            u.in_sleep_state() ? "addict_amphetamine_paralysis_asleep" : "addict_amphetamine_paralysis_awake";
        u.add_msg_if_player( m_warning,
                             SNIPPET.random_from_category( msg ).value_or( translation() ).translated() );
        u.mod_moves( -( u.in_sleep_state() ? 6000 : 300 ) );
        u.wake_up();
        ret = true;
    }
    return ret;
}

static bool cocaine_effect( Character &u, addiction &add )
{
    const int in = std::min( 20, add.intensity );
    return cocaine_add( u, in );
}

/*********************************************/

static const std::map<std::string, std::function<bool( Character &, addiction & )>> builtin_map {
    {"cannabis_effect",    ::cannabis_effect},
    {"nicotine_effect",    ::nicotine_effect},
    {"alcohol_effect",     ::alcohol_effect},
    {"diazepam_effect",    ::diazepam_effect},
    {"opioid_effect",      ::opioid_effect},
    {"amphetamine_effect", ::amphetamine_effect},
    {"cocaine_effect",     ::cocaine_effect}
};

bool addiction::run_effect( Character &u )
{
    bool ret = false;
    if( !type->get_effect().is_null() ) {
        dialogue d( get_talker_for( u ), nullptr );
        ret = type->get_effect()->activate( d );
    } else {
        auto iter = builtin_map.find( type->get_builtin() );
        if( iter != builtin_map.end() ) {
            ret = iter->second.operator()( u, *this );
        } else {
            debugmsg( "invalid builtin \"%s\" for addiction_type \"%s\"", type->get_builtin(), type.c_str() );
        }
    }
    return ret;
}

void add_type::load( const JsonObject &jo, std::string_view )
{
    mandatory( jo, was_loaded, "name", _name );
    mandatory( jo, was_loaded, "type_name", _type_name );
    mandatory( jo, was_loaded, "description", _desc );
    optional( jo, false, "sated", _sated, 2_hours );
    optional( jo, was_loaded, "craving_morale", _craving_morale, morale_type::NULL_ID() );
    optional( jo, was_loaded, "effect_on_condition", _effect );
    optional( jo, was_loaded, "builtin", _builtin );
}

void add_type::check_add_types()
{
    for( const add_type &add : add_type::get_all() ) {
        if( add._effect.is_null() == add._builtin.empty() ) {
            debugmsg( "addiction_type \"%s\" defines %s effect_on_condition %s builtin.  Addictions must define either field, but not both.",
                      add.id.c_str(), add._builtin.empty() ? "neither" : "both", add._builtin.empty() ? "or" : "and" );
        }
        if( !add._builtin.empty() && builtin_map.find( add._builtin ) == builtin_map.end() ) {
            debugmsg( "invalid builtin \"%s\" for addiction_type \"%s\"", add._builtin, add.id.c_str() );
        }
    }
}

std::string add_type_legacy_conv( std::string const &v )
{
    if( v == "CAFFEINE" ) {
        return "caffeine";
    } else if( v == "ALCOHOL" ) {
        return "alcohol";
    } else if( v == "SLEEP" ) {
        return "sleeping pill";
    } else if( v == "PKILLER" ) {
        return "opioid";
    } else if( v == "SPEED" ) {
        return "amphetamine";
    } else if( v == "CIG" ) {
        return "nicotine";
    } else if( v == "COKE" ) {
        return "cocaine";
    } else if( v == "DIAZEPAM" ) {
        return "diazepam";
    } else if( v == "MARLOSS_R" ) {
        return "marloss_r";
    } else if( v == "MARLOSS_B" ) {
        return "marloss_b";
    } else if( v == "MARLOSS_Y" ) {
        return "marloss_y";
    }
    return {};
}
