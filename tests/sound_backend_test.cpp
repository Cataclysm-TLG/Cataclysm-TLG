#if defined(SDL_SOUND)

#include "cata_catch.h"
#include "sound_backend.h"

namespace
{
bool predicate_true()
{
    return true;
}
bool predicate_false()
{
    return false;
}

static bool flag = false;
bool predicate_flag()
{
    return flag;
}
} // namespace

TEST_CASE( "sound_backend_slow_time_predicate_install_and_clear", "[sound_backend]" )
{
    sound_backend::set_slow_time_predicate( nullptr );
    CHECK_FALSE( sound_backend::slow_time_predicate_active() );

    sound_backend::set_slow_time_predicate( &predicate_true );
    CHECK( sound_backend::slow_time_predicate_active() );

    sound_backend::set_slow_time_predicate( &predicate_false );
    CHECK_FALSE( sound_backend::slow_time_predicate_active() );

    sound_backend::set_slow_time_predicate( nullptr );
    CHECK_FALSE( sound_backend::slow_time_predicate_active() );
}

TEST_CASE( "sound_backend_slow_time_predicate_reflects_caller_state", "[sound_backend]" )
{
    sound_backend::set_slow_time_predicate( &predicate_flag );

    flag = true;
    CHECK( sound_backend::slow_time_predicate_active() );

    flag = false;
    CHECK_FALSE( sound_backend::slow_time_predicate_active() );

    flag = true;
    CHECK( sound_backend::slow_time_predicate_active() );

    sound_backend::set_slow_time_predicate( nullptr );
}

TEST_CASE( "sound_backend_poll_is_safe_without_init", "[sound_backend]" )
{
    // poll() is called every frame from refresh_display(); it must be safe
    // to call regardless of whether backend::init has ever run.
    sound_backend::poll();
    sound_backend::poll();
    SUCCEED();
}

#endif // SDL_SOUND
