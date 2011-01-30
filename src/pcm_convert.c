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
#include "pcm_convert.h"
#include "pcm_channels.h"
#include "pcm_format.h"
#include "pcm_byteswap.h"
#include "pcm_pack.h"
#include "audio_format.h"

#include <assert.h>
#include <string.h>
#include <math.h>
#include <glib.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "pcm"

void pcm_convert_init(struct pcm_convert_state *state)
{
	memset(state, 0, sizeof(*state));

	pcm_resample_init(&state->resample);
	pcm_dither_24_init(&state->dither);

	pcm_buffer_init(&state->format_buffer);
	pcm_buffer_init(&state->pack_buffer);
	pcm_buffer_init(&state->channels_buffer);
	pcm_buffer_init(&state->byteswap_buffer);
}

void pcm_convert_deinit(struct pcm_convert_state *state)
{
	pcm_resample_deinit(&state->resample);

	pcm_buffer_deinit(&state->format_buffer);
	pcm_buffer_deinit(&state->pack_buffer);
	pcm_buffer_deinit(&state->channels_buffer);
	pcm_buffer_deinit(&state->byteswap_buffer);
}

static const int16_t *
pcm_convert_16(struct pcm_convert_state *state,
	       const struct audio_format *src_format,
	       const void *src_buffer, size_t src_size,
	       const struct audio_format *dest_format, size_t *dest_size_r,
	       GError **error_r)
{
	const int16_t *buf;
	size_t len;

	assert(dest_format->format == SAMPLE_FORMAT_S16);

	buf = pcm_convert_to_16(&state->format_buffer, &state->dither,
				src_format->format, src_buffer, src_size,
				&len);
	if (buf == NULL) {
		g_set_error(error_r, pcm_convert_quark(), 0,
			    "Conversion from %s to 16 bit is not implemented",
			    sample_format_to_string(src_format->format));
		return NULL;
	}

	if (src_format->channels != dest_format->channels) {
		buf = pcm_convert_channels_16(&state->channels_buffer,
					      dest_format->channels,
					      src_format->channels,
					      buf, len, &len);
		if (buf == NULL) {
			g_set_error(error_r, pcm_convert_quark(), 0,
				    "Conversion from %u to %u channels "
				    "is not implemented",
				    src_format->channels,
				    dest_format->channels);
			return NULL;
		}
	}

	if (src_format->sample_rate != dest_format->sample_rate) {
		buf = pcm_resample_16(&state->resample,
				      dest_format->channels,
				      src_format->sample_rate, buf, len,
				      dest_format->sample_rate, &len,
				      error_r);
		if (buf == NULL)
			return NULL;
	}

	if (dest_format->reverse_endian) {
		buf = pcm_byteswap_16(&state->byteswap_buffer, buf, len);
		assert(buf != NULL);
	}

	*dest_size_r = len;
	return buf;
}

static const int32_t *
pcm_convert_24(struct pcm_convert_state *state,
	       const struct audio_format *src_format,
	       const void *src_buffer, size_t src_size,
	       const struct audio_format *dest_format, size_t *dest_size_r,
	       GError **error_r)
{
	const int32_t *buf;
	size_t len;

	assert(dest_format->format == SAMPLE_FORMAT_S24_P32);

	buf = pcm_convert_to_24(&state->format_buffer, src_format->format,
				src_buffer, src_size, &len);
	if (buf == NULL) {
		g_set_error(error_r, pcm_convert_quark(), 0,
			    "Conversion from %s to 24 bit is not implemented",
			    sample_format_to_string(src_format->format));
		return NULL;
	}

	if (src_format->channels != dest_format->channels) {
		buf = pcm_convert_channels_24(&state->channels_buffer,
					      dest_format->channels,
					      src_format->channels,
					      buf, len, &len);
		if (buf == NULL) {
			g_set_error(error_r, pcm_convert_quark(), 0,
				    "Conversion from %u to %u channels "
				    "is not implemented",
				    src_format->channels,
				    dest_format->channels);
			return NULL;
		}
	}

	if (src_format->sample_rate != dest_format->sample_rate) {
		buf = pcm_resample_24(&state->resample,
				      dest_format->channels,
				      src_format->sample_rate, buf, len,
				      dest_format->sample_rate, &len,
				      error_r);
		if (buf == NULL)
			return NULL;
	}

	if (dest_format->reverse_endian) {
		buf = pcm_byteswap_32(&state->byteswap_buffer, buf, len);
		assert(buf != NULL);
	}

	*dest_size_r = len;
	return buf;
}

/**
 * Convert to 24 bit packed samples (aka S24_3LE / S24_3BE).
 */
static const void *
pcm_convert_24_packed(struct pcm_convert_state *state,
		      const struct audio_format *src_format,
		      const void *src_buffer, size_t src_size,
		      const struct audio_format *dest_format,
		      size_t *dest_size_r,
		      GError **error_r)
{
	assert(dest_format->format == SAMPLE_FORMAT_S24);

	/* use the normal 24 bit conversion first */

	struct audio_format audio_format;
	audio_format_init(&audio_format, dest_format->sample_rate,
			  SAMPLE_FORMAT_S24_P32, dest_format->channels);

	const int32_t *buffer;
	size_t buffer_size;

	buffer = pcm_convert_24(state, src_format, src_buffer, src_size,
				&audio_format, &buffer_size, error_r);
	if (buffer == NULL)
		return NULL;

	/* now convert to packed 24 bit */

	unsigned num_samples = buffer_size / 4;
	size_t dest_size = num_samples * 3;

	uint8_t *dest = pcm_buffer_get(&state->pack_buffer, dest_size);
	pcm_pack_24(dest, buffer, num_samples, dest_format->reverse_endian);

	*dest_size_r = dest_size;
	return dest;
}

static const int32_t *
pcm_convert_32(struct pcm_convert_state *state,
	       const struct audio_format *src_format,
	       const void *src_buffer, size_t src_size,
	       const struct audio_format *dest_format, size_t *dest_size_r,
	       GError **error_r)
{
	const int32_t *buf;
	size_t len;

	assert(dest_format->format == SAMPLE_FORMAT_S32);

	buf = pcm_convert_to_32(&state->format_buffer, src_format->format,
				src_buffer, src_size, &len);
	if (buf == NULL) {
		g_set_error(error_r, pcm_convert_quark(), 0,
			    "Conversion from %s to 24 bit is not implemented",
			    sample_format_to_string(src_format->format));
		return NULL;
	}

	if (src_format->channels != dest_format->channels) {
		buf = pcm_convert_channels_32(&state->channels_buffer,
					      dest_format->channels,
					      src_format->channels,
					      buf, len, &len);
		if (buf == NULL) {
			g_set_error(error_r, pcm_convert_quark(), 0,
				    "Conversion from %u to %u channels "
				    "is not implemented",
				    src_format->channels,
				    dest_format->channels);
			return NULL;
		}
	}

	if (src_format->sample_rate != dest_format->sample_rate) {
		buf = pcm_resample_32(&state->resample,
				      dest_format->channels,
				      src_format->sample_rate, buf, len,
				      dest_format->sample_rate, &len,
				      error_r);
		if (buf == NULL)
			return buf;
	}

	if (dest_format->reverse_endian) {
		buf = pcm_byteswap_32(&state->byteswap_buffer, buf, len);
		assert(buf != NULL);
	}

	*dest_size_r = len;
	return buf;
}

const void *
pcm_convert(struct pcm_convert_state *state,
	    const struct audio_format *src_format,
	    const void *src, size_t src_size,
	    const struct audio_format *dest_format,
	    size_t *dest_size_r,
	    GError **error_r)
{
	switch (dest_format->format) {
	case SAMPLE_FORMAT_S16:
		return pcm_convert_16(state,
				      src_format, src, src_size,
				      dest_format, dest_size_r,
				      error_r);

	case SAMPLE_FORMAT_S24:
		return pcm_convert_24_packed(state,
					     src_format, src, src_size,
					     dest_format, dest_size_r,
					     error_r);

	case SAMPLE_FORMAT_S24_P32:
		return pcm_convert_24(state,
				      src_format, src, src_size,
				      dest_format, dest_size_r,
				      error_r);

	case SAMPLE_FORMAT_S32:
		return pcm_convert_32(state,
				      src_format, src, src_size,
				      dest_format, dest_size_r,
				      error_r);

	default:
		g_set_error(error_r, pcm_convert_quark(), 0,
			    "PCM conversion to %s is not implemented",
			    sample_format_to_string(dest_format->format));
		return NULL;
	}
}
