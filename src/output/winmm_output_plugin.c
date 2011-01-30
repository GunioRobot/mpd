/*
 * Copyright (C) 2003-2011 The Music Player Daemon Project
 * http://www.musicpd.org
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
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "output_api.h"
#include "pcm_buffer.h"
#include "mixer_list.h"
#include "winmm_output_plugin.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "winmm_output"

struct winmm_buffer {
	struct pcm_buffer buffer;

	WAVEHDR hdr;
};

struct winmm_output {
	UINT device_id;
	HWAVEOUT handle;

	/**
	 * This event is triggered by Windows when a buffer is
	 * finished.
	 */
	HANDLE event;

	struct winmm_buffer buffers[8];
	unsigned next_buffer;
};

/**
 * The quark used for GError.domain.
 */
static inline GQuark
winmm_output_quark(void)
{
	return g_quark_from_static_string("winmm_output");
}

HWAVEOUT
winmm_output_get_handle(struct winmm_output* output)
{
	return output->handle;
}

static bool
winmm_output_test_default_device(void)
{
	return waveOutGetNumDevs() > 0;
}

static UINT
get_device_id(const char *device_name)
{
	/* if device is not specified use wave mapper */
	if (device_name == NULL)
		return WAVE_MAPPER;

	/* check for device id */
	char *endptr;
	UINT id = strtoul(device_name, &endptr, 0);
	if (endptr > device_name && *endptr == 0)
		return id;

	/* check for device name */
	for (UINT i = 0; i < waveOutGetNumDevs(); i++) {
		WAVEOUTCAPS caps;
		MMRESULT result = waveOutGetDevCaps(i, &caps, sizeof(caps));
		if (result != MMSYSERR_NOERROR)
			continue;
		/* szPname is only 32 chars long, so it is often truncated.
		   Use partial match to work around this. */
		if (strstr(device_name, caps.szPname) == device_name)
			return i;
	}

	/* fallback to wave mapper */
	return WAVE_MAPPER;
}

static void *
winmm_output_init(G_GNUC_UNUSED const struct audio_format *audio_format,
		  G_GNUC_UNUSED const struct config_param *param,
		  G_GNUC_UNUSED GError **error)
{
	struct winmm_output *wo = g_new(struct winmm_output, 1);
	const char *device = config_get_block_string(param, "device", NULL);
	wo->device_id = get_device_id(device);
	return wo;
}

static void
winmm_output_finish(void *data)
{
	struct winmm_output *wo = data;

	g_free(wo);
}

static bool
winmm_output_open(void *data, struct audio_format *audio_format,
		  GError **error_r)
{
	struct winmm_output *wo = data;

	wo->event = CreateEvent(NULL, false, false, NULL);
	if (wo->event == NULL) {
		g_set_error(error_r, winmm_output_quark(), 0,
			    "CreateEvent() failed");
		return false;
	}

	switch (audio_format->format) {
	case SAMPLE_FORMAT_S8:
	case SAMPLE_FORMAT_S16:
		break;

	case SAMPLE_FORMAT_S24:
	case SAMPLE_FORMAT_S24_P32:
	case SAMPLE_FORMAT_S32:
	case SAMPLE_FORMAT_UNDEFINED:
		/* we havn't tested formats other than S16 */
		audio_format->format = SAMPLE_FORMAT_S16;
		break;
	}

	if (audio_format->channels > 2)
		/* same here: more than stereo was not tested */
		audio_format->channels = 2;

	WAVEFORMATEX format;
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = audio_format->channels;
	format.nSamplesPerSec = audio_format->sample_rate;
	format.nBlockAlign = audio_format_frame_size(audio_format);
	format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
	format.wBitsPerSample = audio_format_sample_size(audio_format) * 8;
	format.cbSize = 0;

	MMRESULT result = waveOutOpen(&wo->handle, wo->device_id, &format,
				      (DWORD_PTR)wo->event, 0, CALLBACK_EVENT);
	if (result != MMSYSERR_NOERROR) {
		CloseHandle(wo->event);
		g_set_error(error_r, winmm_output_quark(), result,
			    "waveOutOpen() failed");
		return false;
	}

	for (unsigned i = 0; i < G_N_ELEMENTS(wo->buffers); ++i) {
		pcm_buffer_init(&wo->buffers[i].buffer);
		memset(&wo->buffers[i].hdr, 0, sizeof(wo->buffers[i].hdr));
	}

	wo->next_buffer = 0;

	return true;
}

static void
winmm_output_close(void *data)
{
	struct winmm_output *wo = data;

	for (unsigned i = 0; i < G_N_ELEMENTS(wo->buffers); ++i)
		pcm_buffer_deinit(&wo->buffers[i].buffer);

	waveOutClose(wo->handle);

	CloseHandle(wo->event);
}

/**
 * Copy data into a buffer, and prepare the wave header.
 */
static bool
winmm_set_buffer(struct winmm_output *wo, struct winmm_buffer *buffer,
		 const void *data, size_t size,
		 GError **error_r)
{
	void *dest = pcm_buffer_get(&buffer->buffer, size);
	if (dest == NULL) {
		g_set_error(error_r, winmm_output_quark(), 0,
			    "Out of memory");
		return false;
	}

	memcpy(dest, data, size);

	memset(&buffer->hdr, 0, sizeof(buffer->hdr));
	buffer->hdr.lpData = dest;
	buffer->hdr.dwBufferLength = size;

	MMRESULT result = waveOutPrepareHeader(wo->handle, &buffer->hdr,
					       sizeof(buffer->hdr));
	if (result != MMSYSERR_NOERROR) {
		g_set_error(error_r, winmm_output_quark(), result,
			    "waveOutPrepareHeader() failed");
		return false;
	}

	return true;
}

/**
 * Wait until the buffer is finished.
 */
static bool
winmm_drain_buffer(struct winmm_output *wo, struct winmm_buffer *buffer,
		   GError **error_r)
{
	if ((buffer->hdr.dwFlags & WHDR_DONE) == WHDR_DONE)
		/* already finished */
		return true;

	while (true) {
		MMRESULT result = waveOutUnprepareHeader(wo->handle,
							 &buffer->hdr,
							 sizeof(buffer->hdr));
		if (result == MMSYSERR_NOERROR)
			return true;
		else if (result != WAVERR_STILLPLAYING) {
			g_set_error(error_r, winmm_output_quark(), result,
				    "waveOutUnprepareHeader() failed");
			return false;
		}

		/* wait some more */
		WaitForSingleObject(wo->event, INFINITE);
	}
}

static size_t
winmm_output_play(void *data, const void *chunk, size_t size, GError **error_r)
{
	struct winmm_output *wo = data;

	/* get the next buffer from the ring and prepare it */
	struct winmm_buffer *buffer = &wo->buffers[wo->next_buffer];
	if (!winmm_drain_buffer(wo, buffer, error_r) ||
	    !winmm_set_buffer(wo, buffer, chunk, size, error_r))
		return 0;

	/* enqueue the buffer */
	MMRESULT result = waveOutWrite(wo->handle, &buffer->hdr,
				       sizeof(buffer->hdr));
	if (result != MMSYSERR_NOERROR) {
		waveOutUnprepareHeader(wo->handle, &buffer->hdr,
				       sizeof(buffer->hdr));
		g_set_error(error_r, winmm_output_quark(), result,
			    "waveOutWrite() failed");
		return 0;
	}

	/* mark our buffer as "used" */
	wo->next_buffer = (wo->next_buffer + 1) %
		G_N_ELEMENTS(wo->buffers);

	return size;
}

static bool
winmm_drain_all_buffers(struct winmm_output *wo, GError **error_r)
{
	for (unsigned i = wo->next_buffer; i < G_N_ELEMENTS(wo->buffers); ++i)
		if (!winmm_drain_buffer(wo, &wo->buffers[i], error_r))
			return false;

	for (unsigned i = 0; i < wo->next_buffer; ++i)
		if (!winmm_drain_buffer(wo, &wo->buffers[i], error_r))
			return false;

	return true;
}

static void
winmm_stop(struct winmm_output *wo)
{
	waveOutReset(wo->handle);

	for (unsigned i = 0; i < G_N_ELEMENTS(wo->buffers); ++i) {
		struct winmm_buffer *buffer = &wo->buffers[i];
		waveOutUnprepareHeader(wo->handle, &buffer->hdr,
				       sizeof(buffer->hdr));
	}
}

static void
winmm_output_drain(void *data)
{
	struct winmm_output *wo = data;

	if (!winmm_drain_all_buffers(wo, NULL))
		winmm_stop(wo);
}

static void
winmm_output_cancel(void *data)
{
	struct winmm_output *wo = data;

	winmm_stop(wo);
}

const struct audio_output_plugin winmm_output_plugin = {
	.name = "winmm",
	.test_default_device = winmm_output_test_default_device,
	.init = winmm_output_init,
	.finish = winmm_output_finish,
	.open = winmm_output_open,
	.close = winmm_output_close,
	.play = winmm_output_play,
	.drain = winmm_output_drain,
	.cancel = winmm_output_cancel,
	.mixer_plugin = &winmm_mixer_plugin,
};
