#define _GNU_SOURCE

/* Lets the app's PulseAudio client (Debian's libpulse 16.1) create playback
 * streams on the TV's PulseAudio.
 *
 * The TV runs LG's PulseAudio 15, protocol version 35 like ours, but its
 * CREATE_PLAYBACK_STREAM request carries one more field than upstream's: a
 * boolean after the format list (captured from LG's own pacat). Without it the
 * server's parser runs out of data and drops the client ("protocol error,
 * kicking client"), so Wine had no sound (DirectSound error 8889000F).
 * Record streams are unchanged.
 *
 * LG's server also refuses variable-rate playback streams ("Not supported");
 * Wine's sound driver asks for one on every playback stream. Variable rate
 * only serves clock adjustment, so the flag is dropped.
 *
 * libpulse sends every command through libpulsecommon's
 * pa_pstream_send_tagstruct_with_creds(); this wraps it and appends false to
 * CREATE_PLAYBACK_STREAM. Preloaded into the app's aarch64 programs through
 * /tmp/wine-tv/prelo (written by wine-tv on the TV only).
 */

#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PA_COMMAND_CREATE_PLAYBACK_STREAM 3
#define PA_STREAM_VARIABLE_RATE 0x0400U

typedef struct pa_pstream pa_pstream;
typedef struct pa_tagstruct pa_tagstruct;
typedef struct pa_creds pa_creds;
typedef struct pa_stream pa_stream;
typedef struct pa_buffer_attr pa_buffer_attr;
typedef struct pa_cvolume pa_cvolume;

/* A library the caller has loaded, possibly with RTLD_LOCAL (Wine's sound
 * driver does), where RTLD_NEXT and RTLD_DEFAULT cannot see it. */
static void *loaded(const char *name)
{
    void *h = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);

    return h ? h : RTLD_NEXT;
}

int pa_stream_connect_playback(pa_stream *s, const char *dev, const pa_buffer_attr *attr,
                               unsigned flags, const pa_cvolume *volume, pa_stream *sync_stream)
{
    static int (*real)(pa_stream *, const char *, const pa_buffer_attr *, unsigned,
                       const pa_cvolume *, pa_stream *);

    if (!real)
        real = (int (*)(pa_stream *, const char *, const pa_buffer_attr *, unsigned,
                        const pa_cvolume *, pa_stream *))dlsym(loaded("libpulse.so.0"), __func__);
    if (!real)
        return -1;
    return real(s, dev, attr, flags & ~PA_STREAM_VARIABLE_RATE, volume, sync_stream);
}

void pa_pstream_send_tagstruct_with_creds(pa_pstream *p, pa_tagstruct *t, const pa_creds *creds)
{
    static void (*real)(pa_pstream *, pa_tagstruct *, const pa_creds *);
    static const uint8_t *(*data)(pa_tagstruct *, size_t *);
    static void (*put_boolean)(pa_tagstruct *, bool);
    const uint8_t *d;
    size_t len = 0;

    if (!real) {
        /* Loaded by now: its libpulse is the caller. */
        void *common = loaded("libpulsecommon-16.1.so");

        real = (void (*)(pa_pstream *, pa_tagstruct *, const pa_creds *))dlsym(common, __func__);
        data = (const uint8_t *(*)(pa_tagstruct *, size_t *))dlsym(common, "pa_tagstruct_data");
        put_boolean = (void (*)(pa_tagstruct *, bool))dlsym(common, "pa_tagstruct_put_boolean");
        if (!real)
            return;
    }
    /* A command starts with its number as a tagged u32: 'L' + big-endian. */
    if (data && put_boolean && (d = data(t, &len)) && len >= 5 && d[0] == 'L' &&
        d[1] == 0 && d[2] == 0 && d[3] == 0 && d[4] == PA_COMMAND_CREATE_PLAYBACK_STREAM)
        put_boolean(t, false);
    real(p, t, creds);
}
