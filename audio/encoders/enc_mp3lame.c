#include "audio/encoders.h"
#include "filters/audio/internal.h"

#include "lame/lame.h"
#include <assert.h>

typedef struct enc_lame_t
{
    audio_info_t info;
    hnd_t filter_chain;
    int64_t packet_count;

    int finishing;
    lame_global_flags *lame;
    int64_t last_sample;
    uint8_t *buffer;
    size_t bufsize;
    audio_packet_t *in;
} enc_lame_t;

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    assert( filter_chain );
    audio_hnd_t *chain = filter_chain;
    if( chain->info.channels > 2 )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "only mono or stereo audio is supported\n" );
        return 0;
    }
    enc_lame_t *h   = calloc( 1, sizeof( enc_lame_t ) );
    h->filter_chain = chain;
    h->info         = chain->info;

    char **opts = x264_split_options( opt_str, (const char*[]){ "bitrate", "vbr", "quality", NULL } );
    assert( opts );

    char *cbr   = x264_get_option( "bitrate", opts );
    char *vbr   = x264_get_option( "vbr", opts );
    float brval = x264_otof( vbr, 6.0 );
    brval       = x264_otof( cbr, brval );
    int quality = x264_otoi( x264_get_option( "quality", opts ), 0 );

    x264_free_string_array( opts );
    if( cbr && vbr )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "both bitrate and quality mode specified" );
        return 0;
    }

    h->info.extradata      = NULL;
    h->info.extradata_size = 0;

    h->lame = lame_init();
    // lame expects floats to be in the same range as shorts, our floats are -1..1 so tell it to scale
    lame_set_scale( h->lame, 32768 );
    lame_set_in_samplerate( h->lame, h->info.samplerate );
    lame_set_num_channels( h->lame, h->info.channels );
    lame_set_quality( h->lame, quality );
    lame_set_VBR( h->lame, vbr_default );

    if( cbr )
    {
        lame_set_VBR( h->lame, vbr_off );
        lame_set_brate( h->lame, (int) brval );
    }
    else
        lame_set_VBR_quality( h->lame, brval );

    lame_init_params( h->lame );

    h->info.framelen   = lame_get_framesize( h->lame );
    h->info.framesize  = h->info.framelen * 2;
    h->info.chansize   = 2;
    h->info.samplesize = 2 * h->info.channels;
    h->info.timebase   = (timebase_t) { 1, h->info.samplerate };
    h->info.samplerate = lame_get_out_samplerate( h->lame );

    h->bufsize = 125 * h->info.framelen / 100 + 7200; // from lame.h, largest frame that the encoding functions may return

    x264_cli_log( "audio", X264_LOG_INFO, "opened lame mp3 encoder (%s: %g%s)\n",
                  ( cbr ? "bitrate" : "VBR" ), brval,
                  ( cbr ? "kbps" : "" ) );

    return h;
}

static const char *get_codec_name( hnd_t handle )
{
    return "mp3";
}

static audio_info_t *get_info( hnd_t handle )
{
    assert( handle );
    enc_lame_t *h = handle;

    return &h->info;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_lame_t *h = handle;
    if( h->finishing )
        return NULL;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->info = h->info;
    out->data = malloc( h->bufsize );

    while( !out->size )
    {
        if( h->in && h->in->flags & AUDIO_FLAG_EOF )
        {
            h->finishing = 1;
            goto error; // Not an error here but it'd do the same handling
        }
        x264_af_free_packet( h->in );

        if( !( h->in = x264_af_get_samples( h->filter_chain, h->last_sample, h->last_sample + h->info.framelen ) ) )
            goto error;
        out->dts        = h->last_sample;
        h->last_sample += h->in->samplecount;

        out->size = lame_encode_buffer_float( h->lame, h->in->samples[0], h->in->samples[1],
                                              h->in->samplecount, out->data, h->bufsize );
    }
    return out;

error:
    x264_af_free_packet( h->in );
    x264_af_free_packet( out );
    return NULL;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    ((enc_lame_t*)handle)->last_sample += samplecount;
}

static audio_packet_t *finish( hnd_t encoder )
{
    enc_lame_t *h = encoder;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    out->dts  = h->last_sample + h->in->samplecount * ++h->finishing; // HACK
    out->info = h->info;
    out->data = malloc( h->bufsize );
    out->size = lame_encode_flush( h->lame, out->data, h->bufsize );
    if( !out->size )
        goto error;
    return out;

error:
    x264_af_free_packet( out );
    return NULL;
}

static void mp3_close( hnd_t handle )
{
    enc_lame_t *h = handle;

    lame_close( h->lame );
    free( h );
}

const audio_encoder_t audio_encoder_mp3 =
{
    .init            = init,
    .get_codec_name  = get_codec_name,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = mp3_close
};
