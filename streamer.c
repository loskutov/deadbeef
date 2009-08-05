#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <samplerate.h>
#include "threading.h"
#include "codec.h"
#include "playlist.h"
#include "common.h"
#include "streamer.h"

extern int sdl_player_freq; // hack!
static SRC_STATE *src;
static SRC_DATA srcdata;
static int codecleft;
static char g_readbuffer[200000]; // hack!
static float g_fbuffer[200000]; // hack!
static float g_srcbuffer[200000]; // hack!
static int streaming_terminate;

#define STREAM_BUFFER_SIZE 200000
static int streambuffer_fill;
static char streambuffer[STREAM_BUFFER_SIZE+1000];
static uintptr_t mutex;
static int nextsong = -1;

void
streamer_set_nextsong (int song) {
    nextsong = song;
}

static int
streamer_read_async (char *bytes, int size);

void
streamer_thread (uintptr_t ctx) {
    codecleft = 0;

    while (!streaming_terminate) {
        if (nextsong >= 0) {
            int sng = nextsong;
            nextsong = -1;
            codec_lock ();
            //streambuffer_fill = 0;
            codecleft = 0;
            codec_unlock ();
            if (ps_set_current (ps_get_for_idx (sng)) < 0) {
                while (ps_nextsong () < 0) {
                    usleep (3000);
                }
            }
        }
        streamer_lock ();
        if (streambuffer_fill < STREAM_BUFFER_SIZE) {
            int sz = STREAM_BUFFER_SIZE - streambuffer_fill;
            int minsize = 512;
            if (streambuffer_fill < 16384) {
                minsize = 16384;
            }
            sz = min (minsize, sz);
            int bytesread = streamer_read_async (&streambuffer[streambuffer_fill], sz);
            //printf ("req=%d, got=%d\n", sz, bytesread);
            streambuffer_fill += bytesread;
        }
        streamer_unlock ();
        usleep (3000);
        //printf ("fill: %d        \r", streambuffer_fill);
    }

    if (src) {
        src_delete (src);
        src = NULL;
    }
}

int
streamer_init (void) {
    mutex = mutex_create ();
//    src = src_new (SRC_SINC_BEST_QUALITY, 2, NULL);
    src = src_new (SRC_LINEAR, 2, NULL);
    if (!src) {
        return -1;
    }
    thread_start (streamer_thread, 0);
    return 0;
}

void
streamer_free (void) {
    streaming_terminate = 1;
    mutex_free (mutex);
}

void
streamer_reset (void) { // must be called when current song changes by external reasons
    codecleft = 0;
    src_reset (src);
}

// returns number of bytes been read
static int
streamer_read_async (char *bytes, int size) {
    int initsize = size;
    for (;;) {
        int bytesread = 0;
        codec_lock ();
        codec_t *codec = playlist_current.codec;
        if (!codec) {
            codec_unlock ();
            break;
        }
        if (codec->info.samplesPerSecond != -1) {
            int nchannels = codec->info.channels;
            int samplerate = codec->info.samplesPerSecond;
            // read and do SRC
            if (codec->info.samplesPerSecond == sdl_player_freq) {
                int i;
                if (codec->info.channels == 2) {
                    bytesread = codec->read (bytes, size);
                    codec_unlock ();
                }
                else {
                    bytesread = codec->read (g_readbuffer, size/2);
                    codec_unlock ();
                    for (i = 0; i < size/4; i++) {
                        int16_t sample = (int16_t)(((int32_t)(((int16_t*)g_readbuffer)[i])));
                        ((int16_t*)bytes)[i*2+0] = sample;
                        ((int16_t*)bytes)[i*2+1] = sample;
                    }
                    bytesread *= 2;
                }
            }
            else {
                int nsamples = size/4;
                // convert to codec samplerate
                nsamples = nsamples * samplerate / sdl_player_freq * 2;
                if (!src_is_valid_ratio ((double)sdl_player_freq/samplerate)) {
                    printf ("invalid ratio! %d / %d = %f", sdl_player_freq, samplerate, (float)sdl_player_freq/samplerate);
                }
                assert (src_is_valid_ratio ((double)sdl_player_freq/samplerate));
                // read data at source samplerate (with some room for SRC)
                int nbytes = (nsamples - codecleft) * 2 * nchannels;
                if (nbytes < 0) {
                    nbytes = 0;
                }
                else {
                    //printf ("reading %d bytes from mp3\n", nbytes);
                    bytesread = codec->read (g_readbuffer, nbytes);
                }
                codec_unlock ();
                // recalculate nsamples according to how many bytes we've got
                nsamples = bytesread / (2 * nchannels) + codecleft;
                // convert to float
                int i;
                float *fbuffer = g_fbuffer + codecleft*2;
                if (nchannels == 2) {
                    for (i = 0; i < (nsamples - codecleft) * 2; i++) {
                        fbuffer[i] = ((int16_t *)g_readbuffer)[i]/32767.f;
                    }
                }
                else if (nchannels == 1) { // convert mono to stereo
                    for (i = 0; i < (nsamples - codecleft); i++) {
                        fbuffer[i*2+0] = ((int16_t *)g_readbuffer)[i]/32767.f;
                        fbuffer[i*2+1] = fbuffer[i*2+0];
                    }
                }
                //codec_lock ();
                // convert samplerate
                srcdata.data_in = g_fbuffer;
                srcdata.data_out = g_srcbuffer;
                srcdata.input_frames = nsamples;
                srcdata.output_frames = size/4;
                srcdata.src_ratio = (double)sdl_player_freq/samplerate;
                srcdata.end_of_input = 0;
    //            src_set_ratio (src, srcdata.src_ratio);
                src_process (src, &srcdata);
                //codec_unlock ();
                // convert back to s16 format
                nbytes = size;
                int genbytes = srcdata.output_frames_gen * 4;
                bytesread = min(size, genbytes);
                for (i = 0; i < bytesread/2; i++) {
                    float sample = g_srcbuffer[i];
                    if (sample > 1) {
                        sample = 1;
                    }
                    if (sample < -1) {
                        sample = -1;
                    }
                    ((int16_t*)bytes)[i] = (int16_t)(sample*32767.f);
                }
                // calculate how many unused input samples left
                codecleft = nsamples - srcdata.input_frames_used;
                // copy spare samples for next update
                memmove (g_fbuffer, &g_fbuffer[srcdata.input_frames_used*2], codecleft * 8);
            }
        }
        else {
            codec_unlock ();
        }
        bytes += bytesread;
        size -= bytesread;
        if (size == 0) {
            return initsize;
        }
        else {
            // that means EOF
            while (ps_nextsong () < 0) {
                usleep (3000);
            }
        }
    }
    return initsize - size;
}

void
streamer_lock (void) {
    mutex_lock (mutex);
}

void
streamer_unlock (void) {
    mutex_unlock (mutex);
}

int
streamer_read (char *bytes, int size) {
    streamer_lock ();
    int sz = min (size, streambuffer_fill);
    if (sz) {
        memcpy (bytes, streambuffer, sz);
        if (sz < streambuffer_fill) {
            // shift buffer
            memmove (streambuffer, &streambuffer[sz], streambuffer_fill-sz);
        }
        streambuffer_fill -= sz;
    }
    streamer_unlock ();
    return sz;
}
