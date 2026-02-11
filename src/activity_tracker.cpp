#include "activity_tracker.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "game_constants.h"
#include "options.h"
#include "string_formatter.h"

int activity_tracker::weariness() const
{
    if( intake > tracker ) {
        return tracker / 2000;
    }
    return ( tracker - intake * 0.5 ) / 1000;
}

// Called every 5 minutes, when activity level is logged
void activity_tracker::try_reduce_weariness( int bmr )
{
    const float recovery_mult = 0.05f;
    // As fatigue_mod approaches zero, low_activity_ticks and reduction approach infinity which in turn make tracker approach - infinity before being capped at 0.
    // Skip the math and just automatically set tracker to 0.
    if( average_activity() < LIGHT_EXERCISE ) {
        // cata_assert( fatigue_mod > 0.0f );
        low_activity_ticks += std::min( 1.0f,
                                        ( ( LIGHT_EXERCISE - average_activity() ) / ( LIGHT_EXERCISE - NO_EXERCISE ) ) );
        // Recover (by default) twice as fast while sleeping
        if( average_activity() < NO_EXERCISE ) {
            low_activity_ticks += ( NO_EXERCISE - average_activity() ) / ( NO_EXERCISE - SLEEP_EXERCISE );
        }

        const int bmr_cal = bmr * 1000;

        if( low_activity_ticks >= 1.0f ) {
            int reduction = tracker;
            // 1/120 of whichever's bigger
            if( bmr_cal > reduction ) {
                reduction = std::floor( bmr_cal * recovery_mult * low_activity_ticks / 6.0f );
            } else {
                reduction = std::ceil( reduction * recovery_mult * low_activity_ticks / 6.0f );
            }
            low_activity_ticks = 0.0f;

            tracker -= std::max( reduction, 1 );
        }
    }

    // If happens to be no reduction, character is not (as) hypoglycemic
    intake = static_cast<int>( intake * std::pow( 1 - recovery_mult, ( 1.0f / 12.0f ) ) );

    // Normalize values, make sure we stay above 0
    intake = std::max( intake, 0 );
    tracker = std::max( tracker, 0 );
    low_activity_ticks = std::max( low_activity_ticks, 0.0f );
}

void activity_tracker::weary_clear()
{
    tracker = 0;
    intake = 0;
    output = 0;
    low_activity_ticks = 0.0f;
}

int activity_tracker::debug_get_tracker() const
{
    return tracker;
}

void activity_tracker::debug_set_tracker( int new_tracker )
{
    tracker = new_tracker;
}

void activity_tracker::set_intake( int ncal )
{
    intake = ncal;
}

std::string activity_tracker::debug_weary_info() const
{
    return string_format( "Intake: %.1f Tracker: %.1f", intake / 1000.0f, output / 1000.0f );
}

void activity_tracker::calorie_adjust( int ncal )
{
    if( ncal > 0 ) {
        intake += ncal;
    } else {
        // ncal is negative, we need positive
        output -= ncal;
        tracker -= ncal;
    }
}

float activity_tracker::activity( bool sleeping ) const
{
    if( current_turn == calendar::turn ) {
        return current_activity;
    } else if( sleeping ) {
        return SLEEP_EXERCISE;
    } else {
        return NO_EXERCISE;
    }
}

float activity_tracker::average_activity() const
{
    if( activity_reset && current_turn != calendar::turn ) {
        return previous_activity / num_events;
    }
    return ( accumulated_activity + current_activity ) / num_events;
}

float activity_tracker::instantaneous_activity_level() const
{
    if( current_turn == calendar::turn ) {
        return current_activity;
    }
    return previous_turn_activity;
}

// The idea here is the character is going about their business logging activities,
// and log_activity() handles sorting them out, it records the largest magnitude for a given turn,
// and then rolls the previous turn's value into the accumulator once a new activity is logged.
// After a reset, we have to pretend the previous values weren't logged.
void activity_tracker::log_activity( float new_level )
{
    current_activity = std::max( current_activity, new_level );
    current_turn = calendar::turn;
}

void activity_tracker::new_turn( bool sleeping )
{
    float base_activity_level = sleeping ? SLEEP_EXERCISE : NO_EXERCISE;

    if( activity_reset ) {
        activity_reset = false;
        previous_turn_activity = current_activity;
        current_activity = base_activity_level;
        accumulated_activity = 0.0f;
        brisk_activity = 0.0f;
        active_activity = 0.0f;
        extra_activity = 0.0f;
        explosive_activity = 0.0f;
        brisk_activity_new = 0.0f;
        active_activity_new = 0.0f;
        extra_activity_new = 0.0f;
        explosive_activity_new = 0.0f;
        num_events = 1;
    } else {
        // This is for the last turn that had activity logged.
        accumulated_activity += current_activity;
        // High-impact activity is less efficient in terms of weariness over time.
        // Because the system is just kcal spent = weariness, we need to track strenous
        // activity separately and apply a malus to weariness later in suffer.cpp.
        if( current_activity == BRISK_EXERCISE ) {
            brisk_activity += current_activity;
        }
        if( current_activity == ACTIVE_EXERCISE ) {
            active_activity += current_activity;
        }
        if( current_activity == EXTRA_EXERCISE ) {
            extra_activity += current_activity;
        }
        if( current_activity == EXPLOSIVE_EXERCISE ) {
            explosive_activity += current_activity;
        }
        // Then handle the interventing turns that had no activity logged.
        int num_turns = to_turns<int>( calendar::turn - current_turn );
        if( num_turns > 1 ) {
            accumulated_activity += ( num_turns - 1 ) * std::min( NO_EXERCISE, current_activity );
            //
            if( brisk_activity > brisk_activity_new ) {
                brisk_activity += ( num_turns - 1 ) * std::min( NO_EXERCISE, current_activity );
                tracker += std::max( 0.0f, ( brisk_activity - brisk_activity_new ) * 12.5f );
                brisk_activity_new = brisk_activity;
            }
            if( active_activity > active_activity_new ) {
                active_activity += ( num_turns - 1 ) * std::min( NO_EXERCISE, current_activity );
                tracker += std::max( 0.0f, ( active_activity - active_activity_new ) * 25.0f );
                active_activity_new = active_activity;
            }
            if( extra_activity > extra_activity_new ) {
                extra_activity += ( num_turns - 1 ) * std::min( NO_EXERCISE, current_activity );
                tracker += std::max( 0.0f, ( extra_activity - extra_activity_new ) * 50.0f );
                extra_activity_new = extra_activity;
            }
            if( explosive_activity > explosive_activity_new ) {
                explosive_activity += ( num_turns - 1 ) * std::min( NO_EXERCISE, current_activity );
                tracker += std::max( 0.0f, ( explosive_activity - explosive_activity_new ) * 120.0f );
                explosive_activity_new = explosive_activity;
            }
            num_events += num_turns - 1;
        }
        previous_turn_activity = current_activity;
        current_activity = base_activity_level;
        num_events++;
    }
}

void activity_tracker::reset_activity_level()
{
    previous_activity = accumulated_activity;
    activity_reset = true;
}

std::string activity_tracker::activity_level_str() const
{
    for( const std::pair<const float, std::string> &member : activity_levels_str_map ) {
        if( current_activity <= member.first ) {
            return member.second;
        }
    }
    return ( --activity_levels_str_map.end() )->second;
}
