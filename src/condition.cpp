#include "condition.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "action.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_utility.h"
#include "character.h"
#include "coordinates.h"
#include "debug.h"
#include "dialogue.h"
#include "dialogue_helpers.h"
#include "effect.h"
#include "effect_on_condition.h"
#include "enum_conversions.h"
#include "enum_traits.h"
#include "faction.h"
#include "field.h"
#include "flag.h"
#include "flexbuffer_json-inl.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "generic_factory.h"
#include "global_vars.h"
#include "item.h"
#include "item_category.h"
#include "item_location.h"
#include "json_error.h"
#include "line.h"
#include "map.h"
#include "map_iterator.h"
#include "mapdata.h"
#include "martialarts.h"
#include "math_parser.h"
#include "math_parser_type.h"
#include "mission.h"
#include "mtype.h"
#include "mutation.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmap.h"
#include "overmapbuffer.h"
#include "point.h"
#include "popup.h"
#include "profession.h"
#include "ranged.h"
#include "recipe_groups.h"
#include "rng.h"
#include "string_formatter.h"
#include "talker.h"
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "units.h"
#include "vehicle.h"
#include "viewer.h"
#include "vpart_position.h"
#include "weather.h"
#include "widget.h"
#include "worldfactory.h"

class Creature;
class basecamp;
class recipe;
struct mapgen_arguments;

static const efftype_id effect_currently_busy( "currently_busy" );

static const json_character_flag json_flag_MUTATION_THRESHOLD( "MUTATION_THRESHOLD" );

namespace
{
struct deferred_math {
    JsonObject jo;
    std::string str;
    math_type_t type;
    std::shared_ptr<math_exp> exp;

    deferred_math( JsonObject const &jo_, std::string_view str_, math_type_t type_ )
        : jo( jo_ ), str( str_ ), type( type_ ), exp( std::make_shared<math_exp>() ) {

        jo.allow_omitted_members();
    }

    void _validate_type() const;
};

struct condition_parser {
    using condition_func = conditional_t::func;
    using f_t = condition_func( * )( const JsonObject &, std::string_view );
    using f_t_beta = condition_func( * )( const JsonObject &, std::string_view, bool );
    using f_t_simple = condition_func( * )();
    using f_t_beta_simple = condition_func( * )( bool );

    condition_parser( std::string_view key_alpha_, jarg arg_, f_t f_ ) : key_alpha( key_alpha_ ),
        arg( arg_ ), f( f_ ) {}
    condition_parser( std::string_view key_alpha_, std::string_view key_beta_, jarg arg_,
                      f_t_beta f_ ) : key_alpha( key_alpha_ ), key_beta( key_beta_ ), arg( arg_ ), f_beta( f_ ) {
        has_beta = true;
    }
    condition_parser( std::string_view key_alpha_, f_t_simple f_ ) : key_alpha( key_alpha_ ),
        f_simple( f_ ) {}
    condition_parser( std::string_view key_alpha_, std::string_view key_beta_,
                      f_t_beta_simple f_ ) : key_alpha( key_alpha_ ), key_beta( key_beta_ ), f_beta_simple( f_ ) {
        has_beta = true;
    }

    bool check( const JsonObject &jo, bool beta = false ) const {
        std::string_view key = beta ? key_beta : key_alpha;
        if( ( ( arg & jarg::member ) && jo.has_member( key ) ) ||
            ( ( arg & jarg::object ) && jo.has_object( key ) ) ||
            ( ( arg & jarg::string ) && jo.has_string( key ) ) ||
            ( ( arg & jarg::array ) && jo.has_array( key ) ) ) {
            return true;
        }
        return false;
    }

    bool has_beta = false;
    std::string_view key_alpha;
    std::string_view key_beta;
    jarg arg;
    f_t f;
    f_t_beta f_beta;
    f_t_simple f_simple;
    f_t_beta_simple f_beta_simple;
};

std::queue<deferred_math> &get_deferred_math()
{
    static std::queue<deferred_math> dfr_math;
    return dfr_math;
}

void clear_deferred_math()
{
    std::queue<deferred_math> empty;
    get_deferred_math().swap( empty );
}

std::shared_ptr<math_exp> &defer_math( JsonObject const &jo, std::string_view str,
                                       math_type_t type )
{
    get_deferred_math().emplace( jo, str, type );
    return get_deferred_math().back().exp;
}

}  // namespace

std::string get_talk_varname( const JsonObject &jo, std::string_view member,
                              bool check_value, dbl_or_var &default_val )
{
    if( check_value && !( jo.has_string( "value" ) ||
                          jo.has_array( "possible_values" ) ) ) {
        jo.throw_error( "invalid " + std::string( member ) + " condition in " + jo.str() );
    }
    const std::string &var_basename = jo.get_string( std::string( member ) );
    const std::string &type_var = jo.get_string( "type", "" );
    const std::string &var_context = jo.get_string( "context", "" );
    default_val = get_dbl_or_var( jo, "default", false );
    return "npctalk_var" + ( type_var.empty() ? "" : "_" + type_var ) + ( var_context.empty() ? "" : "_"
            + var_context ) + "_" + var_basename;
}

std::string get_talk_var_basename( const JsonObject &jo, std::string_view member,
                                   bool check_value )
{
    if( check_value && !( jo.has_string( "value" ) ||
                          jo.has_array( "possible_values" ) ) ) {
        jo.throw_error( "invalid " + std::string( member ) + " condition in " + jo.str() );
    }
    const std::string &var_basename = jo.get_string( std::string( member ) );
    return var_basename;
}

dbl_or_var_part get_dbl_or_var_part( const JsonValue &jv, std::string_view member, bool required,
                                     double default_val )
{
    dbl_or_var_part ret_val;
    if( jv.test_float() ) {
        ret_val.dbl_val = jv.get_float();
    } else if( jv.test_object() ) {
        JsonObject jo = jv.get_object();
        if( jo.has_array( "math" ) ) {
            ret_val.math_val.emplace();
            ret_val.math_val->from_json( jo, "math", math_type_t::ret );
        } else {
            jo.allow_omitted_members();
            ret_val.var_val = read_var_info( jo );
        }
    } else if( required ) {
        jv.throw_error( "No valid value for " + std::string( member ) );
    } else {
        ret_val.dbl_val = default_val;
    }
    return ret_val;
}

dbl_or_var get_dbl_or_var( const JsonObject &jo, std::string_view member, bool required,
                           double default_val )
{
    dbl_or_var ret_val;
    if( jo.has_array( member ) ) {
        JsonArray ja = jo.get_array( member );
        ret_val.min = get_dbl_or_var_part( ja.next_value(), member );
        ret_val.max = get_dbl_or_var_part( ja.next_value(), member );
        ret_val.pair = true;
    } else if( required ) {
        ret_val.min = get_dbl_or_var_part( jo.get_member( member ), member, required, default_val );
    } else {
        if( jo.has_member( member ) ) {
            ret_val.min = get_dbl_or_var_part( jo.get_member( member ), member, required, default_val );
        } else {
            ret_val.min.dbl_val = default_val;
        }
    }
    return ret_val;
}

duration_or_var_part get_duration_or_var_part( const JsonValue &jv, const std::string_view &member,
        bool required, time_duration default_val )
{
    duration_or_var_part ret_val;
    if( jv.test_string() ) {
        if( jv.get_string() == "infinite" ) {
            ret_val.dur_val = time_duration::from_turns( calendar::INDEFINITELY_LONG );
        } else {
            ret_val.dur_val = read_from_json_string<time_duration>( jv, time_duration::units );
        }
    } else if( jv.test_int() ) {
        ret_val.dur_val = time_duration::from_turns( jv.get_float() );
    } else if( jv.test_object() ) {
        JsonObject jo = jv.get_object();
        if( jo.has_array( "math" ) ) {
            ret_val.math_val.emplace();
            ret_val.math_val->from_json( jo, "math", math_type_t::ret );
        } else {
            jo.allow_omitted_members();
            ret_val.var_val = read_var_info( jo );
        }
    } else if( required ) {
        jv.throw_error( "No valid value for " + std::string( member ) );
    } else {
        ret_val.dur_val = default_val;
    }
    return ret_val;
}

duration_or_var get_duration_or_var( const JsonObject &jo, const std::string_view &member,
                                     bool required,
                                     time_duration default_val )
{
    duration_or_var ret_val;
    if( jo.has_array( member ) ) {
        JsonArray ja = jo.get_array( member );
        ret_val.min = get_duration_or_var_part( ja.next_value(), member );
        ret_val.max = get_duration_or_var_part( ja.next_value(), member );
        ret_val.pair = true;
    } else if( required ) {
        ret_val.min = get_duration_or_var_part( jo.get_member( member ), member, required, default_val );
    } else {
        if( jo.has_member( member ) ) {
            ret_val.min = get_duration_or_var_part( jo.get_member( member ), member, required, default_val );
        } else {
            ret_val.min.dur_val = default_val;
        }
    }
    return ret_val;
}

str_or_var get_str_or_var( const JsonValue &jv, std::string_view member, bool required,
                           std::string_view default_val )
{
    str_or_var ret_val;
    if( jv.test_string() ) {
        ret_val.str_val = jv.get_string();
    } else if( jv.test_object() ) {
        ret_val = get_str_or_var( jv.get_object(), member, default_val );
    } else if( required ) {
        jv.throw_error( "No valid value for " + std::string( member ) );
    } else {
        ret_val.str_val = default_val;
    }
    return ret_val;
}

str_or_var get_str_or_var( const JsonObject &jo, std::string_view,
                           std::string_view default_val )
{
    str_or_var ret_val;
    if( jo.has_member( "mutator" ) ) {
        // if we have a mutator then process that here.
        ret_val.function = conditional_t::get_get_string( jo );
    } else {
        ret_val.var_val = read_var_info( jo );
        ret_val.default_val = default_val;
    }
    return ret_val;
}

static bool json_object_read( const JsonObject &jo, translation &v )
{
    try {
        v.deserialize( jo );
        return true;
    } catch( const JsonError & ) {
        return false;
    }
}

translation_or_var get_translation_or_var( const JsonValue &jv, std::string_view member,
        bool required, const translation &default_val )
{
    translation_or_var ret_val;
    if( jv.test_object() ) {
        ret_val = get_translation_or_var( jv.get_object(), member, default_val );
    } else {
        translation str_val;
        if( jv.read( str_val ) ) {
            ret_val.str_val = str_val;
        } else if( required ) {
            jv.throw_error( "No valid value for " + std::string( member ) );
        } else {
            ret_val.str_val = default_val;
        }
    }
    return ret_val;
}

translation_or_var get_translation_or_var( const JsonObject &jo, std::string_view,
        const translation &default_val )
{
    translation_or_var ret_val;
    translation str_val;
    if( json_object_read( jo, str_val ) ) {
        ret_val.str_val = str_val;
    } else {
        if( jo.has_member( "mutator" ) ) {
            // if we have a mutator then process that here.
            ret_val.function = conditional_t::get_get_translation( jo );
        } else {
            ret_val.var_val = read_translation_var_info( jo );
            ret_val.default_val = default_val;
        }
    }
    return ret_val;
}

str_translation_or_var get_str_translation_or_var(
    const JsonValue &jv, std::string_view member, bool required,
    std::string_view str_default_val, const translation &translation_default_val )
{
    str_translation_or_var ret_val;
    if( jv.test_object() ) {
        const JsonObject &jo = jv.get_object();
        if( jo.get_bool( "i18n", false ) ) {
            ret_val.val = get_translation_or_var( jo, member, translation_default_val );
        } else {
            ret_val.val = get_str_or_var( jo, member, str_default_val );
        }
    } else {
        ret_val.val = get_str_or_var( jv, member, required, str_default_val );
    }
    return ret_val;
}

tripoint_abs_ms get_tripoint_from_var( std::optional<var_info> var, const_dialogue const &d,
                                       bool is_npc )
{
    tripoint_abs_ms target_pos = get_map().getglobal( d.const_actor( is_npc )->pos() );
    if( var.has_value() ) {
        std::string value = read_var_value( var.value(), d );
        if( !value.empty() ) {
            target_pos = tripoint_abs_ms( tripoint::from_string( value ) );
        }
    }
    if( !d.has_actor( is_npc ) ) {
        debugmsg( "Tried to access location of invalid %s talker.  %s", is_npc ? "beta" : "alpha",
                  d.get_callstack() );
        return tripoint_abs_ms( tripoint_min );
    }
    return get_map().getglobal( d.const_actor( is_npc )->pos() );
}

template<class T>
static T abstract_read_var_info_no_translation( std::string && );

template<>
std::string abstract_read_var_info_no_translation( std::string &&s )
{
    return std::move( s );
}

template<>
translation abstract_read_var_info_no_translation( std::string &&s )
{
    return no_translation( s );
}

template<class T>
static abstract_var_info<T> abstract_read_var_info( const JsonObject &jo )
{
    T default_val;
    dbl_or_var empty;
    var_type type;
    std::string name;
    if( jo.has_member( "default_str" ) ) {
        jo.read( "default_str", default_val );
    } else if( jo.has_string( "default" ) ) {
        std::string tmp = std::to_string( to_turns<int>( read_from_json_string<time_duration>
                                          ( jo.get_member( "default" ), time_duration::units ) ) );
        default_val = abstract_read_var_info_no_translation<T>( std::move( tmp ) );
    } else if( jo.has_float( "default" ) ) {
        std::string tmp = std::to_string( jo.get_float( "default" ) );
        default_val = abstract_read_var_info_no_translation<T>( std::move( tmp ) );
    }

    if( jo.has_string( "var_name" ) ) {
        const std::string &type_var = jo.get_string( "type", "" );
        const std::string &var_context = jo.get_string( "context", "" );
        name = "npctalk_var_" + type_var + ( type_var.empty() ? "" : "_" ) + var_context +
               ( var_context.empty() ? "" : "_" )
               + jo.get_string( "var_name" );
    }
    if( jo.has_member( "u_val" ) ) {
        type = var_type::u;
        if( name.empty() ) {
            name = get_talk_varname( jo, "u_val", false, empty );
        }
    } else if( jo.has_member( "npc_val" ) ) {
        type = var_type::npc;
        if( name.empty() ) {
            name = get_talk_varname( jo, "npc_val", false, empty );
        }
    } else if( jo.has_member( "global_val" ) ) {
        type = var_type::global;
        if( name.empty() ) {
            name = get_talk_varname( jo, "global_val", false, empty );
        }
    } else if( jo.has_member( "var_val" ) ) {
        type = var_type::var;
        if( name.empty() ) {
            name = get_talk_varname( jo, "var_val", false, empty );
        }
    } else if( jo.has_member( "context_val" ) ) {
        type = var_type::context;
        if( name.empty() ) {
            name = get_talk_varname( jo, "context_val", false, empty );
        }
    } else if( jo.has_member( "faction_val" ) ) {
        type = var_type::faction;
        if( name.empty() ) {
            name = get_talk_varname( jo, "faction_val", false, empty );
        }
    } else if( jo.has_member( "party_val" ) ) {
        type = var_type::party;
        if( name.empty() ) {
            name = get_talk_varname( jo, "party_val", false, empty );
        }
    } else {
        jo.throw_error( "Invalid variable type." );
    }
    return abstract_var_info<T>( type, name, default_val );
}

var_info read_var_info( const JsonObject &jo )
{
    return abstract_read_var_info<std::string>( jo );
}

translation_var_info read_translation_var_info( const JsonObject &jo )
{
    return abstract_read_var_info<translation>( jo );
}

void write_var_value( var_type type, const std::string &name, dialogue *d,
                      const std::string &value, int call_depth )
{
    global_variables &globvars = get_globals();
    std::string ret;
    var_info vinfo( var_type::global, "" );
    switch( type ) {
        case var_type::global:
            globvars.set_global_value( name, value );
            break;
        case var_type::var:
            ret = d->get_value( name );
            vinfo = process_variable( ret );
            if( call_depth > 1000 ) {
                debugmsg( "Possible infinite loop detected: var_val points to itself or forms a cycle.  %s->%s %s",
                          name, vinfo.name, d->get_callstack() );
            } else {
                write_var_value( vinfo.type, vinfo.name, d, value,
                                 call_depth + 1 );
            }
            break;
        case var_type::u:
            if( d->has_alpha ) {
                d->actor( false )->set_value( name, value );
            } else {
                debugmsg( "Tried to use an invalid alpha talker.  %s", d->get_callstack() );
            }
            break;
        case var_type::npc:
            if( d->has_beta ) {
                d->actor( true )->set_value( name, value );
            } else {
                debugmsg( "Tried to use an invalid beta talker.  %s", d->get_callstack() );
            }
            break;
        case var_type::faction:
            debugmsg( "Not implemented yet." );
            break;
        case var_type::party:
            debugmsg( "Not implemented yet." );
            break;
        case var_type::context:
            d->set_value( name, value );
            break;
        default:
            debugmsg( "Invalid type." );
            break;
    }
}

void write_var_value( var_type type, const std::string &name, const_dialogue const &d,
                      const std::string &value )
{
    switch( type ) {
        case var_type::global:
            get_globals().set_global_value( name, value );
            break;
        case var_type::context:
        case var_type::var:
        case var_type::u:
        case var_type::npc:
        case var_type::faction:
        case var_type::party:
            debugmsg( "Only global variables can be assigned from an eval function.\n%s", d.get_callstack() );
            break;
        default:
            debugmsg( "Invalid type." );
            break;
    }
}

void write_var_value( var_type type, const std::string &name, dialogue *d,
                      double value )
{
    // NOLINTNEXTLINE(cata-translate-string-literal)
    write_var_value( type, name, d, string_format( "%g", value ) );
}

static bodypart_id get_bp_from_str( const std::string &ctxt )
{
    bodypart_id bid = bodypart_str_id::NULL_ID();
    if( !ctxt.empty() ) {
        bid = bodypart_id( ctxt );
        if( !bid.is_valid() ) {
            bid = bodypart_str_id::NULL_ID();
        }
    }
    return bid;
}

void read_condition( const JsonObject &jo, const std::string &member_name,
                     conditional_t::func &condition, bool default_val )
{
    const auto null_function = [default_val]( const_dialogue const & ) {
        return default_val;
    };

    if( !jo.has_member( member_name ) ) {
        condition = null_function;
    } else if( jo.has_string( member_name ) ) {
        const std::string type = jo.get_string( member_name );
        conditional_t sub_condition( type );
        condition = [sub_condition]( const_dialogue const & d ) {
            return sub_condition( d );
        };
    } else if( jo.has_object( member_name ) ) {
        JsonObject con_obj = jo.get_object( member_name );
        conditional_t sub_condition( con_obj );
        condition = [sub_condition]( const_dialogue const & d ) {
            return sub_condition( d );
        };
    } else {
        jo.throw_error_at( member_name, "invalid condition syntax" );
    }
}

void finalize_conditions()
{
    std::queue<deferred_math> &dfr = get_deferred_math();
    while( !dfr.empty() ) {
        deferred_math &math = dfr.front();
        try {
            math.exp->parse( math.str, false );
        } catch( math::exception const &ex ) {
            JsonObject jo{ std::move( math.jo ) };
            clear_deferred_math();
            jo.throw_error_at( "math", ex.what() );
        }
        math._validate_type();
        dfr.pop();
    }
}

namespace conditional_fun
{
namespace
{

conditional_t::func f_has_any_trait( const JsonObject &jo, std::string_view member, bool is_npc )
{
    std::vector<str_or_var> traits_to_check;
    for( JsonValue jv : jo.get_array( member ) ) {
        traits_to_check.emplace_back( get_str_or_var( jv, member ) );
    }
    return [traits_to_check, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        for( const str_or_var &trait : traits_to_check ) {
            if( actor->has_trait( trait_id( trait.evaluate( d ) ) ) ) {
                return true;
            }
        }
        return false;
    };
}

conditional_t::func f_has_trait( const JsonObject &jo, std::string_view member, bool is_npc )
{
    str_or_var trait_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [trait_to_check, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_trait( trait_id( trait_to_check.evaluate( d ) ) );
    };
}

conditional_t::func f_is_trait_purifiable( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var trait_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [trait_to_check, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->is_trait_purifiable( trait_id( trait_to_check.evaluate( d ) ) );
    };
}

conditional_t::func f_has_visible_trait( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var trait_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [trait_to_check, is_npc]( const_dialogue const & d ) {
        const_talker const *observer = d.const_actor( !is_npc );
        const_talker const *observed = d.const_actor( is_npc );
        int visibility_cap = observer->get_const_character()->get_mutation_visibility_cap(
                                 observed->get_const_character() );
        bool observed_has = observed->has_trait( trait_id( trait_to_check.evaluate( d ) ) );
        const mutation_branch &mut_branch = trait_id( trait_to_check.evaluate( d ) ).obj();
        bool is_visible = mut_branch.visibility > 0 && mut_branch.visibility >= visibility_cap;
        return observed_has && is_visible;
    };
}

conditional_t::func f_has_martial_art( const JsonObject &jo, std::string_view member,
                                       bool is_npc )
{
    str_or_var style_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [style_to_check, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->knows_martial_art( matype_id( style_to_check.evaluate( d ) ) );
    };
}

conditional_t::func f_has_flag( const JsonObject &jo, std::string_view member,
                                bool is_npc )
{
    str_or_var trait_flag_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [trait_flag_to_check, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        if( json_character_flag( trait_flag_to_check.evaluate( d ) ) == json_flag_MUTATION_THRESHOLD ) {
            return actor->crossed_threshold();
        }
        return actor->has_flag( json_character_flag( trait_flag_to_check.evaluate( d ) ) );
    };
}

conditional_t::func f_has_species( const JsonObject &jo, std::string_view member,
                                   bool is_npc )
{
    str_or_var species_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [species_to_check, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        return actor->has_species( species_id( species_to_check.evaluate( d ) ) );
    };
}

conditional_t::func f_bodytype( const JsonObject &jo, std::string_view member,
                                bool is_npc )
{
    str_or_var bt_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [bt_to_check, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        return actor->bodytype( bodytype_id( bt_to_check.evaluate( d ) ) );
    };
}

conditional_t::func f_has_activity( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_activity();
    };
}

conditional_t::func f_has_activity( const JsonObject &, std::string_view, bool is_npc )
{
    return f_has_activity( is_npc );
}

conditional_t::func f_has_proficiency( const JsonObject &jo, std::string_view member,
                                       bool is_npc )
{
    str_or_var proficiency_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [proficiency_to_check, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->knows_proficiency( proficiency_id( proficiency_to_check.evaluate(
                    d ) ) );
    };
}

conditional_t::func f_is_riding( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->is_mounted();
    };
}

conditional_t::func f_is_riding( const JsonObject &, std::string_view, bool is_npc )
{
    return f_is_riding( is_npc );
}

conditional_t::func f_npc_has_class( const JsonObject &jo, std::string_view member,
                                     bool is_npc )
{
    str_or_var class_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [class_to_check, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->is_myclass( npc_class_id( class_to_check.evaluate( d ) ) );
    };
}

conditional_t::func f_u_has_mission( const JsonObject &jo, std::string_view member )
{
    str_or_var u_mission = get_str_or_var( jo.get_member( member ), member, true );
    return [u_mission]( const_dialogue const & d ) {
        for( mission *miss_it : get_avatar().get_active_missions() ) {
            if( miss_it->mission_id() == mission_type_id( u_mission.evaluate( d ) ) ) {
                return true;
            }
        }
        return false;
    };
}

conditional_t::func f_u_monsters_in_direction( const JsonObject &jo,
        std::string_view member )
{
    str_or_var dir = get_str_or_var( jo.get_member( member ), member, true );
    return [dir]( const_dialogue const & d ) {
        //This string_to_enum function is defined in widget.h. Should it be moved?
        const int card_dir = static_cast<int>( io::string_to_enum<cardinal_direction>( dir.evaluate(
                d ) ) );
        int monster_count = get_avatar().get_mon_visible().unique_mons[card_dir].size();
        return monster_count > 0;
    };
}

conditional_t::func f_u_safe_mode_trigger( const JsonObject &jo, std::string_view member )
{
    str_or_var dir = get_str_or_var( jo.get_member( member ), member, true );
    return [dir]( const_dialogue const & d ) {
        //This string_to_enum function is defined in widget.h. Should it be moved?
        const int card_dir = static_cast<int>( io::string_to_enum<cardinal_direction>( dir.evaluate(
                d ) ) );
        return get_avatar().get_mon_visible().dangerous[card_dir];
    };
}

conditional_t::func f_u_profession( const JsonObject &jo, std::string_view member )
{
    str_or_var u_profession = get_str_or_var( jo.get_member( member ), member, true );
    return [u_profession]( const_dialogue const & d ) {
        const profession *prof = get_player_character().get_profession();
        std::set<const profession *> hobbies = get_player_character().get_hobbies();
        if( prof->get_profession_id() == profession_id( u_profession.evaluate( d ) ) ) {
            return true;
        } else if( profession_id( u_profession.evaluate( d ) )->is_hobby() ) {
            for( const profession *hob : hobbies ) {
                if( hob->get_profession_id() == profession_id( u_profession.evaluate( d ) ) ) {
                    return true;
                }
                break;
            }
            return false;
        } else {
            return false;
        }
    };
}

conditional_t::func f_has_strength( const JsonObject &jo, std::string_view member,
                                    bool is_npc )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->str_cur() >= dov.evaluate( d );
    };
}

conditional_t::func f_has_dexterity( const JsonObject &jo, std::string_view member,
                                     bool is_npc )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->dex_cur() >= dov.evaluate( d );
    };
}

conditional_t::func f_has_intelligence( const JsonObject &jo, std::string_view member,
                                        bool is_npc )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->int_cur() >= dov.evaluate( d );
    };
}

conditional_t::func f_has_perception( const JsonObject &jo, std::string_view member,
                                      bool is_npc )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->per_cur() >= dov.evaluate( d );
    };
}

conditional_t::func f_has_part_temp( const JsonObject &jo, std::string_view member,
                                     bool is_npc )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    std::optional<bodypart_id> bp;
    optional( jo, false, "bodypart", bp );
    return [dov, bp, is_npc]( const_dialogue const & d ) {
        bodypart_id bid = bp.value_or( get_bp_from_str( d.reason ) );
        return units::to_legacy_bodypart_temp( d.const_actor( is_npc )->get_cur_part_temp(
                bid ) ) >= dov.evaluate( d );
    };
}

conditional_t::func f_is_wearing( const JsonObject &jo, std::string_view member,
                                  bool is_npc )
{
    str_or_var item_id = get_str_or_var( jo.get_member( member ), member, true );
    return [item_id, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->is_wearing( itype_id( item_id.evaluate( d ) ) );
    };
}

conditional_t::func f_has_item( const JsonObject &jo, std::string_view member, bool is_npc )
{
    str_or_var item_id = get_str_or_var( jo.get_member( member ), member, true );
    return [item_id, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        return actor->charges_of( itype_id( item_id.evaluate( d ) ) ) > 0 ||
               actor->has_amount( itype_id( item_id.evaluate( d ) ), 1 );
    };
}

conditional_t::func f_has_items( const JsonObject &jo, const std::string_view member,
                                 bool is_npc )
{
    JsonObject has_items = jo.get_object( member );
    if( !has_items.has_member( "item" ) || ( !has_items.has_member( "count" ) &&
            !has_items.has_member( "charges" ) ) ) {
        return []( const_dialogue const & ) {
            return false;
        };
    } else {
        str_or_var item_id = get_str_or_var( has_items.get_member( "item" ), "item", true );
        dbl_or_var count = get_dbl_or_var( has_items, "count", false );
        dbl_or_var charges = get_dbl_or_var( has_items, "charges", false );
        return [item_id, count, charges, is_npc]( const_dialogue const & d ) {
            const_talker const *actor = d.const_actor( is_npc );
            itype_id id = itype_id( item_id.evaluate( d ) );
            if( charges.evaluate( d ) == 0 && item::count_by_charges( id ) ) {
                return actor->has_charges( id, count.evaluate( d ), true );
            }
            if( charges.evaluate( d ) > 0 && count.evaluate( d ) == 0 ) {
                return actor->has_charges( id, charges.evaluate( d ), true );
            }
            bool has_enough_charges = true;
            if( charges.evaluate( d ) > 0 ) {
                has_enough_charges = actor->has_charges( id, charges.evaluate( d ), true );
            }
            return has_enough_charges && actor->has_amount( id, count.evaluate( d ) );
        };
    }
}

conditional_t::func f_has_items_sum( const JsonObject &jo, const std::string_view member,
                                     bool is_npc )
{
    std::vector<std::pair<str_or_var, dbl_or_var>> item_and_amount;

    for( const JsonObject jsobj : jo.get_array( member ) ) {
        const str_or_var item = get_str_or_var( jsobj.get_member( "item" ), "item", true );
        const dbl_or_var amount = get_dbl_or_var( jsobj, "amount", true, 1 );
        item_and_amount.emplace_back( item, amount );
    }
    return [item_and_amount, is_npc]( const_dialogue const & d ) {
        add_msg_debug( debugmode::DF_TALKER, "using _has_items_sum:" );

        itype_id item_to_find;
        double percent = 0.0f;
        double count_desired;
        double count_present;
        double charges_present;
        double total_present;
        for( const auto &pair : item_and_amount ) {
            item_to_find = itype_id( pair.first.evaluate( d ) );
            count_desired = pair.second.evaluate( d );
            count_present = d.const_actor( is_npc )->get_amount( item_to_find );
            charges_present = d.const_actor( is_npc )->charges_of( item_to_find );
            total_present = std::max( count_present, charges_present );
            percent += total_present / count_desired;

            add_msg_debug( debugmode::DF_TALKER,
                           "item: %s, count_desired: %f, count_present: %f, charges_present: %f, total_present: %f, percent: %f",
                           item_to_find.str(), count_desired, count_present, charges_present, total_present, percent );

            if( percent >= 1 ) {
                return true;
            }
        }
        return false;
    };
}

conditional_t::func f_has_item_with_flag( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var flag = get_str_or_var( jo.get_member( member ), member, true );
    return [flag, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_item_with_flag( flag_id( flag.evaluate( d ) ) );
    };
}

conditional_t::func f_has_item_category( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var category_id = get_str_or_var( jo.get_member( member ), member, true );
    size_t count = 1;
    if( jo.has_int( "count" ) ) {
        int tcount = jo.get_int( "count" );
        if( tcount > 1 && tcount < INT_MAX ) {
            count = static_cast<size_t>( tcount );
        }
    }

    return [category_id, count, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        const item_category_id cat_id = item_category_id( category_id.evaluate( d ) );
        const auto items_with = actor->const_items_with( [cat_id]( const item & it ) {
            return it.get_category_shallow().get_id() == cat_id;
        } );
        return items_with.size() >= count;
    };
}

conditional_t::func f_has_bionics( const JsonObject &jo, std::string_view member,
                                   bool is_npc )
{
    str_or_var bionics_id = get_str_or_var( jo.get_member( member ), member, true );
    return [bionics_id, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        if( bionics_id.evaluate( d ) == "ANY" ) {
            return actor->num_bionics() > 0 || actor->has_max_power();
        }
        return actor->has_bionic( bionic_id( bionics_id.evaluate( d ) ) );
    };
}

conditional_t::func f_has_any_effect( const JsonObject &jo, std::string_view member,
                                      bool is_npc )
{
    std::vector<str_or_var> effects_to_check;
    for( JsonValue jv : jo.get_array( member ) ) {
        effects_to_check.emplace_back( get_str_or_var( jv, member ) );
    }
    dbl_or_var intensity = get_dbl_or_var( jo, "intensity", false, -1 );
    str_or_var bp;
    if( jo.has_member( "bodypart" ) ) {
        bp = get_str_or_var( jo.get_member( "bodypart" ), "bodypart", true );
    } else {
        bp.str_val = "";
    }
    return [effects_to_check, intensity, bp, is_npc]( const_dialogue const & d ) {
        bodypart_id bid = bp.evaluate( d ).empty() ? get_bp_from_str( d.reason ) :
                          bodypart_id( bp.evaluate( d ) );
        for( const str_or_var &effect_id : effects_to_check ) {
            effect target = d.const_actor( is_npc )->get_effect( efftype_id( effect_id.evaluate( d ) ), bid );
            if( !target.is_null() && intensity.evaluate( d ) <= target.get_intensity() ) {
                return true;
            }
        }
        return false;
    };
}

conditional_t::func f_has_effect( const JsonObject &jo, std::string_view member,
                                  bool is_npc )
{
    str_or_var effect_id = get_str_or_var( jo.get_member( member ), member, true );
    dbl_or_var intensity = get_dbl_or_var( jo, "intensity", false, -1 );
    str_or_var bp;
    if( jo.has_member( "bodypart" ) ) {
        bp = get_str_or_var( jo.get_member( "bodypart" ), "bodypart", true );
    } else {
        bp.str_val = "";
    }
    return [effect_id, intensity, bp, is_npc]( const_dialogue const & d ) {
        bodypart_id bid = bp.evaluate( d ).empty() ? get_bp_from_str( d.reason ) :
                          bodypart_id( bp.evaluate( d ) );
        effect target = d.const_actor( is_npc )->get_effect( efftype_id( effect_id.evaluate( d ) ), bid );
        return !target.is_null() && intensity.evaluate( d ) <= target.get_intensity();
    };
}

conditional_t::func f_need( const JsonObject &jo, std::string_view member, bool is_npc )
{
    str_or_var need = get_str_or_var( jo.get_member( member ), member, true );
    dbl_or_var dov;
    if( jo.has_int( "amount" ) ) {
        dov.min.dbl_val = jo.get_int( "amount" );
    } else if( jo.has_object( "amount" ) ) {
        dov = get_dbl_or_var( jo, "amount" );
    } else if( jo.has_string( "level" ) ) {
        const std::string &level = jo.get_string( "level" );
        auto flevel = fatigue_level_strs.find( level );
        if( flevel != fatigue_level_strs.end() ) {
            dov.min.dbl_val = static_cast<int>( flevel->second );
        }
    }
    return [need, dov, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        int amount = dov.evaluate( d );
        return ( actor->get_fatigue() > amount && need.evaluate( d ) == "fatigue" ) ||
               ( actor->get_hunger() > amount && need.evaluate( d ) == "hunger" ) ||
               ( actor->get_thirst() > amount && need.evaluate( d ) == "thirst" );
    };
}

conditional_t::func f_at_om_location( const JsonObject &jo, std::string_view member,
                                      bool is_npc )
{
    str_or_var location = get_str_or_var( jo.get_member( member ), member, true );
    return [location, is_npc]( const_dialogue const & d ) {
        const tripoint_abs_omt omt_pos = d.const_actor( is_npc )->global_omt_location();
        const oter_id &omt_ter = overmap_buffer.ter( omt_pos );
        const std::string &omt_str = omt_ter.id().str();
        std::string location_value = location.evaluate( d );

        if( location_value == "FACTION_CAMP_ANY" ) {
            std::optional<basecamp *> bcp = overmap_buffer.find_camp( omt_pos.xy() );
            if( bcp ) {
                return true;
            }
            // TODO: legacy check to be removed once primitive field camp OMTs have been purged
            return omt_str.find( "faction_base_camp" ) != std::string::npos;
        } else if( location_value == "FACTION_CAMP_START" ) {
            const std::optional<mapgen_arguments> *maybe_args = overmap_buffer.mapgen_args( omt_pos );
            return !recipe_group::get_recipes_by_id( "all_faction_base_types", omt_ter, maybe_args ).empty();
        } else {
            return oter_no_dir_or_connections( omt_ter ) == location_value;
        }
    };
}

conditional_t::func f_near_om_location( const JsonObject &jo, std::string_view member,
                                        bool is_npc )
{
    str_or_var location = get_str_or_var( jo.get_member( member ), member, true );
    const dbl_or_var range = get_dbl_or_var( jo, "range", false, 1 );
    return [location, range, is_npc]( const_dialogue const & d ) {
        const tripoint_abs_omt omt_pos = d.const_actor( is_npc )->global_omt_location();
        for( const tripoint_abs_omt &curr_pos : points_in_radius( omt_pos,
                range.evaluate( d ) ) ) {
            const oter_id &omt_ter = overmap_buffer.ter( curr_pos );
            const std::optional<mapgen_arguments> *maybe_args = overmap_buffer.mapgen_args( omt_pos );
            const std::string &omt_str = omt_ter.id().str();
            std::string location_value = location.evaluate( d );

            if( location_value == "FACTION_CAMP_ANY" ) {
                std::optional<basecamp *> bcp = overmap_buffer.find_camp( curr_pos.xy() );
                if( bcp ) {
                    return true;
                }
                // TODO: legacy check to be removed once primitive field camp OMTs have been purged
                if( omt_str.find( "faction_base_camp" ) != std::string::npos ) {
                    return true;
                }
            } else if( location_value  == "FACTION_CAMP_START" &&
                       !recipe_group::get_recipes_by_id( "all_faction_base_types", omt_ter, maybe_args ).empty() ) {
                return true;
            } else {
                if( oter_no_dir_or_connections( omt_ter ) == location_value ) {
                    return true;
                }
            }
        }
        // should never get here this is for safety
        return false;
    };
}

conditional_t::func f_has_var( const JsonObject &jo, std::string_view member, bool is_npc )
{
    dbl_or_var empty;
    const std::string var_name = get_talk_varname( jo, member, false, empty );
    const std::string &value = jo.has_member( "value" ) ? jo.get_string( "value" ) : std::string();
    if( !jo.has_member( "value" ) ) {
        jo.throw_error( R"(Missing field: "value")" );
        return []( const_dialogue const & ) {
            return false;
        };
    }

    return [var_name, value, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        return actor->get_value( var_name ) == value;
    };
}

conditional_t::func f_expects_vars( const JsonObject &jo, std::string_view member )
{
    std::vector<str_or_var> to_check;
    if( jo.has_array( member ) ) {
        for( const JsonValue &jv : jo.get_array( member ) ) {
            to_check.push_back( get_str_or_var( jv, member, true ) );
        }
    }

    return [to_check]( const_dialogue const & d ) {
        std::string missing_variables;
        for( const str_or_var &val : to_check ) {
            if( d.get_context().find( "npctalk_var_" + val.evaluate( d ) ) == d.get_context().end() ) {
                missing_variables += val.evaluate( d ) + ", ";
            }
        }
        if( !missing_variables.empty() ) {
            debugmsg( string_format( "Missing required variables: %s", missing_variables ) );
            return false;
        }
        return true;
    };
}

conditional_t::func f_npc_role_nearby( const JsonObject &jo, std::string_view member )
{
    str_or_var role = get_str_or_var( jo.get_member( member ), member, true );
    return [role]( const_dialogue const & d ) {
        const std::vector<npc *> available = g->get_npcs_if( [&]( const npc & guy ) {
            return d.const_actor( false )->posz() == guy.posz() &&
                   guy.companion_mission_role_id == role.evaluate( d ) &&
                   ( rl_dist( d.const_actor( false )->pos(), guy.pos() ) <= 48 );
        } );
        return !available.empty();
    };
}

conditional_t::func f_npc_is_travelling( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        const Character *traveller = d.const_actor( is_npc )->get_const_character();
        if( !traveller ) {
            return false;
        }
        return !traveller->omt_path.empty();
    };
}

conditional_t::func f_npc_allies( const JsonObject &jo, std::string_view member )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov]( const_dialogue const & d ) {
        return g->allies().size() >= static_cast<std::vector<npc *>::size_type>( dov.evaluate( d ) );
    };
}

conditional_t::func f_npc_allies_global( const JsonObject &jo, std::string_view member )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov]( const_dialogue const & d ) {
        const auto all_npcs = overmap_buffer.get_overmap_npcs();
        const size_t count = std::count_if( all_npcs.begin(),
        all_npcs.end(), []( const shared_ptr_fast<npc> &ptr ) {
            return ptr.get()->is_player_ally() && !ptr.get()->hallucination && !ptr.get()->is_dead();
        } );

        return count >= static_cast<size_t>( dov.evaluate( d ) );
    };
}

conditional_t::func f_u_has_cash( const JsonObject &jo, std::string_view member )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov]( const_dialogue const & d ) {
        return d.const_actor( false )->cash() >= dov.evaluate( d );
    };
}

conditional_t::func f_u_are_owed( const JsonObject &jo, std::string_view member )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov]( const_dialogue const & d ) {
        return d.const_actor( true )->debt() >= dov.evaluate( d );
    };
}

conditional_t::func f_npc_aim_rule( const JsonObject &jo, std::string_view member,
                                    bool is_npc )
{
    str_or_var setting = get_str_or_var( jo.get_member( member ), member, true );
    return [setting, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_ai_rule( "aim_rule", setting.evaluate( d ) );
    };
}

conditional_t::func f_npc_engagement_rule( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var setting = get_str_or_var( jo.get_member( member ), member, true );
    return [setting, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_ai_rule( "engagement_rule", setting.evaluate( d ) );
    };
}

conditional_t::func f_npc_cbm_reserve_rule( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var setting = get_str_or_var( jo.get_member( member ), member, true );
    return [setting, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_ai_rule( "cbm_reserve_rule", setting.evaluate( d ) );
    };
}

conditional_t::func f_npc_cbm_recharge_rule( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var setting = get_str_or_var( jo.get_member( member ), member, true );
    return [setting, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_ai_rule( "cbm_recharge_rule", setting.evaluate( d ) );
    };
}

conditional_t::func f_npc_rule( const JsonObject &jo, std::string_view member, bool is_npc )
{
    str_or_var rule = get_str_or_var( jo.get_member( member ), member, true );
    return [rule, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_ai_rule( "ally_rule", rule.evaluate( d ) );
    };
}

conditional_t::func f_npc_override( const JsonObject &jo, std::string_view member,
                                    bool is_npc )
{
    str_or_var rule = get_str_or_var( jo.get_member( member ), member, true );
    return [rule, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_ai_rule( "ally_override", rule.evaluate( d ) );
    };
}

conditional_t::func f_is_season( const JsonObject &jo, std::string_view member )
{
    str_or_var season_name = get_str_or_var( jo.get_member( member ), member, true );
    return [season_name]( const_dialogue const & d ) {
        const season_type season = season_of_year( calendar::turn );
        return ( season == SPRING && season_name.evaluate( d ) == "spring" ) ||
               ( season == SUMMER && season_name.evaluate( d ) == "summer" ) ||
               ( season == AUTUMN && season_name.evaluate( d ) == "autumn" ) ||
               ( season == WINTER && season_name.evaluate( d ) == "winter" );
    };
}

conditional_t::func f_mission_goal( const JsonObject &jo, std::string_view member,
                                    bool is_npc )
{
    str_or_var mission_goal_str = get_str_or_var( jo.get_member( member ), member, true );
    return [mission_goal_str, is_npc]( const_dialogue const & d ) {
        mission *miss = d.const_actor( is_npc )->selected_mission();
        if( !miss ) {
            return false;
        }
        const mission_goal mgoal = io::string_to_enum<mission_goal>( mission_goal_str.evaluate( d ) );
        return miss->get_type().goal == mgoal;
    };
}

conditional_t::func f_is_gender( bool is_male, bool is_npc )
{
    return [is_male, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->is_male() == is_male;
    };
}

conditional_t::func f_is_male( bool is_npc )
{
    return f_is_gender( true, is_npc );
}
conditional_t::func f_is_female( bool is_npc )
{
    return f_is_gender( false, is_npc );
}

conditional_t::func f_is_alive( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->get_is_alive();
    };
}

conditional_t::func f_exists( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        if( ( is_npc && !d.has_beta ) || ( !is_npc && !d.has_alpha ) ) {
            return false;
        } else {
            return true;
        }
    };
}
conditional_t::func f_is_avatar( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->get_const_character() &&
               d.const_actor( is_npc )->get_const_character()->is_avatar();
    };
}

conditional_t::func f_is_npc( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->get_const_npc();
    };
}

conditional_t::func f_is_character( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->get_const_character();
    };
}

conditional_t::func f_is_monster( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->get_const_monster();
    };
}

conditional_t::func f_is_item( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->get_const_item();
    };
}

conditional_t::func f_is_furniture( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->get_const_computer();
    };
}

conditional_t::func f_player_see( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        const Creature *c = d.const_actor( is_npc )->get_const_creature();
        if( c ) {
            return get_player_view().sees( *c );
        } else {
            return get_player_view().sees( d.const_actor( is_npc )->pos() );
        }
    };
}

conditional_t::func f_has_alpha()
{
    return []( const_dialogue const & d ) {
        return d.has_alpha;
    };
}

conditional_t::func f_has_beta()
{
    return []( const_dialogue const & d ) {
        return d.has_beta;
    };
}

conditional_t::func f_no_assigned_mission()
{
    return []( const_dialogue const & d ) {
        return d.missions_assigned.empty();
    };
}

conditional_t::func f_has_assigned_mission()
{
    return []( const_dialogue const & d ) {
        return d.missions_assigned.size() == 1;
    };
}

conditional_t::func f_has_many_assigned_missions()
{
    return []( const_dialogue const & d ) {
        return d.missions_assigned.size() >= 2;
    };
}

conditional_t::func f_no_available_mission( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->available_missions().empty();
    };
}

conditional_t::func f_has_available_mission( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->available_missions().size() == 1;
    };
}

conditional_t::func f_has_many_available_missions( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->available_missions().size() >= 2;
    };
}

conditional_t::func f_mission_complete( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        mission *miss = d.const_actor( is_npc )->selected_mission();
        return miss && miss->is_complete( d.const_actor( is_npc )->getID() );
    };
}

conditional_t::func f_mission_incomplete( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        mission *miss = d.const_actor( is_npc )->selected_mission();
        return miss && !miss->is_complete( d.const_actor( is_npc )->getID() );
    };
}

conditional_t::func f_mission_failed( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        mission *miss = d.const_actor( is_npc )->selected_mission();
        return miss && miss->has_failed();
    };
}

conditional_t::func f_npc_service( const JsonObject &jo, std::string_view member, bool is_npc )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [is_npc, dov]( const_dialogue const & d ) {
        return !d.const_actor( is_npc )->has_effect( effect_currently_busy, bodypart_str_id::NULL_ID() ) &&
               d.const_actor( false )->cash() >= dov.evaluate( d );
    };
}

conditional_t::func f_npc_available( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return !d.const_actor( is_npc )->has_effect( effect_currently_busy, bodypart_str_id::NULL_ID() );
    };
}

conditional_t::func f_npc_following( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->is_following();
    };
}

conditional_t::func f_npc_friend( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->is_friendly( get_player_character() );
    };
}

conditional_t::func f_npc_hostile( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->is_enemy();
    };
}

conditional_t::func f_npc_train_skills( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return !d.const_actor( is_npc )->skills_offered_to( *d.const_actor( !is_npc ) ).empty();
    };
}

conditional_t::func f_npc_train_styles( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return !d.const_actor( is_npc )->styles_offered_to( *d.const_actor( !is_npc ) ).empty();
    };
}

conditional_t::func f_npc_train_spells( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return !d.const_actor( is_npc )->spells_offered_to( *d.const_actor( !is_npc ) ).empty();
    };
}

conditional_t::func f_follower_present( const JsonObject &jo, std::string_view member )
{
    const std::string &var_name = jo.get_string( std::string( member ) );
    return [var_name]( const_dialogue const & d ) {
        npc *npc_to_check = nullptr;
        for( npc &guy : g->all_npcs() ) {
            if( guy.myclass.str() == var_name ) {
                npc_to_check = &guy;
                break;
            }
        }
        npc const *d_npc = d.const_actor( true )->get_const_npc();
        if( npc_to_check == nullptr || d_npc == nullptr ) {
            return false;
        }
        const std::set<character_id> followers = g->get_follower_list();
        if( !std::any_of( followers.begin(), followers.end(), [&npc_to_check]( const character_id & id ) {
        return id == npc_to_check->getID();
        } ) ||
        !npc_to_check->is_following() ) {
            return false;
        }
        return rl_dist( npc_to_check->pos_bub(), d_npc->pos_bub() ) < 5 &&
               get_map().clear_path( npc_to_check->pos_bub(), d_npc->pos_bub(), 5, 0, 100 );
    };
}

conditional_t::func f_at_safe_space( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return overmap_buffer.is_safe( d.const_actor( is_npc )->global_omt_location() ) &&
               d.const_actor( is_npc )->is_safe();
    };
}

conditional_t::func f_can_stow_weapon( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        return !actor->unarmed_attack() && actor->can_stash_weapon();
    };
}

conditional_t::func f_can_drop_weapon( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        return !actor->unarmed_attack() && !actor->wielded_with_flag( flag_NO_UNWIELD );
    };
}

conditional_t::func f_has_weapon( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return !d.const_actor( is_npc )->unarmed_attack();
    };
}

conditional_t::func f_is_controlling_vehicle( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        if( const optional_vpart_position &vp = get_map().veh_at( actor->pos() ) ) {
            return actor->is_in_control_of( vp->vehicle() );
        }
        return false;
    };
}

conditional_t::func f_is_driving( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        if( const optional_vpart_position &vp = get_map().veh_at( actor->pos() ) ) {
            return vp->vehicle().is_moving() && actor->is_in_control_of( vp->vehicle() );
        }
        return false;
    };
}

conditional_t::func f_has_stolen_item( bool /*is_npc*/ )
{
    return []( const_dialogue const & d ) {
        return d.const_actor( false )->has_stolen_item( *d.const_actor( true ) );
    };
}

conditional_t::func f_is_day()
{
    return []( const_dialogue const & ) {
        return !is_night( calendar::turn );
    };
}

conditional_t::func f_is_outside( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return is_creature_outside( *d.const_actor( is_npc )->get_const_creature() );
    };
}

conditional_t::func f_is_underwater( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return get_map().is_divable( d.const_actor( is_npc )->pos() );
    };
}

conditional_t::func f_is_on_land( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return get_map().has_floor( d.const_actor( is_npc )->pos() );
    };
}

conditional_t::func f_is_on_liquid( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return !get_map().is_dry( d.const_actor( is_npc )->pos() );
    };
}

conditional_t::func f_one_in_chance( const JsonObject &jo, std::string_view member )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov]( const_dialogue const & d ) {
        return one_in( dov.evaluate( d ) );
    };
}

conditional_t::func f_query( const JsonObject &jo, std::string_view member, bool is_npc )
{
    translation_or_var message = get_translation_or_var( jo.get_member( member ), member, true );
    bool default_val = jo.get_bool( "default" );
    return [message, default_val, is_npc]( const_dialogue const & d ) {
        const_talker const *actor = d.const_actor( is_npc );
        if( actor->get_const_character() && actor->get_const_character()->is_avatar() ) {
            std::string translated_message = message.evaluate( d );
            return query_yn( translated_message );
        } else {
            return default_val;
        }
    };
}

conditional_t::func f_query_tile( const JsonObject &jo, std::string_view member, bool is_npc )
{
    std::string type = jo.get_string( member.data() );
    var_info target_var = read_var_info( jo.get_object( "target_var" ) );
    if( target_var.type != var_type::global ) {
        jo.throw_error_at( "target_var", "Only global variables can be used as targets for u_query" ) ;
    }
    std::string message;
    if( jo.has_member( "message" ) ) {
        message = jo.get_string( "message" );
    }
    dbl_or_var range;
    if( jo.has_member( "range" ) ) {
        range = get_dbl_or_var( jo, "range" );
    }
    bool z_level = jo.get_bool( "z_level", false );
    return [type, target_var, message, range, z_level, is_npc]( const_dialogue const & d ) {
        std::optional<tripoint> loc;
        Character const *ch = d.const_actor( is_npc )->get_const_character();
        if( ch && ch->as_avatar() ) {
            if( type == "anywhere" ) {
                if( !message.empty() ) {
                    static_popup popup;
                    popup.on_top( true );
                    popup.message( "%s", message );
                }
                tripoint center = d.const_actor( is_npc )->pos();
                const look_around_params looka_params = { true, center, center, false, true, true, z_level };
                loc = g->look_around( looka_params ).position;
            } else if( type == "line_of_sight" ) {
                if( !message.empty() ) {
                    static_popup popup;
                    popup.on_top( true );
                    popup.message( "%s", message );
                }
                avatar dummy;
                dummy.set_location( get_avatar().get_location() );
                target_handler::trajectory traj = target_handler::mode_select_only( dummy, range.evaluate( d ) );
                if( !traj.empty() ) {
                    loc = traj.back().raw();
                }
            } else if( type == "around" ) {
                if( !message.empty() ) {
                    loc = choose_adjacent( message );
                } else {
                    loc = choose_adjacent( _( "Choose direction" ) );
                }
            } else {
                debugmsg( string_format( "Invalid selection type: %s", type ) );
            }

        }
        if( loc.has_value() ) {
            tripoint_abs_ms pos_global = get_map().getglobal( *loc );
            write_var_value( target_var.type, target_var.name, d,
                             pos_global.to_string() );
        }
        return loc.has_value();
    };
}

conditional_t::func f_x_in_y_chance( const JsonObject &jo, const std::string_view member )
{
    const JsonObject &var_obj = jo.get_object( member );
    dbl_or_var dovx = get_dbl_or_var( var_obj, "x" );
    dbl_or_var dovy = get_dbl_or_var( var_obj, "y" );
    return [dovx, dovy]( const_dialogue const & d ) {
        return x_in_y( dovx.evaluate( d ),
                       dovy.evaluate( d ) );
    };
}

conditional_t::func f_is_weather( const JsonObject &jo, std::string_view member )
{
    str_or_var weather = get_str_or_var( jo.get_member( member ), member, true );
    return [weather]( const_dialogue const & d ) {
        return get_weather().weather_id == weather_type_id( weather.evaluate( d ) );
    };
}

conditional_t::func f_map_ter_furn_with_flag( const JsonObject &jo, std::string_view member )
{
    str_or_var furn_type = get_str_or_var( jo.get_member( member ), member, true );
    var_info loc_var = read_var_info( jo.get_object( "loc" ) );
    bool terrain = true;
    if( member == "map_terrain_with_flag" ) {
        terrain = true;
    } else if( member == "map_furniture_with_flag" ) {
        terrain = false;
    }
    return [terrain, furn_type, loc_var]( const_dialogue const & d ) {
        tripoint_bub_ms loc = get_map().bub_from_abs( get_tripoint_from_var( loc_var, d, false ) );
        if( terrain ) {
            return get_map().ter( loc )->has_flag( furn_type.evaluate( d ) );
        } else {
            return get_map().furn( loc )->has_flag( furn_type.evaluate( d ) );
        }
    };
}

conditional_t::func f_map_ter_furn_id( const JsonObject &jo, std::string_view member )
{
    str_or_var furn_type = get_str_or_var( jo.get_member( member ), member, true );
    var_info loc_var = read_var_info( jo.get_object( "loc" ) );

    return [member, furn_type, loc_var]( const_dialogue const & d ) {
        tripoint_bub_ms loc = get_map().bub_from_abs( get_tripoint_from_var( loc_var, d, false ) );
        if( member == "map_terrain_id" ) {
            return get_map().ter( loc ) == ter_id( furn_type.evaluate( d ) );
        } else if( member == "map_furniture_id" ) {
            return get_map().furn( loc ) == furn_id( furn_type.evaluate( d ) );
        } else if( member == "map_field_id" ) {
            const field &fields_here = get_map().field_at( loc );
            return !!fields_here.find_field( field_type_id( furn_type.evaluate( d ) ) );
        } else {
            debugmsg( "Invalid map id: %s", member );
            return false;
        }
    };
}

conditional_t::func f_map_in_city( const JsonObject &jo, std::string_view member )
{
    str_or_var target = get_str_or_var( jo.get_member( member ), member, true );
    return [target]( const_dialogue const & d ) {
        tripoint_abs_omt target_pos = project_to<coords::omt>( tripoint_abs_ms( tripoint::from_string(
                                          target.evaluate( d ) ) ) );

        // TODO: Remove this in favour of a seperate condition for location z-level that can be used in conjunction with this map_in_city as needed
        if( target_pos.z() < -1 ) {
            return false;
        }

        return overmap_buffer.is_in_city( target_pos );
    };
}

conditional_t::func f_mod_is_loaded( const JsonObject &jo, std::string_view member )
{
    str_or_var compared_mod = get_str_or_var( jo.get_member( member ), member, true );
    return [compared_mod]( const_dialogue const & d ) {
        mod_id comp_mod = mod_id( compared_mod.evaluate( d ) );
        for( const mod_id &mod : world_generator->active_world->active_mod_order ) {
            if( comp_mod == mod ) {
                return true;
            }
        }
        return false;
    };
}

conditional_t::func f_has_faction_trust( const JsonObject &jo, std::string_view member )
{
    dbl_or_var dov = get_dbl_or_var( jo, member );
    return [dov]( const_dialogue const & d ) {
        return d.const_actor( true )->get_faction()->trusts_u >= dov.evaluate( d );
    };
}

conditional_t::func f_compare_string( const JsonObject &jo, std::string_view member )
{
    // return true if at least two strings match, OR

    std::vector<str_or_var> values;
    for( JsonValue jv : jo.get_array( member ) ) {
        values.emplace_back( get_str_or_var( jv, member ) );
    }

    return [values]( const_dialogue const & d ) {
        std::unordered_set<std::string> seen_values;
        for( const str_or_var &val : values ) {
            std::string evaluated_value = val.evaluate( d );
            if( seen_values.count( evaluated_value ) > 0 ) {
                return true;
            }
            seen_values.insert( evaluated_value );
        }
        return false;
    };
}

conditional_t::func f_compare_string_match_all( const JsonObject &jo, std::string_view member )
{
    // return true if all strings match, AND
    std::vector<str_or_var> values;
    for( JsonValue jv : jo.get_array( member ) ) {
        values.emplace_back( get_str_or_var( jv, member ) );
    }

    return [values]( const_dialogue const & d ) {
        std::string first_value = values[0].evaluate( d );
        for( size_t i = 1; i < values.size(); ++i ) {
            if( values[i].evaluate( d ) != first_value ) {
                return false;
            }
        }
        return true;
    };
}

conditional_t::func f_get_condition( const JsonObject &jo, std::string_view member )
{
    str_or_var conditionalToGet = get_str_or_var( jo.get_member( member ), member, true );
    return [conditionalToGet]( const_dialogue const & d ) {
        return d.evaluate_conditional( conditionalToGet.evaluate( d ), d );
    };
}

conditional_t::func f_test_eoc( const JsonObject &jo, std::string_view member )
{
    str_or_var eocToTest = get_str_or_var( jo.get_member( member ), member, true );
    return [eocToTest]( const_dialogue const & d ) -> bool {
        effect_on_condition_id tested( eocToTest.evaluate( d ) );
        if( !tested.is_valid() )
        {
            debugmsg( "Invalid eoc id: %s", eocToTest.evaluate( d ) );
            return false;
        }
        return tested->condition( d );
    };
}

conditional_t::func f_has_ammo()
{
    return []( const_dialogue const & d ) {
        item_location const *it = d.const_actor( true )->get_const_item();
        if( it ) {
            return ( *it )->ammo_sufficient( d.const_actor( false )->get_const_character() );
        } else {
            debugmsg( "beta talker must be Item" );
            return false;
        }
    };
}

conditional_t::func f_math( const JsonObject &jo, const std::string_view member )
{
    eoc_math math;
    math.from_json( jo, member, math_type_t::compare );
    return [math = std::move( math )]( const_dialogue const & d ) ->bool {
        return math.act( d );
    };
}

conditional_t::func f_u_has_camp()
{
    return []( const_dialogue const & ) {
        for( const tripoint_abs_omt &camp_tripoint : get_player_character().camps ) {
            std::optional<basecamp *> camp = overmap_buffer.find_camp( camp_tripoint.xy() );
            if( !camp ) {
                continue;
            }
            basecamp *bcp = *camp;
            if( bcp->get_owner() == get_player_character().get_faction()->id ) {
                return true;
            }
        }
        return false;
    };
}

conditional_t::func f_has_pickup_list( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->has_ai_rule( "pickup_rule", "any" );
    };
}

conditional_t::func f_is_by_radio()
{
    return []( const_dialogue const & d ) {
        return d.by_radio;
    };
}

conditional_t::func f_has_reason()
{
    return []( const_dialogue const & d ) {
        return !d.reason.empty();
    };
}

conditional_t::func f_roll_contested( const JsonObject &jo, const std::string_view member )
{
    dbl_or_var get_check = get_dbl_or_var( jo, member );
    dbl_or_var difficulty = get_dbl_or_var( jo, "difficulty", true );
    dbl_or_var die_size = get_dbl_or_var( jo, "die_size", false, 10 );
    return [get_check, difficulty, die_size]( const_dialogue const & d ) {
        return rng( 1, die_size.evaluate( d ) ) + get_check.evaluate( d ) >
               difficulty.evaluate( d );
    };
}

conditional_t::func f_u_know_recipe( const JsonObject &jo, std::string_view member )
{
    str_or_var known_recipe_id = get_str_or_var( jo.get_member( member ), member, true );
    return [known_recipe_id]( const_dialogue const & d ) {
        const recipe &rep = recipe_id( known_recipe_id.evaluate( d ) ).obj();
        // should be a talker function but recipes aren't in Character:: yet
        return get_player_character().knows_recipe( &rep );
    };
}

conditional_t::func f_mission_has_generic_rewards()
{
    return []( const_dialogue const & d ) {
        mission *miss = d.const_actor( true )->selected_mission();
        if( miss == nullptr ) {
            debugmsg( "mission_has_generic_rewards: mission_selected == nullptr" );
            return true;
        }
        return miss->has_generic_rewards();
    };
}

conditional_t::func f_has_worn_with_flag( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var flag = get_str_or_var( jo.get_member( member ), member, true );
    std::optional<bodypart_id> bp;
    optional( jo, false, "bodypart", bp );
    return [flag, bp, is_npc]( const_dialogue const & d ) {
        bodypart_id bid = bp.value_or( get_bp_from_str( d.reason ) );
        return d.const_actor( is_npc )->worn_with_flag( flag_id( flag.evaluate( d ) ), bid );
    };
}

conditional_t::func f_has_wielded_with_flag( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var flag = get_str_or_var( jo.get_member( member ), member, true );
    return [flag, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->wielded_with_flag( flag_id( flag.evaluate( d ) ) );
    };
}

conditional_t::func f_has_wielded_with_weapon_category( const JsonObject &jo,
        std::string_view member,
        bool is_npc )
{
    str_or_var w_cat = get_str_or_var( jo.get_member( member ), member, true );
    return [w_cat, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->wielded_with_weapon_category( weapon_category_id( w_cat.evaluate(
                    d ) ) );
    };
}

conditional_t::func f_has_wielded_with_skill( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    // ideally all this "u wield with X" should be moved to some mutator
    // and a single effect should check mutator applied to the item in your hands
    str_or_var w_skill = get_str_or_var( jo.get_member( member ), member, true );
    return [w_skill, is_npc]( const_dialogue const & d ) {

        return d.const_actor( is_npc )->wielded_with_weapon_skill( skill_id( w_skill.evaluate( d ) ) );
    };
}

conditional_t::func f_has_wielded_with_ammotype( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var w_ammotype = get_str_or_var( jo.get_member( member ), member, true );
    return [w_ammotype, is_npc]( const_dialogue const & d ) {

        return d.const_actor( is_npc )->wielded_with_item_ammotype( ammotype( w_ammotype.evaluate( d ) ) );
    };
}

conditional_t::func f_can_see( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->can_see();
    };
}

conditional_t::func f_is_deaf( bool is_npc )
{
    return [is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->is_deaf();
    };
}

conditional_t::func f_is_on_terrain( const JsonObject &jo, std::string_view member,
                                     bool is_npc )
{
    str_or_var terrain_type = get_str_or_var( jo.get_member( member ), member, true );
    return [terrain_type, is_npc]( const_dialogue const & d ) {
        map &here = get_map();
        return here.ter( d.const_actor( is_npc )->pos() ) == ter_id( terrain_type.evaluate( d ) );
    };
}

conditional_t::func f_is_on_terrain_with_flag( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var terrain_type = get_str_or_var( jo.get_member( member ), member, true );
    return [terrain_type, is_npc]( const_dialogue const & d ) {
        map &here = get_map();
        return here.ter( d.const_actor( is_npc )->pos() )->has_flag( terrain_type.evaluate( d ) );
    };
}

conditional_t::func f_is_in_field( const JsonObject &jo, std::string_view member,
                                   bool is_npc )
{
    str_or_var field_type = get_str_or_var( jo.get_member( member ), member, true );
    return [field_type, is_npc]( const_dialogue const & d ) {
        map &here = get_map();
        field_type_id ft = field_type_id( field_type.evaluate( d ) );
        for( const std::pair<const field_type_id, field_entry> &f : here.field_at( d.const_actor(
                    is_npc )->pos() ) ) {
            if( f.second.get_field_type() == ft ) {
                return true;
            }
        }
        return false;
    };
}

conditional_t::func f_has_move_mode( const JsonObject &jo, std::string_view member,
                                     bool is_npc )
{
    str_or_var mode = get_str_or_var( jo.get_member( member ), member, true );
    return [mode, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->get_move_mode() == move_mode_id( mode.evaluate( d ) );
    };
}

conditional_t::func f_can_see_location( const JsonObject &jo, std::string_view member,
                                        bool is_npc )
{
    str_or_var target = get_str_or_var( jo.get_member( member ), member, true );
    return [is_npc, target]( const_dialogue const & d ) {
        tripoint_abs_ms target_pos = tripoint_abs_ms( tripoint::from_string( target.evaluate( d ) ) );
        return d.const_actor( is_npc )->can_see_location( get_map().bub_from_abs( target_pos ).raw() );
    };
}

conditional_t::func f_using_martial_art( const JsonObject &jo, std::string_view member,
        bool is_npc )
{
    str_or_var style_to_check = get_str_or_var( jo.get_member( member ), member, true );
    return [style_to_check, is_npc]( const_dialogue const & d ) {
        return d.const_actor( is_npc )->using_martial_art( matype_id( style_to_check.evaluate( d ) ) );
    };
}

} // namespace
} // namespace conditional_fun

template<class T>
static std::function<T( const_dialogue const & )> get_get_str_( const JsonObject &jo,
        std::function<T( const std::string & )> ret_func )
{
    const std::string &mutator = jo.get_string( "mutator" );
    if( jo.get_string( "mutator" ) == "mon_faction" ) {
        str_or_var mtypeid = get_str_or_var( jo.get_member( "mtype_id" ), "mtype_id" );
        return [mtypeid, ret_func]( const_dialogue const & d ) {
            return ret_func( ( static_cast<mtype_id>( mtypeid.evaluate( d ) ) )->default_faction.str() );
        };
    } else if( jo.get_string( "mutator" ) == "game_option" ) {
        str_or_var option = get_str_or_var( jo.get_member( "option" ), "option" );
        return [option, ret_func]( const_dialogue const & d ) {
            return ret_func( get_option<std::string>( option.evaluate( d ) ) );
        };
    } else if( jo.get_string( "mutator" ) == "valid_technique" ) {
        std::vector<str_or_var> blacklist;
        if( jo.has_array( "blacklist" ) ) {
            for( const JsonValue &jv : jo.get_array( "blacklist" ) ) {
                blacklist.push_back( get_str_or_var( jv, "blacklist" ) );
            }
        }

        bool crit = jo.get_bool( "crit", false );
        bool dodge_counter = jo.get_bool( "dodge_counter", false );
        bool block_counter = jo.get_bool( "block_counter", false );

        return [blacklist, crit, dodge_counter, block_counter, ret_func]( const_dialogue const & d ) {
            std::vector<matec_id> bl;
            bl.reserve( blacklist.size() );
            for( const str_or_var &sv : blacklist ) {
                bl.emplace_back( sv.evaluate( d ) );
            }
            return ret_func( d.const_actor( false )->get_random_technique( *d.const_actor(
                                 true )->get_const_creature(),
                             crit, dodge_counter, block_counter, bl ).str() );
        };
    } else if( jo.get_string( "mutator" ) == "loc_relative_u" ) {
        str_or_var target = get_str_or_var( jo.get_member( "target" ), "target" );
        bool use_beta_talker = mutator == "npc_loc_relative";
        return [target, use_beta_talker, ret_func]( const_dialogue const & d ) {
            tripoint_abs_ms char_pos = get_map().getglobal( d.const_actor( use_beta_talker )->pos() );
            tripoint_abs_ms target_pos = char_pos + tripoint::from_string( target.evaluate( d ) );
            return ret_func( target_pos.to_string() );
        };
    } else if( mutator == "topic_item" ) {
        return [ret_func]( const_dialogue const & d ) {
            return ret_func( d.cur_item.str() );
        };
    }

    return nullptr;
}

template<class T>
static std::function<T( const_dialogue const & )> get_get_translation_( const JsonObject &jo,
        std::function<T( const translation & )> ret_func )
{
    if( jo.get_string( "mutator" ) == "ma_technique_description" ) {
        str_or_var ma = get_str_or_var( jo.get_member( "matec_id" ), "matec_id" );

        return [ma, ret_func]( const_dialogue const & d ) {
            return ret_func( matec_id( ma.evaluate( d ) )->description );
        };
    } else if( jo.get_string( "mutator" ) == "ma_technique_name" ) {
        str_or_var ma = get_str_or_var( jo.get_member( "matec_id" ), "matec_id" );

        return [ma, ret_func]( const_dialogue const & d ) {
            return ret_func( matec_id( ma.evaluate( d ) )->name );
        };
    }

    return nullptr;
}

std::function<translation( const_dialogue const & )> conditional_t::get_get_translation(
    const JsonObject &jo )
{
    auto ret_func = get_get_str_<translation>( jo, []( const std::string & s ) {
        return no_translation( s );
    } );

    if( !ret_func ) {
        ret_func = get_get_translation_<translation>( jo, []( const translation & t ) {
            return t;
        } );
        if( !ret_func ) {
            jo.throw_error( "unrecognized string mutator in " + jo.str() );
            return []( const_dialogue const & ) {
                return translation();
            };
        }
    }

    return ret_func;
}

std::function<std::string( const_dialogue const & )> conditional_t::get_get_string(
    const JsonObject &jo )
{
    auto ret_func = get_get_str_<std::string>( jo, []( const std::string & s ) {
        return s;
    } );

    if( !ret_func ) {
        ret_func = get_get_translation_<std::string>( jo, []( const translation & t ) {
            return t.translated();
        } );
        if( !ret_func ) {
            jo.throw_error( "unrecognized string mutator in " + jo.str() );
            return []( const_dialogue const & ) {
                return "INVALID";
            };
        }
    }

    return ret_func;
}

namespace
{
std::unordered_map<std::string_view, int ( const_talker::* )() const> const f_get_vals = {
    { "activity_level", &const_talker::get_activity_level },
    { "age", &const_talker::get_age },
    { "anger", &const_talker::get_anger },
    { "bmi_permil", &const_talker::get_bmi_permil },
    { "cash", &const_talker::cash },
    { "difficulty", &const_talker::get_difficulty },
    { "dexterity_base", &const_talker::get_dex_max },
    { "dexterity_bonus", &const_talker::get_dex_bonus },
    { "dexterity", &const_talker::dex_cur },
    { "exp", &const_talker::get_kill_xp },
    { "fatigue", &const_talker::get_fatigue },
    { "fine_detail_vision_mod", &const_talker::get_fine_detail_vision_mod },
    { "focus", &const_talker::focus_cur },
    { "friendly", &const_talker::get_friendly },
    { "grab_strength", &const_talker::get_grab_strength },
    { "height", &const_talker::get_height },
    { "hunger", &const_talker::get_hunger },
    { "instant_thirst", &const_talker::get_instant_thirst },
    { "intelligence_base", &const_talker::get_int_max },
    { "intelligence_bonus", &const_talker::get_int_bonus },
    { "intelligence", &const_talker::int_cur },
    { "mana_max", &const_talker::mana_max },
    { "mana", &const_talker::mana_cur },
    { "morale", &const_talker::morale_cur },
    { "owed", &const_talker::debt },
    { "perception_base", &const_talker::get_per_max },
    { "perception_bonus", &const_talker::get_per_bonus },
    { "perception", &const_talker::per_cur },
    { "pkill", &const_talker::get_pkill },
    { "pos_x", &const_talker::posx },
    { "pos_y", &const_talker::posy },
    { "pos_z", &const_talker::posz },
    { "rad", &const_talker::get_rad },
    { "size", &const_talker::get_size },
    { "sleep_deprivation", &const_talker::get_sleep_deprivation },
    { "sold", &const_talker::sold },
    { "stamina", &const_talker::get_stamina },
    { "stim", &const_talker::get_stim },
    { "strength_base", &const_talker::get_str_max },
    { "strength_bonus", &const_talker::get_str_bonus },
    { "strength", &const_talker::str_cur },
    { "thirst", &const_talker::get_thirst },
    { "count", &const_talker::get_count }
};
} // namespace

// Consider adding new, single-purpose math functions instead of feeding this monster another else-if
std::function<double( const_dialogue const & )>
conditional_t::get_get_dbl( std::string_view checked_value, char scope )
{
    const bool is_npc = scope == 'n';

    if( auto iter = f_get_vals.find( checked_value ); iter != f_get_vals.end() ) {
        return [is_npc, func = iter->second ]( const_dialogue const & d ) {
            return ( d.const_actor( is_npc )->*func )();
        };

    } else if( checked_value == "allies" ) {
        if( is_npc ) {
            throw math::syntax_error( "Can't get allies count for NPCs" );
        }
        return []( const_dialogue const & /* d */ ) {
            return static_cast<double>( g->allies().size() );
        };
    } else if( checked_value == "dodge" ) {
        return [is_npc]( const_dialogue const & d ) {
            return d.const_actor( is_npc )->get_const_character()->get_dodge();
        };
    } else if( checked_value == "power_percentage" ) {
        return [is_npc]( const_dialogue const & d ) {
            // Energy in milijoule
            units::energy::value_type power_max = d.const_actor( is_npc )->power_max().value();
            if( power_max == 0 ) {
                return 0.0; //Default value if character does not have power, avoids division with 0.
            }
            return static_cast<double>( d.const_actor( is_npc )->power_cur().value() * 100.0L / power_max );
        };
    } else if( checked_value == "mana_percentage" ) {
        return [is_npc]( const_dialogue const & d ) {
            int mana_max = d.const_actor( is_npc )->mana_max();
            if( mana_max == 0 ) {
                return 0.0; //Default value if character does not have mana, avoids division with 0.
            }
            return d.const_actor( is_npc )->mana_cur() * 100.0 / mana_max;
        };
    } else if( checked_value == "body_temp" ) {
        return [is_npc]( const_dialogue const & d ) {
            return units::to_legacy_bodypart_temp( d.const_actor( is_npc )->get_body_temp() );
        };
    } else if( checked_value == "body_temp_delta" ) {
        return [is_npc]( const_dialogue const & d ) {
            return units::to_legacy_bodypart_temp_delta( d.const_actor( is_npc )->get_body_temp_delta() );
        };
    } else if( checked_value == "power" ) {
        return [is_npc]( const_dialogue const & d ) {
            // Energy in milijoule
            return static_cast<double>( d.const_actor( is_npc )->power_cur().value() );
        };
    } else if( checked_value == "power_max" ) {
        return [is_npc]( const_dialogue const & d ) {
            // Energy in milijoule
            return static_cast<double>( d.const_actor( is_npc )->power_max().value() );
        };
    }

    throw math::syntax_error( string_format( R"(Invalid aspect "%s" for val())", checked_value ) );
}

namespace
{
std::unordered_map<std::string_view, void ( talker::* )( int )> const f_set_vals = {
    { "age", &talker::set_age },
    { "anger", &talker::set_anger },
    { "dexterity_base", &talker::set_dex_max },
    { "dexterity_bonus", &talker::set_dex_bonus },
    { "exp", &talker::set_kill_xp },
    { "fatigue", &talker::set_fatigue },
    { "friendly", &talker::set_friendly },
    { "height", &talker::set_height },
    { "intelligence_base", &talker::set_int_max },
    { "intelligence_bonus", &talker::set_int_bonus },
    { "mana", &talker::set_mana_cur },
    { "morale", &talker::set_morale },
    { "perception_base", &talker::set_per_max },
    { "perception_bonus", &talker::set_per_bonus },
    { "pkill", &talker::set_pkill },
    { "rad", &talker::set_rad },
    { "sleep_deprivation", &talker::set_sleep_deprivation },
    { "stamina", &talker::set_stamina },
    { "stim", &talker::set_stim },
    { "strength_base", &talker::set_str_max },
    { "strength_bonus", &talker::set_str_bonus },
    { "thirst", &talker::set_thirst },
};
} // namespace

// Consider adding new, single-purpose math functions instead of feeding this monster another else-if
std::function<void( dialogue &, double )>
conditional_t::get_set_dbl( std::string_view checked_value, char scope )
{
    const bool is_npc = scope == 'n';

    if( auto iter = f_set_vals.find( checked_value ); iter != f_set_vals.end() ) {
        return [is_npc, func = iter->second ]( dialogue & d, double input ) {
            ( d.actor( is_npc )->*func )( input );
        };

    } else if( checked_value == "owed" ) {
        return [is_npc]( dialogue & d, double input ) {
            d.actor( is_npc )->add_debt( input - d.actor( is_npc )->debt() );
        };
    } else if( checked_value == "sold" ) {
        return [is_npc]( dialogue & d, double input ) {
            d.actor( is_npc )->add_sold( input - d.actor( is_npc )->sold() );
        };
    } else if( checked_value == "pos_x" ) {
        return [is_npc]( dialogue & d, double input ) {
            tripoint const tr = d.actor( is_npc )->pos();
            d.actor( is_npc )->set_pos( tripoint( input, tr.y, tr.z ) );
        };
    } else if( checked_value == "pos_y" ) {
        return [is_npc]( dialogue & d, double input ) {
            tripoint const tr = d.actor( is_npc )->pos();
            d.actor( is_npc )->set_pos( tripoint( tr.x, input, tr.z ) );
        };
    } else if( checked_value == "pos_z" ) {
        return [is_npc]( dialogue & d, double input ) {
            tripoint const tr = d.actor( is_npc )->pos();
            d.actor( is_npc )->set_pos( tripoint( tr.xy(), input ) );
        };
    } else if( checked_value == "power" ) {
        return [is_npc]( dialogue & d, double input ) {
            // Energy in milijoule
            d.actor( is_npc )->set_power_cur( 1_mJ * input );
        };
    } else if( checked_value == "power_percentage" ) {
        return [is_npc]( dialogue & d, double input ) {
            // Energy in milijoule
            d.actor( is_npc )->set_power_cur( ( d.actor( is_npc )->power_max() * input ) / 100 );
        };
    } else if( checked_value == "focus" ) {
        return [is_npc]( dialogue & d, double input ) {
            d.actor( is_npc )->mod_focus( input - d.actor( is_npc )->focus_cur() );
        };
    } else if( checked_value == "mana_percentage" ) {
        return [is_npc]( dialogue & d, double input ) {
            d.actor( is_npc )->set_mana_cur( ( d.actor( is_npc )->mana_max() * input ) / 100 );
        };
    }
    throw math::syntax_error( string_format( R"(Invalid aspect "%s" for val())", checked_value ) );
}

void deferred_math::_validate_type() const
{
    math_type_t exp_type = exp->get_type();
    if( exp_type == math_type_t::assign && type != math_type_t::assign ) {
        jo.throw_error_at( "math",
                           R"(Assignment operators can't be used in this context.  Did you mean to use "=="? )" );
    } else if( exp_type != math_type_t::assign && type == math_type_t::assign ) {
        jo.throw_error_at( "math", R"(Eval statement in assignment context has no effect)" );
    }
}

void eoc_math::from_json( const JsonObject &jo, std::string_view member, math_type_t type_ )
{
    JsonArray const objects = jo.get_array( member );
    std::string combined;
    for( size_t i = 0; i < objects.size(); i++ ) {
        combined.append( objects.get_string( i ) );
    }
    exp = defer_math( jo, combined, type_ );
}

template<typename D>
double eoc_math::act( D &d ) const
{
    try {
        return exp->eval( d );
    } catch( math::exception const &re ) {
        debugmsg( "%s\n\n%s", re.what(), d.get_callstack() );
    }

    return 0;
}

template double eoc_math::act( dialogue &d ) const;
template double eoc_math::act( const_dialogue const &d ) const;

static const
std::vector<condition_parser>
parsers = {
    {"u_has_any_trait", "npc_has_any_trait", jarg::array, &conditional_fun::f_has_any_trait },
    {"u_has_trait", "npc_has_trait", jarg::member, &conditional_fun::f_has_trait },
    { "u_is_trait_purifiable", "npc_is_trait_purifiable", jarg::member, &conditional_fun::f_is_trait_purifiable},
    {"u_has_visible_trait", "npc_has_visible_trait", jarg::member, &conditional_fun::f_has_visible_trait },
    {"u_has_martial_art", "npc_has_martial_art", jarg::member, &conditional_fun::f_has_martial_art },
    {"u_using_martial_art", "npc_using_martial_art", jarg::member, &conditional_fun::f_using_martial_art },
    {"u_has_proficiency", "npc_has_proficiency", jarg::member, &conditional_fun::f_has_proficiency },
    {"u_has_flag", "npc_has_flag", jarg::member, &conditional_fun::f_has_flag },
    {"u_has_species", "npc_has_species", jarg::member, &conditional_fun::f_has_species },
    {"u_bodytype", "npc_bodytype", jarg::member, &conditional_fun::f_bodytype },
    {"u_has_class", "npc_has_class", jarg::member, &conditional_fun::f_npc_has_class },
    {"u_has_activity", "npc_has_activity", jarg::string, &conditional_fun::f_has_activity },
    {"u_is_riding", "npc_is_riding", jarg::string, &conditional_fun::f_is_riding },
    {"u_has_mission", jarg::string, &conditional_fun::f_u_has_mission },
    {"u_monsters_in_direction", jarg::string, &conditional_fun::f_u_monsters_in_direction },
    {"u_safe_mode_trigger", jarg::member, &conditional_fun::f_u_safe_mode_trigger },
    {"u_profession", jarg::string, &conditional_fun::f_u_profession },
    {"u_has_strength", "npc_has_strength", jarg::member | jarg::array, &conditional_fun::f_has_strength },
    {"u_has_dexterity", "npc_has_dexterity", jarg::member | jarg::array, &conditional_fun::f_has_dexterity },
    {"u_has_intelligence", "npc_has_intelligence", jarg::member | jarg::array, &conditional_fun::f_has_intelligence },
    {"u_has_perception", "npc_has_perception", jarg::member | jarg::array, &conditional_fun::f_has_perception },
    {"u_has_part_temp", "npc_has_part_temp", jarg::member | jarg::array, &conditional_fun::f_has_part_temp },
    {"u_is_wearing", "npc_is_wearing", jarg::member, &conditional_fun::f_is_wearing },
    {"u_has_item", "npc_has_item", jarg::member, &conditional_fun::f_has_item },
    {"u_has_item_with_flag", "npc_has_item_with_flag", jarg::member, &conditional_fun::f_has_item_with_flag },
    {"u_has_items", "npc_has_items", jarg::member, &conditional_fun::f_has_items },
    {"u_has_items_sum", "npc_has_items_sum", jarg::array, &conditional_fun::f_has_items_sum },
    {"u_has_item_category", "npc_has_item_category", jarg::member, &conditional_fun::f_has_item_category },
    {"u_has_bionics", "npc_has_bionics", jarg::member, &conditional_fun::f_has_bionics },
    {"u_has_any_effect", "npc_has_any_effect", jarg::array, &conditional_fun::f_has_any_effect },
    {"u_has_effect", "npc_has_effect", jarg::member, &conditional_fun::f_has_effect },
    {"u_need", "npc_need", jarg::member, &conditional_fun::f_need },
    {"u_query", "npc_query", jarg::member, &conditional_fun::f_query },
    {"u_query_tile", "npc_query_tile", jarg::member, &conditional_fun::f_query_tile },
    {"u_at_om_location", "npc_at_om_location", jarg::member, &conditional_fun::f_at_om_location },
    {"u_near_om_location", "npc_near_om_location", jarg::member, &conditional_fun::f_near_om_location },
    {"u_has_var", "npc_has_var", jarg::string, &conditional_fun::f_has_var },
    { "follower_present", jarg::string, &conditional_fun::f_follower_present},
    {"expects_vars", jarg::member, &conditional_fun::f_expects_vars },
    {"npc_role_nearby", jarg::string, &conditional_fun::f_npc_role_nearby },
    {"npc_allies", jarg::member | jarg::array, &conditional_fun::f_npc_allies },
    {"npc_allies_global", jarg::member | jarg::array, &conditional_fun::f_npc_allies_global },
    {"u_service", "npc_service", jarg::member, &conditional_fun::f_npc_service },
    {"u_has_cash", jarg::member | jarg::array, &conditional_fun::f_u_has_cash },
    {"u_are_owed", jarg::member | jarg::array, &conditional_fun::f_u_are_owed },
    {"u_aim_rule", "npc_aim_rule", jarg::member, &conditional_fun::f_npc_aim_rule },
    {"u_engagement_rule", "npc_engagement_rule", jarg::member, &conditional_fun::f_npc_engagement_rule },
    {"u_cbm_reserve_rule", "npc_cbm_reserve_rule", jarg::member, &conditional_fun::f_npc_cbm_reserve_rule },
    {"u_cbm_recharge_rule", "npc_cbm_recharge_rule", jarg::member, &conditional_fun::f_npc_cbm_recharge_rule },
    {"u_rule", "npc_rule", jarg::member, &conditional_fun::f_npc_rule },
    {"u_override", "npc_override", jarg::member, &conditional_fun::f_npc_override },
    {"is_season", jarg::member, &conditional_fun::f_is_season },
    {"u_mission_goal", "mission_goal", jarg::member, &conditional_fun::f_mission_goal },
    {"u_mission_goal", "npc_mission_goal", jarg::member, &conditional_fun::f_mission_goal },
    {"roll_contested", jarg::member, &conditional_fun::f_roll_contested },
    {"u_know_recipe", jarg::member, &conditional_fun::f_u_know_recipe },
    {"one_in_chance", jarg::member | jarg::array, &conditional_fun::f_one_in_chance },
    {"x_in_y_chance", jarg::object, &conditional_fun::f_x_in_y_chance },
    {"u_has_worn_with_flag", "npc_has_worn_with_flag", jarg::member, &conditional_fun::f_has_worn_with_flag },
    {"u_has_wielded_with_flag", "npc_has_wielded_with_flag", jarg::member, &conditional_fun::f_has_wielded_with_flag },
    {"u_has_wielded_with_weapon_category", "npc_has_wielded_with_weapon_category", jarg::member, &conditional_fun::f_has_wielded_with_weapon_category },
    {"u_has_wielded_with_skill", "npc_has_wielded_with_skill", jarg::member, &conditional_fun::f_has_wielded_with_skill },
    {"u_has_wielded_with_ammotype", "npc_has_wielded_with_ammotype", jarg::member, &conditional_fun::f_has_wielded_with_ammotype },
    {"u_is_on_terrain", "npc_is_on_terrain", jarg::member, &conditional_fun::f_is_on_terrain },
    {"u_is_on_terrain_with_flag", "npc_is_on_terrain_with_flag", jarg::member, &conditional_fun::f_is_on_terrain_with_flag },
    {"u_is_in_field", "npc_is_in_field", jarg::member, &conditional_fun::f_is_in_field },
    {"u_has_move_mode", "npc_has_move_mode", jarg::member, &conditional_fun::f_has_move_mode },
    {"u_can_see_location", "npc_can_see_location", jarg::member, &conditional_fun::f_can_see_location },
    {"is_weather", jarg::member, &conditional_fun::f_is_weather },
    {"map_terrain_with_flag", jarg::member, &conditional_fun::f_map_ter_furn_with_flag },
    {"map_furniture_with_flag", jarg::member, &conditional_fun::f_map_ter_furn_with_flag },
    {"map_terrain_id", jarg::member, &conditional_fun::f_map_ter_furn_id },
    {"map_furniture_id", jarg::member, &conditional_fun::f_map_ter_furn_id },
    {"map_field_id", jarg::member, &conditional_fun::f_map_ter_furn_id },
    {"map_in_city", jarg::member, &conditional_fun::f_map_in_city },
    {"mod_is_loaded", jarg::member, &conditional_fun::f_mod_is_loaded },
    {"u_has_faction_trust", jarg::member | jarg::array, &conditional_fun::f_has_faction_trust },
    {"math", jarg::member, &conditional_fun::f_math },
    {"compare_string", jarg::member, &conditional_fun::f_compare_string },
    {"compare_string_match_all", jarg::member, &conditional_fun::f_compare_string_match_all },
    {"get_condition", jarg::member, &conditional_fun::f_get_condition },
    {"test_eoc", jarg::member, &conditional_fun::f_test_eoc },
};

// When updating this, please also update `dynamic_line_string_keys` in
// `lang/string_extractor/parsers/talk_topic.py` so the lines are properly
// extracted for translation
static const
std::vector<condition_parser>
parsers_simple = {
    {"u_male", "npc_male", &conditional_fun::f_is_male },
    {"u_female", "npc_female", &conditional_fun::f_is_female },
    {"u_is_travelling", "npc_is_travelling", &conditional_fun::f_npc_is_travelling },
    {"has_no_assigned_mission", &conditional_fun::f_no_assigned_mission },
    {"has_assigned_mission", &conditional_fun::f_has_assigned_mission },
    {"has_many_assigned_missions", &conditional_fun::f_has_many_assigned_missions },
    {"u_has_no_available_mission", "has_no_available_mission", &conditional_fun::f_no_available_mission },
    {"u_has_no_available_mission", "npc_has_no_available_mission", &conditional_fun::f_no_available_mission },
    {"u_has_available_mission", "has_available_mission", &conditional_fun::f_has_available_mission },
    {"u_has_available_mission", "npc_has_available_mission", &conditional_fun::f_has_available_mission },
    {"u_has_many_available_missions", "has_many_available_missions", &conditional_fun::f_has_many_available_missions },
    {"u_has_many_available_missions", "npc_has_many_available_missions", &conditional_fun::f_has_many_available_missions },
    {"u_mission_complete", "mission_complete", &conditional_fun::f_mission_complete },
    {"u_mission_complete", "npc_mission_complete", &conditional_fun::f_mission_complete },
    {"u_mission_incomplete", "mission_incomplete", &conditional_fun::f_mission_incomplete },
    {"u_mission_incomplete", "npc_mission_incomplete", &conditional_fun::f_mission_incomplete },
    {"u_mission_failed", "mission_failed", &conditional_fun::f_mission_failed },
    {"u_mission_failed", "npc_mission_failed", &conditional_fun::f_mission_failed },
    {"u_available", "npc_available", &conditional_fun::f_npc_available },
    {"u_following", "npc_following", &conditional_fun::f_npc_following },
    {"u_friend", "npc_friend", &conditional_fun::f_npc_friend },
    {"u_hostile", "npc_hostile", &conditional_fun::f_npc_hostile },
    {"u_train_skills", "npc_train_skills", &conditional_fun::f_npc_train_skills },
    {"u_train_styles", "npc_train_styles", &conditional_fun::f_npc_train_styles },
    {"u_train_spells", "npc_train_spells", &conditional_fun::f_npc_train_spells },
    {"u_at_safe_space", "at_safe_space", &conditional_fun::f_at_safe_space },
    {"u_at_safe_space", "npc_at_safe_space", &conditional_fun::f_at_safe_space },
    {"u_can_stow_weapon", "npc_can_stow_weapon", &conditional_fun::f_can_stow_weapon },
    {"u_can_drop_weapon", "npc_can_drop_weapon", &conditional_fun::f_can_drop_weapon },
    {"u_has_weapon", "npc_has_weapon", &conditional_fun::f_has_weapon },
    {"u_controlling_vehicle", "npc_controlling_vehicle", &conditional_fun::f_is_controlling_vehicle },
    {"u_driving", "npc_driving", &conditional_fun::f_is_driving },
    {"u_has_activity", "npc_has_activity", &conditional_fun::f_has_activity },
    {"u_is_riding", "npc_is_riding", &conditional_fun::f_is_riding },
    {"is_day", &conditional_fun::f_is_day },
    {"u_has_stolen_item", "npc_has_stolen_item", &conditional_fun::f_has_stolen_item },
    {"u_is_outside", "is_outside", &conditional_fun::f_is_outside },
    {"u_is_outside", "npc_is_outside", &conditional_fun::f_is_outside },
    {"u_is_underwater", "npc_is_underwater", &conditional_fun::f_is_underwater },
    {"u_is_on_land", "npc_is_on_land", &conditional_fun::f_is_on_land },
    {"u_is_on_liquid", "npc_is_on_liquid", &conditional_fun::f_is_on_liquid },
    {"u_has_camp", &conditional_fun::f_u_has_camp },
    {"u_has_pickup_list", "has_pickup_list", &conditional_fun::f_has_pickup_list },
    {"u_has_pickup_list", "npc_has_pickup_list", &conditional_fun::f_has_pickup_list },
    {"is_by_radio", &conditional_fun::f_is_by_radio },
    {"has_reason", &conditional_fun::f_has_reason },
    {"mission_has_generic_rewards", &conditional_fun::f_mission_has_generic_rewards },
    {"u_can_see", "npc_can_see", &conditional_fun::f_can_see },
    {"u_is_deaf", "npc_is_deaf", &conditional_fun::f_is_deaf },
    {"u_is_alive", "npc_is_alive", &conditional_fun::f_is_alive },
    {"u_exists", "npc_exists", &conditional_fun::f_exists },
    {"u_is_avatar", "npc_is_avatar", &conditional_fun::f_is_avatar },
    {"u_is_npc", "npc_is_npc", &conditional_fun::f_is_npc },
    {"u_is_character", "npc_is_character", &conditional_fun::f_is_character },
    {"u_is_monster", "npc_is_monster", &conditional_fun::f_is_monster },
    {"u_is_item", "npc_is_item", &conditional_fun::f_is_item },
    {"u_is_furniture", "npc_is_furniture", &conditional_fun::f_is_furniture },
    {"has_ammo", &conditional_fun::f_has_ammo },
    {"player_see_u", "player_see_npc", &conditional_fun::f_player_see },
    {"has_alpha", &conditional_fun::f_has_alpha },
    {"has_beta", &conditional_fun::f_has_beta },
};

conditional_t::conditional_t( const JsonObject &jo )
{
    // improve the clarity of NPC setter functions
    bool found_sub_member = false;
    const auto parse_array = []( const JsonObject & jo, const std::string_view type ) {
        std::vector<conditional_t> conditionals;
        for( const JsonValue entry : jo.get_array( type ) ) {
            if( entry.test_string() ) {
                conditional_t type_condition( entry.get_string() );
                conditionals.emplace_back( type_condition );
            } else {
                JsonObject cond = entry.get_object();
                conditional_t type_condition( cond );
                conditionals.emplace_back( type_condition );
            }
        }
        return conditionals;
    };
    if( jo.has_array( "and" ) ) {
        std::vector<conditional_t> and_conditionals = parse_array( jo, "and" );
        found_sub_member = true;
        condition = [acs = std::move( and_conditionals )]( const_dialogue const & d ) {
            return std::all_of( acs.begin(), acs.end(), [&d]( conditional_t const & cond ) {
                return cond( d );
            } );
        };
    } else if( jo.has_array( "or" ) ) {
        std::vector<conditional_t> or_conditionals = parse_array( jo, "or" );
        found_sub_member = true;
        condition = [ocs = std::move( or_conditionals )]( const_dialogue const & d ) {
            return std::any_of( ocs.begin(), ocs.end(), [&d]( conditional_t const & cond ) {
                return cond( d );
            } );
        };
    } else if( jo.has_object( "not" ) ) {
        JsonObject cond = jo.get_object( "not" );
        const conditional_t sub_condition = conditional_t( cond );
        found_sub_member = true;
        condition = [sub_condition]( const_dialogue const & d ) {
            return !sub_condition( d );
        };
    } else if( jo.has_string( "not" ) ) {
        const conditional_t sub_condition = conditional_t( jo.get_string( "not" ) );
        found_sub_member = true;
        condition = [sub_condition]( const_dialogue const & d ) {
            return !sub_condition( d );
        };
    }
    if( !found_sub_member ) {
        for( const std::string &sub_member : dialogue_data::complex_conds() ) {
            if( jo.has_member( sub_member ) ) {
                found_sub_member = true;
                break;
            }
        }
    }
    bool found = false;
    for( const condition_parser &p : parsers ) {
        if( p.has_beta ) {
            if( p.check( jo ) ) {
                condition = p.f_beta( jo, p.key_alpha, false );
                found = true;
            } else if( p.check( jo, true ) ) {
                condition = p.f_beta( jo, p.key_beta, true );
                found = true;
            }
        } else if( p.check( jo ) ) {
            condition = p.f( jo, p.key_alpha );
            if( jo.has_member( "math" ) ) {
                found_sub_member = true;
            }
            found = true;
        }
        if( found ) {
            break;
        }
    }
    if( !found ) {
        for( const std::string &sub_member : dialogue_data::simple_string_conds() ) {
            if( jo.has_string( sub_member ) ) {
                const conditional_t sub_condition( jo.get_string( sub_member ) );
                condition = [sub_condition]( const_dialogue const & d ) {
                    return sub_condition( d );
                };
                found_sub_member = true;
                break;
            }
        }
    }
    if( !found_sub_member ) {
        jo.throw_error( "unrecognized condition in " + jo.str() );
    }
}

conditional_t::conditional_t( std::string_view type )
{
    bool found = false;
    for( const condition_parser &p : parsers_simple ) {
        if( p.has_beta ) {
            if( type == p.key_alpha ) {
                condition = p.f_beta_simple( false );
                found = true;
            } else if( type == p.key_beta ) {
                condition = p.f_beta_simple( true );
                found = true;
            }
        } else if( type == p.key_alpha ) {
            condition = p.f_simple();
            found = true;
        }
        if( found ) {
            break;
        }
    }
    if( !found ) {
        condition = []( const_dialogue const & ) {
            return false;
        };
    }
}

const std::unordered_set<std::string> &dialogue_data::simple_string_conds()
{
    static std::unordered_set<std::string> ret;
    if( ret.empty() ) {
        for( const condition_parser &p : parsers_simple ) {
            ret.emplace( p.key_alpha );
            if( p.has_beta ) {
                ret.emplace( p.key_beta );
            }
        }
    }
    return ret;
}

const std::unordered_set<std::string> &dialogue_data::complex_conds()
{
    static std::unordered_set<std::string> ret;
    if( ret.empty() ) {
        for( const condition_parser &p : parsers ) {
            ret.emplace( p.key_alpha );
            if( p.has_beta ) {
                ret.emplace( p.key_beta );
            }
        }
    }
    return ret;
}
