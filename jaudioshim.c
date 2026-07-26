/*
 * audioshim.c — couche minimaliste au-dessus de PortAudio.
 *
 * Expose des fonctions C simples et non bloquantes, appelables depuis J
 * via 'cd' :
 *
 *   shim_init(sample_rate, channels)  -> 0 si ok
 *   shim_play(data, nframes)          -> remplace le son courant (fade/crossfade)
 *   shim_stop()                       -> fade-out puis silence, abandonne le son
 *   shim_pause()                      -> fige la lecture (fade court, position conservee)
 *   shim_resume()                     -> reprend la lecture (fade court)
 *   shim_set_gain(gain)               -> volume global, 1.0 = inchange
 *   shim_position()                   -> position de lecture en frames (-1 si idle)
 *   shim_seek(frame)                  -> deplace la position de lecture
 *   shim_status()                     -> 0 idle / 1 fade-in / 2 steady / 3 crossfade / 4 fade-out
 *   shim_terminate()
 *
 * Le buffer passé à shim_play est COPIE en interne (memcpy) : l'appelant
 * peut libérer/réutiliser son tableau immédiatement après l'appel.
 *
 * Format attendu : float32, entrelacé sur 'channels' voies, valeurs -1..1.
 * 'nframes' est un nombre de FRAMES (pas un nombre de floats total).
 *
 * Compilation :
 *   gcc -shared -fPIC -O2 -o libaudioshim.so audioshim.c -lportaudio -lpthread
 */

#define _GNU_SOURCE
#include <portaudio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

typedef enum {
    ST_IDLE   = 0,
    ST_FADEIN = 1,
    ST_STEADY = 2,
    ST_XFADE  = 3,
    ST_FADEOUT= 4
} state_t;

static PaStream *g_stream = NULL;
static int  g_channels  = 1;
static long g_fade_len  = 0;   /* durée du fade play/stop/pause, en frames */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* voix "active" (ce qui doit dominer) et "outgoing" (ce qui s'éteint) */
static float *g_active   = NULL; static long g_active_len = 0,   g_active_pos = 0;
static float *g_outgoing = NULL; static long g_outgoing_len = 0, g_outgoing_pos = 0;
static long g_fade_counter = 0;
static state_t g_state = ST_IDLE;

/* pause : rampe independante superposee a l'etat ci-dessus */
static int   g_paused     = 0;     /* 0 = lecture normale, 1 = pause demandee */
static float g_pause_ramp = 1.0f;  /* 1.0 = plein volume, 0.0 = gele/silence */

/* volume global */
static float g_gain = 1.0f;

/* ---- callback temps réel : jamais de malloc/free ici, jamais d'appel bloquant ---- */
static int callback(const void *input, void *output, unsigned long frameCount,
                    const PaStreamCallbackTimeInfo *timeInfo,
                    PaStreamCallbackFlags statusFlags, void *userData)
{
    (void)input; (void)timeInfo; (void)statusFlags; (void)userData;
    float *out = (float *)output;

    pthread_mutex_lock(&g_lock);
    for (unsigned long i = 0; i < frameCount; i++) {

        /* --- rampe de pause : lisse la transition, evite tout clic --- */
        float pause_target = g_paused ? 0.0f : 1.0f;
        float pause_step = g_fade_len > 0 ? 1.0f / (float)g_fade_len : 1.0f;
        if (g_pause_ramp < pause_target) {
            g_pause_ramp += pause_step;
            if (g_pause_ramp > pause_target) g_pause_ramp = pause_target;
        } else if (g_pause_ramp > pause_target) {
            g_pause_ramp -= pause_step;
            if (g_pause_ramp < pause_target) g_pause_ramp = pause_target;
        }
        /* gele : on n'avance plus aucune position/compteur tant que la
         *          rampe n'a pas fini de descendre a 0 */
        int frozen = (g_paused && g_pause_ramp <= 0.0f);

        float gain_active = 0.0f, gain_outgoing = 0.0f;

        switch (g_state) {
            case ST_IDLE:
                break;
            case ST_FADEIN:
                gain_active = g_fade_len > 0 ? (float)g_fade_counter / (float)g_fade_len : 1.0f;
                if (gain_active > 1.0f) gain_active = 1.0f;
                break;
            case ST_STEADY:
                gain_active = 1.0f;
                break;
            case ST_XFADE:
                gain_active = g_fade_len > 0 ? (float)g_fade_counter / (float)g_fade_len : 1.0f;
                if (gain_active > 1.0f) gain_active = 1.0f;
                gain_outgoing = 1.0f - gain_active;
            break;
            case ST_FADEOUT:
                gain_outgoing = g_fade_len > 0 ? 1.0f - (float)g_fade_counter / (float)g_fade_len : 0.0f;
                if (gain_outgoing < 0.0f) gain_outgoing = 0.0f;
                break;
        }

        float mix = g_pause_ramp * g_gain;

        for (int c = 0; c < g_channels; c++) {
            float a = 0.0f, o = 0.0f;
            if (g_active && g_active_pos < g_active_len)
                a = g_active[g_active_pos * g_channels + c];
            if (g_outgoing && g_outgoing_pos < g_outgoing_len)
                o = g_outgoing[g_outgoing_pos * g_channels + c];
            out[i * g_channels + c] = mix * (gain_active * a + gain_outgoing * o);
        }

        if (frozen) continue;   /* rien n'avance tant qu'on est vraiment en pause */

            if (g_active)   g_active_pos++;
            if (g_outgoing) g_outgoing_pos++;

            switch (g_state) {
                case ST_FADEIN:
                    g_fade_counter++;
                    if (g_active_pos >= g_active_len) {
                        free(g_active); g_active = NULL;
                        g_state = ST_IDLE; g_fade_counter = 0;
                    } else if (g_fade_counter >= g_fade_len) {
                        g_state = ST_STEADY;
                    }
                    break;
                case ST_STEADY:
                    if (g_active_pos >= g_active_len) {
                        free(g_active); g_active = NULL;
                        g_state = ST_IDLE;
                    }
                    break;
                case ST_XFADE:
                    g_fade_counter++;
                    if (g_outgoing && g_outgoing_pos >= g_outgoing_len) {
                        free(g_outgoing); g_outgoing = NULL;
                    }
                    if (g_fade_counter >= g_fade_len || !g_outgoing) {
                        if (g_outgoing) { free(g_outgoing); g_outgoing = NULL; }
                        g_fade_counter = 0;
                        if (g_active && g_active_pos < g_active_len) {
                            g_state = ST_STEADY;
                        } else {
                            if (g_active) { free(g_active); g_active = NULL; }
                            g_state = ST_IDLE;
                        }
                    }
                    break;
                case ST_FADEOUT:
                    g_fade_counter++;
                    if (g_fade_counter >= g_fade_len || !g_outgoing || g_outgoing_pos >= g_outgoing_len) {
                        if (g_outgoing) { free(g_outgoing); g_outgoing = NULL; }
                        g_fade_counter = 0;
                        g_state = ST_IDLE;
                    }
                    break;
                default:
                    break;
            }
    }
    pthread_mutex_unlock(&g_lock);
    return paContinue;
}

/* ---- API exportee, appelee depuis J (thread REPL) ---- */

EXPORT int shim_init(int sample_rate, int channels) {
    if (channels < 1) channels = 1;
    g_channels = channels;
    g_fade_len = (long)(0.01 * sample_rate);   /* 10 ms de fade */
    if (g_fade_len < 1) g_fade_len = 1;

    g_paused = 0;
    g_pause_ramp = 1.0f;
    g_gain = 1.0f;

    PaError err = Pa_Initialize();
    if (err != paNoError) return -1;

    /* Cherche en priorite un device nomme "pulse"/"pipewire" : evite de
     *      prendre la carte ALSA en acces exclusif, ce qui empecherait toute
     *      autre application (navigateur, etc.) de jouer du son en meme temps. */
    PaDeviceIndex device = paNoDevice;
    int n = Pa_GetDeviceCount();
    for (int i = 0; i < n; i++) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels < channels) continue;
        if (strcasestr(info->name, "pulse") || strcasestr(info->name, "pipewire")) {
            device = i;
            break;
        }
    }
    if (device == paNoDevice) device = Pa_GetDefaultOutputDevice();
    if (device == paNoDevice) { Pa_Terminate(); return -4; }

    PaStreamParameters outParams;
    outParams.device = device;
    outParams.channelCount = channels;
    outParams.sampleFormat = paFloat32;
    outParams.suggestedLatency = Pa_GetDeviceInfo(device)->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = NULL;

    err = Pa_OpenStream(&g_stream,
                        NULL,          /* pas d'entree */
                        &outParams,
                        sample_rate,
                        256,           /* frames par buffer */
                        paNoFlag,
                        callback,
                        NULL);
    if (err != paNoError) { Pa_Terminate(); return -2; }

    err = Pa_StartStream(g_stream);
    if (err != paNoError) { Pa_CloseStream(g_stream); Pa_Terminate(); return -3; }

    return 0;
}

EXPORT int shim_play(const float *data, int nframes) {
    if (!g_stream || nframes <= 0 || !data) return -1;

    size_t nbytes = (size_t)nframes * (size_t)g_channels * sizeof(float);
    float *buf = (float *)malloc(nbytes);
    if (!buf) return -2;
    memcpy(buf, data, nbytes);

    pthread_mutex_lock(&g_lock);
    g_paused = 0;
    g_pause_ramp = 1.0f;   /* un nouveau play annule toute pause en cours */

    if (g_state == ST_IDLE) {
        g_active = buf;
        g_active_len = nframes;
        g_active_pos = 0;
        g_fade_counter = 0;
        g_state = ST_FADEIN;
    } else {
        /* le son courant devient la voix "outgoing" : crossfade */
        if (g_outgoing) free(g_outgoing);
        g_outgoing = g_active;
        g_outgoing_len = g_active_len;
        g_outgoing_pos = g_active_pos;

        g_active = buf;
        g_active_len = nframes;
        g_active_pos = 0;
        g_fade_counter = 0;
        g_state = ST_XFADE;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

EXPORT void shim_stop(void) {
    pthread_mutex_lock(&g_lock);
    if (g_state != ST_IDLE) {
        if (g_outgoing) free(g_outgoing);
        g_outgoing = g_active;
        g_outgoing_len = g_active_len;
        g_outgoing_pos = g_active_pos;

        g_active = NULL;
        g_fade_counter = 0;
        g_state = ST_FADEOUT;
    }
    g_paused = 0;
    g_pause_ramp = 1.0f;
    pthread_mutex_unlock(&g_lock);
}

EXPORT void shim_pause(void) {
    pthread_mutex_lock(&g_lock);
    g_paused = 1;
    pthread_mutex_unlock(&g_lock);
}

EXPORT void shim_resume(void) {
    pthread_mutex_lock(&g_lock);
    g_paused = 0;
    pthread_mutex_unlock(&g_lock);
}

EXPORT void shim_set_gain(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    pthread_mutex_lock(&g_lock);
    g_gain = gain;
    pthread_mutex_unlock(&g_lock);
}

/* position de lecture en frames ; -1 si rien n'est actif */
EXPORT int shim_position(void) {
    int pos;
    pthread_mutex_lock(&g_lock);
    pos = g_active ? (int)g_active_pos : -1;
    pthread_mutex_unlock(&g_lock);
    return pos;
}

/* deplace la position de lecture ; renvoie 0 si ok, -1 si rien n'est actif */
EXPORT int shim_seek(int frame) {
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    if (g_active) {
        if (frame < 0) frame = 0;
        if (frame > g_active_len) frame = (int)g_active_len;
        g_active_pos = frame;
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

EXPORT int shim_status(void) {
    return (int)g_state;
}

EXPORT void shim_terminate(void) {
    if (g_stream) {
        Pa_StopStream(g_stream);
        Pa_CloseStream(g_stream);
        g_stream = NULL;
    }
    Pa_Terminate();

    pthread_mutex_lock(&g_lock);
    if (g_active)   { free(g_active);   g_active = NULL; }
    if (g_outgoing) { free(g_outgoing); g_outgoing = NULL; }
    g_state = ST_IDLE;
    g_paused = 0;
    g_pause_ramp = 1.0f;
    g_gain = 1.0f;
    pthread_mutex_unlock(&g_lock);
}
