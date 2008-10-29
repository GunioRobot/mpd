/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "../output_api.h"

#include <glib.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#define MPD_PULSE_NAME "mpd"
#define CONN_ATTEMPT_INTERVAL 60

struct pulse_data {
	struct audio_output *ao;

	pa_simple *s;
	char *server;
	char *sink;
	int num_connect_attempts;
	time_t last_connect_attempt;
};

static struct pulse_data *pulse_new_data(void)
{
	struct pulse_data *ret;

	ret = g_new(struct pulse_data, 1);

	ret->s = NULL;
	ret->server = NULL;
	ret->sink = NULL;
	ret->num_connect_attempts = 0;
	ret->last_connect_attempt = 0;

	return ret;
}

static void pulse_free_data(struct pulse_data *pd)
{
	g_free(pd->server);
	g_free(pd->sink);
	g_free(pd);
}

static void *
pulse_init(struct audio_output *ao,
	   mpd_unused const struct audio_format *audio_format,
	   ConfigParam *param)
{
	BlockParam *server = NULL;
	BlockParam *sink = NULL;
	struct pulse_data *pd;

	if (param) {
		server = getBlockParam(param, "server");
		sink = getBlockParam(param, "sink");
	}

	pd = pulse_new_data();
	pd->ao = ao;
	pd->server = g_strdup(server->value);
	pd->sink = g_strdup(sink->value);

	return pd;
}

static void pulse_finish(void *data)
{
	struct pulse_data *pd = data;

	pulse_free_data(pd);
}

static bool pulse_test_default_device(void)
{
	pa_simple *s;
	pa_sample_spec ss;
	int error;

	ss.format = PA_SAMPLE_S16NE;
	ss.rate = 44100;
	ss.channels = 2;

	s = pa_simple_new(NULL, MPD_PULSE_NAME, PA_STREAM_PLAYBACK, NULL,
			  MPD_PULSE_NAME, &ss, NULL, NULL, &error);
	if (!s) {
		g_message("Cannot connect to default PulseAudio server: %s\n",
			  pa_strerror(error));
		return false;
	}

	pa_simple_free(s);

	return true;
}

static bool
pulse_open(void *data, struct audio_format *audio_format)
{
	struct pulse_data *pd = data;
	pa_sample_spec ss;
	time_t t;
	int error;

	t = time(NULL);

	if (pd->num_connect_attempts != 0 &&
	    (t - pd->last_connect_attempt) < CONN_ATTEMPT_INTERVAL)
		return false;

	pd->num_connect_attempts++;
	pd->last_connect_attempt = t;

	/* MPD doesn't support the other pulseaudio sample formats, so
	   we just force MPD to send us everything as 16 bit */
	audio_format->bits = 16;

	ss.format = PA_SAMPLE_S16NE;
	ss.rate = audio_format->sample_rate;
	ss.channels = audio_format->channels;

	pd->s = pa_simple_new(pd->server, MPD_PULSE_NAME, PA_STREAM_PLAYBACK,
			      pd->sink, audio_output_get_name(pd->ao),
			      &ss, NULL, NULL,
			      &error);
	if (!pd->s) {
		g_warning("Cannot connect to server in PulseAudio output "
			  "\"%s\" (attempt %i): %s\n",
			  audio_output_get_name(pd->ao),
			  pd->num_connect_attempts, pa_strerror(error));
		return false;
	}

	pd->num_connect_attempts = 0;

	g_debug("PulseAudio output \"%s\" connected and playing %i bit, %i "
		"channel audio at %i Hz\n",
		audio_output_get_name(pd->ao),
		audio_format->bits,
		audio_format->channels, audio_format->sample_rate);

	return true;
}

static void pulse_cancel(void *data)
{
	struct pulse_data *pd = data;
	int error;

	if (pa_simple_flush(pd->s, &error) < 0)
		g_warning("Flush failed in PulseAudio output \"%s\": %s\n",
			  audio_output_get_name(pd->ao),
			  pa_strerror(error));
}

static void pulse_close(void *data)
{
	struct pulse_data *pd = data;

	if (pd->s) {
		pa_simple_drain(pd->s, NULL);
		pa_simple_free(pd->s);
		pd->s = NULL;
	}
}

static bool
pulse_play(void *data, const char *playChunk, size_t size)
{
	struct pulse_data *pd = data;
	int error;

	if (pa_simple_write(pd->s, playChunk, size, &error) < 0) {
		g_warning("PulseAudio output \"%s\" disconnecting due to "
			  "write error: %s\n",
			  audio_output_get_name(pd->ao),
			  pa_strerror(error));
		pulse_close(pd);
		return false;
	}

	return true;
}

const struct audio_output_plugin pulse_plugin = {
	.name = "pulse",
	.test_default_device = pulse_test_default_device,
	.init = pulse_init,
	.finish = pulse_finish,
	.open = pulse_open,
	.play = pulse_play,
	.cancel = pulse_cancel,
	.close = pulse_close,
};