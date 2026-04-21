#if defined(SDL_SOUND) && defined(USE_SDL3)

#include "sound_backend.h"

#include <cstddef>
#include <string>

namespace sound_backend
{

struct sfx_audio {};
struct music_source {};

bool init( int /*frequency*/, int /*out_channels*/, int /*chunk_size*/,
           const init_options & /*opts*/ )
{
    return true;
}

void shutdown() {}

sfx_audio *load_sfx( const std::string & /*path*/ )
{
    return nullptr;
}

void free_sfx( sfx_audio * /*a*/ ) {}

music_source *load_music( const std::string & /*path*/ )
{
    return nullptr;
}

void free_music( music_source * /*m*/ ) {}

void play_reserved( sfx_audio * /*a*/, sfx::channel /*slot*/,
                    const play_opts & /*opts*/ ) {}

void play_oneshot( sfx_audio * /*a*/, const play_opts & /*opts*/ ) {}

void tag_reserved_groups( const std::array<sfx::group,
                          static_cast<size_t>( sfx::channel::MAX_CHANNEL )> &/*assignments*/ ) {}

void stop_reserved( sfx::channel /*slot*/, int /*fade_out_ms*/ ) {}

bool is_reserved_playing( sfx::channel /*slot*/ )
{
    return false;
}

bool is_reserved_fading( sfx::channel /*slot*/ )
{
    return false;
}

int set_reserved_volume( sfx::channel /*slot*/, int /*vol*/ )
{
    return -1;
}

int get_reserved_volume( sfx::channel /*slot*/ )
{
    return 0;
}

void set_reserved_position( sfx::channel /*slot*/, int /*angle_deg*/, int /*distance*/ ) {}

static bool_predicate g_slow_time_predicate = nullptr;

void set_slow_time_predicate( bool_predicate fn )
{
    g_slow_time_predicate = fn;
}

bool slow_time_predicate_active()
{
    return g_slow_time_predicate != nullptr && g_slow_time_predicate();
}

void stop_all_sfx( int /*fade_out_ms*/ ) {}

void fade_group( sfx::group /*g*/, int /*ms*/ ) {}

void poll() {}

void set_music_finished_cb( music_finished_cb /*cb*/ ) {}

bool play_music( music_source * /*m*/, int /*loops*/, int /*fade_in_ms*/ )
{
    return false;
}

void stop_music( int /*fade_out_ms*/ ) {}

void set_music_volume( int /*vol*/ ) {}

bool is_music_playing()
{
    return false;
}

} // namespace sound_backend

#endif // SDL_SOUND && USE_SDL3
