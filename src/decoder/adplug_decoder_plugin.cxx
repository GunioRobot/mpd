#include "config.h"

extern "C" {
#include "../decoder_api.h"
#include "audio_check.h"
}

#include <glib.h>
//#include <assert.h>
#include <adplug/adplug.h>
#include <adplug/emuopl.h>
#include <adplug/kemuopl.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "adplug"

#ifndef min
#  define min(a,b) (((a) < (b)) ? (a) : (b))
#endif


enum {
	ADPLUG_SAMPLE_RATE = 44100,
	ADPLUG_CHANNELS = 1,
    ADPLUG_BUFFER_FRAMES = 256,
    ADPLUG_BUFFER_SAMPLES = ADPLUG_BUFFER_FRAMES * ADPLUG_CHANNELS,
};

inline unsigned char getsampsize() { return (ADPLUG_CHANNELS * (16 / 8)); }

static void
adplug_file_decode(struct decoder *decoder, const char *path_fs)
{
    /* don't know the difference between these two emulators,
     * should it be made configurable?
     */
    Copl *opl = new CEmuopl(44100, true, false);
    //Copl *opl = new CKemuopl(44100, true, true);

    /* TODO the mpd log gets cluttered up with a bunch of messages from this
     * constructor! */
    CPlayer *p = CAdPlug::factory(path_fs, opl);
	short buf[ADPLUG_BUFFER_SAMPLES];
    if(!p){
        g_warning("Unknown filetype\n");
        return;
    }
    /* no support for subsongs yet */
    p->rewind(0);

	/* initialize the MPD decoder */
	struct audio_format audio_format;
	enum decoder_command cmd;

	GError *error = NULL;
	if (!audio_format_init_checked(&audio_format, ADPLUG_SAMPLE_RATE,
				       SAMPLE_FORMAT_S16, 2,
				       &error)) {
		g_warning("%s", error->message);
		g_error_free(error);
		return;
	}

	decoder_initialized(decoder, &audio_format, true, NULL);
    /* play */
    do {
        static long	minicnt = 0;
        long		i, towrite = ADPLUG_BUFFER_SAMPLES / getsampsize();
        short		*pos = buf;

        // Prepare audiobuf with emulator output
        while(towrite > 0) {
            while(minicnt < 0) {
                  minicnt += ADPLUG_SAMPLE_RATE;
                  //self->playing = self->p->update();
                  p->update();
            }
            i = min(towrite, (long)(minicnt / p->getrefresh() + 4) & ~3); //the fuck??!
            g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "li: %i", i);
            opl->update((short *)pos, i);
            pos += i * getsampsize();
            towrite -= i;
            minicnt -= (long)(p->getrefresh() * i);
        }
        cmd = decoder_data(decoder, NULL, buf, sizeof(buf), 0);
    } while(cmd != DECODE_COMMAND_STOP);

}

static struct tag *
adplug_tag_dup(const char *path_fs)
{
    /* don't know the difference between these two emulators,
     * should it be made configurable?
     */
    Copl *opl = new CEmuopl(44100, true, true);
    //Copl *opl = new CKemuopl(44100, true, true);
    CPlayer *p = CAdPlug::factory(path_fs, opl);
    if(!p){
        g_warning("Unknown filetype\n");
        return NULL;
    }
	struct tag *tag = tag_new();
    tag_add_item(tag, TAG_ARTIST, p->getauthor().c_str());
    tag_add_item(tag, TAG_TITLE, p->gettitle().c_str());
    return tag;
}

static const char *const adplug_suffixes[] = {
    "a2m", "adl", "adlib", "amd", "bam", "cff", "cmf", "d00", "dtm", "dfm",
    "dmo", "dro", "hsc", "hsp", "imf", "laa", "lds", "m", "mad", "mid", "mkj",
    "msc", "mtk", "rad", "raw", "rix", "rol", "s3m", "sa2", "sat", "sci", "sng",
    "wlf", "xad", "xsm",
    NULL
};

extern const struct decoder_plugin adplug_decoder_plugin;
const struct decoder_plugin adplug_decoder_plugin = {
    "adplug",
    NULL, /* init() */
    NULL, /* finish() */
    NULL, /* stream_decode() */
    adplug_file_decode,
    adplug_tag_dup, /* tag_dup() */
    NULL, /* stream_tag() */
    NULL, /* container_scan() */
    adplug_suffixes,
    NULL, /* mime_types */
};
