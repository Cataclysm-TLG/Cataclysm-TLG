#include "rng.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "calendar.h"
#include "cata_utility.h"
#include "units.h"

unsigned int rng_bits()
{
    // Whole uint range.
    static std::uniform_int_distribution<unsigned int> rng_uint_dist;
    return rng_uint_dist( rng_get_engine() );
}

int rng( int lo, int hi )
{
    static std::uniform_int_distribution<int> rng_int_dist;
    if( lo > hi ) {
        std::swap( lo, hi );
    }
    return rng_int_dist( rng_get_engine(), std::uniform_int_distribution<>::param_type( lo, hi ) );
}

double rng_float( double lo, double hi )
{
    static std::uniform_real_distribution<double> rng_real_dist;
    if( lo > hi ) {
        std::swap( lo, hi );
    }
    if( std::isfinite( lo ) && std::isfinite( hi ) ) {
        return rng_real_dist( rng_get_engine(), std::uniform_real_distribution<>::param_type( lo, hi ) );
    }
    debugmsg( "rng_float called with nan/inf" );
    return 0;
}

units::angle random_direction()
{
    return rng_float( 0_pi_radians, 2_pi_radians );
}

// Returns the area under a curve with provided standard deviation and center
// from difficulty to positive infinity. That is, the chance that a normal roll on
// said curve will return a value of difficulty or greater.
float normal_roll_chance( float center, float stddev, float difficulty )
{
    cata_assert( stddev >= 0.f );
    // We're going to be using them a lot, so let's name our variables.
    // M = the given "center" of the curve
    // S = the given standard deviation of the curve
    // A = the difficulty
    // So, the equation of the normal curve is...
    // y = (1.f/(S*std::sqrt(2 * M_PI))) * exp(-(std::pow(x - M, 2))/(2 * std::pow(S, 2)))
    // Thanks to wolfram alpha, we know the integral of that from A to B to be
    // 0.5 * (erf((M-A)/(std::sqrt(2) * S)) - erf((M-B)/(std::sqrt(2) * S)))
    // And since we know B to be infinity, we can simplify that to
    // 0.5 * (erfc((A-m)/(std::sqrt(2)* S))+sgn(S)-1) (as long as S != 0)
    // Wait a second, what are erf, erfc and sgn?
    // Oh, those are the error function, complementary error function, and sign function
    // Luckily, erf() is provided to us in math.h, and erfc is just 1 - erf
    // Sign is pretty obvious x > 0 ? x == 0 ? 0 : 1 : -1;
    // Since we know S will always be > 0, that term vanishes.

    // With no standard deviation, we will always return center
    if( stddev == 0.f ) {
        return ( center > difficulty ) ? 1.f : 0.f;
    }

    float numerator = difficulty - center;
    float denominator = std::sqrt( 2 ) * stddev;
    float compl_erf = 1.f - std::erf( numerator / denominator );
    return 0.5 * compl_erf;
}

double normal_roll( double mean, double stddev )
{
    static std::normal_distribution<double> rng_normal_dist;
    return rng_normal_dist( rng_get_engine(), std::normal_distribution<>::param_type( mean, stddev ) );
}

double exponential_roll( double lambda )
{
    static std::exponential_distribution<double> rng_exponential_dist;
    return rng_exponential_dist( rng_get_engine(),
                                 std::exponential_distribution<>::param_type( lambda ) );
}

double chi_squared_roll( double trial_num )
{
    static std::chi_squared_distribution<double> rng_chi_squared_dist;
    return rng_chi_squared_dist( rng_get_engine(),
                                 std::chi_squared_distribution<>::param_type( trial_num ) );
}

double rng_exponential( double min, double mean )
{
    const double adjusted_mean = mean - min;
    if( adjusted_mean <= 0.0 ) {
        return 0.0;
    }
    // lambda = 1 / mean
    return min + exponential_roll( 1.0 / adjusted_mean );
}

bool one_in( int chance )
{
    return chance <= 1 || rng( 0, chance - 1 ) == 0;
}

bool one_turn_in( const time_duration &duration )
{
    return one_in( to_turns<int>( duration ) );
}

bool x_in_y( double x, double y )
{
    return rng_float( 0.0, 1.0 ) <= x / y;
}

int dice( int number, int sides )
{
    int ret = 0;
    for( int i = 0; i < number; i++ ) {
        ret += rng( 1, sides );
    }
    return ret;
}

// probabilistically round a double to an int
// 1.3 has a 70% chance of rounding to 1, 30% chance to 2.
int roll_remainder( double value )
{
    double integ;
    double frac = modf( value, &integ );
    if( value > 0.0 && value > integ && x_in_y( frac, 1.0 ) ) {
        integ++;
    } else if( value < 0.0 && value < integ && x_in_y( -frac, 1.0 ) ) {
        integ--;
    }

    return integ;
}

// http://www.cse.yorku.ca/~oz/hash.html
// for world seeding.
int djb2_hash( const unsigned char *input )
{
    unsigned int hash = 5381;
    unsigned char c = *input++;
    while( c != '\0' ) {
        hash = ( ( hash << 5 ) + hash ) + c; /* hash * 33 + c */
        c = *input++;
    }
    return hash;
}

std::vector<int> rng_sequence( size_t count, int lo, int hi, int seed )
{
    if( lo > hi ) {
        std::swap( lo, hi );
    }
    std::vector<int> result;
    result.reserve( count );

    // NOLINTNEXTLINE(cata-determinism)
    cata_default_random_engine eng( seed );
    std::uniform_int_distribution<int> rng_int_dist;
    const std::uniform_int_distribution<int>::param_type param( lo, hi );
    for( size_t i = 0; i < count; i++ ) {
        result.push_back( rng_int_dist( eng, param ) );
    }
    return result;
}

double rng_normal( double lo, double hi )
{
    if( lo > hi ) {
        std::swap( lo, hi );
    }

    const double range = ( hi - lo ) / 4;
    if( range == 0.0 ) {
        return hi;
    }
    double val = normal_roll( ( hi + lo ) / 2, range );
    return clamp( val, lo, hi );
}

cata_default_random_engine::result_type rng_get_first_seed()
{
    using rep = std::chrono::high_resolution_clock::rep;
    static rep seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return static_cast<cata_default_random_engine::result_type>( seed );
}

cata_default_random_engine &rng_get_engine()
{
    // NOLINTNEXTLINE(cata-determinism)
    static cata_default_random_engine eng( rng_get_first_seed() );
    return eng;
}

void rng_set_engine_seed( unsigned int seed )
{
    if( seed != 0 ) {
        rng_get_engine().seed( seed );
    }
}

std::string random_string( size_t length )
{
    auto randchar = []() -> char {
        // NOLINTNEXTLINE(modernize-avoid-c-arrays)
        static constexpr char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
        static constexpr size_t num_chars = sizeof( charset ) - 1;
        return charset[rng( 0, num_chars - 1 )];
    };
    std::string str( length, 0 );
    std::generate_n( str.begin(), length, randchar );
    return str;
}

distribution::distribution()
{
    generator_function = []() {
        return 0.0f;
    };
}

distribution::distribution( const distribution &d )
{
    generator_function = d.generator_function;
}

distribution::distribution( const std::function<float()> &gen )
{
    generator_function = gen;
}

float distribution::roll() const
{
    return generator_function();
}

distribution distribution::constant( float val )
{
    return distribution( [val]() {
        return val;
    } );
}

distribution distribution::one_in( float in )
{
    if( in <= 1.0f ) {
        debugmsg( "Invalid one_in: %.2f", in );
        return distribution();
    }

    return distribution( [in]() {
        return x_in_y( 1, in );
    } );
}

distribution distribution::rng_roll( int from, int to )
{
    return distribution( [from, to]() -> float {
        return rng( from, to );
    } );
}

distribution distribution::dice_roll( int sides, int size )
{
    if( sides < 1 || size < 1 ) {
        debugmsg( "Invalid dice: %d sides, %d sizes", sides, size );
        return distribution();
    }

    return distribution( [sides, size]() -> float {
        return dice( sides, size );
    } );
}

distribution distribution::operator+( const distribution &other ) const
{
    auto my_fun = generator_function;
    auto other_fun = other.generator_function;
    return distribution( [my_fun, other_fun]() {
        return my_fun() + other_fun();
    } );
}

distribution distribution::operator*( const distribution &other ) const
{
    auto my_fun = generator_function;
    auto other_fun = other.generator_function;
    return distribution( [my_fun, other_fun]() {
        return my_fun() * other_fun();
    } );
}

distribution &distribution::operator=( const distribution &other ) = default;

distribution load_distribution( const JsonObject &jo )
{
    if( jo.has_float( "constant" ) ) {
        return distribution::constant( jo.get_float( "constant" ) );
    }
    if( jo.has_float( "one_in" ) ) {
        return distribution::one_in( jo.get_float( "one_in" ) );
    }
    if( jo.has_array( "dice" ) ) {
        JsonArray jarr = jo.get_array( "dice" );
        return distribution::dice_roll( jarr.get_int( 0 ), jarr.get_int( 1 ) );
    }
    if( jo.has_array( "rng" ) ) {
        JsonArray jarr = jo.get_array( "rng" );
        return distribution::rng_roll( jarr.get_int( 0 ), jarr.get_int( 1 ) );
    }
    if( jo.has_array( "sum" ) ) {
        JsonArray jarr = jo.get_array( "sum" );
        distribution ret = load_distribution( jarr.next_object() );
        while( jarr.has_more() ) {
            ret = ret + load_distribution( jarr.next_object() );
        }
        return ret;
    }
    if( jo.has_array( "mul" ) ) {
        JsonArray jarr = jo.get_array( "mul" );
        distribution ret = load_distribution( jarr.next_object() );
        while( jarr.has_more() ) {
            ret = ret * load_distribution( jarr.next_object() );
        }
        return ret;
    }
    jo.throw_error( "Invalid distribution" );
}

distribution load_distribution( const JsonObject &jo, std::string_view name )
{
    if( !jo.has_member( name ) ) {
        return distribution();
    }

    if( jo.has_float( name ) ) {
        return distribution::constant( jo.get_float( name ) );
    }

    if( jo.has_object( name ) ) {
        JsonObject obj = jo.get_object( name );
        return load_distribution( obj );
    }

    jo.throw_error_at( name, "Invalid distribution type" );
}
