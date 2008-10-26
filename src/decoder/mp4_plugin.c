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

#include "../decoder_api.h"
#include "../utils.h"
#include "../log.h"

#include "mp4ff.h"

#include <limits.h>
#include <faad.h>
/* all code here is either based on or copied from FAAD2's frontend code */

static int mp4_getAACTrack(mp4ff_t * infile)
{
	/* find AAC track */
	int i, rc;
	int numTracks = mp4ff_total_tracks(infile);

	for (i = 0; i < numTracks; i++) {
		unsigned char *buff = NULL;
		unsigned int buff_size = 0;
#ifdef HAVE_MP4AUDIOSPECIFICCONFIG
		mp4AudioSpecificConfig mp4ASC;
#else
		unsigned long dummy1_32;
		unsigned char dummy2_8, dummy3_8, dummy4_8, dummy5_8, dummy6_8,
		    dummy7_8, dummy8_8;
#endif

		mp4ff_get_decoder_config(infile, i, &buff, &buff_size);

		if (buff) {
#ifdef HAVE_MP4AUDIOSPECIFICCONFIG
			rc = AudioSpecificConfig(buff, buff_size, &mp4ASC);
#else
			rc = AudioSpecificConfig(buff, &dummy1_32, &dummy2_8,
						 &dummy3_8, &dummy4_8,
						 &dummy5_8, &dummy6_8,
						 &dummy7_8, &dummy8_8);
#endif
			free(buff);
			if (rc < 0)
				continue;
			return i;
		}
	}

	/* can't decode this */
	return -1;
}

static uint32_t mp4_inputStreamReadCallback(void *inStream, void *buffer,
					    uint32_t length)
{
	return readFromInputStream((InputStream *) inStream, buffer, length);
}

static uint32_t mp4_inputStreamSeekCallback(void *inStream, uint64_t position)
{
	return seekInputStream((InputStream *) inStream, position, SEEK_SET);
}

static int mp4_decode(struct decoder * mpd_decoder, InputStream * inStream)
{
	mp4ff_t *mp4fh;
	mp4ff_callback_t *mp4cb;
	int32_t track;
	float file_time, total_time;
	int32_t scale;
	faacDecHandle decoder;
	faacDecFrameInfo frameInfo;
	faacDecConfigurationPtr config;
	struct audio_format audio_format;
	unsigned char *mp4Buffer;
	unsigned int mp4BufferSize;
	uint32_t sample_rate;
	unsigned char channels;
	long sampleId;
	long numSamples;
	long dur;
	unsigned int sampleCount;
	char *sampleBuffer;
	size_t sampleBufferLen;
	unsigned int initial = 1;
	float *seekTable;
	long seekTableEnd = -1;
	bool seekPositionFound = false;
	long offset;
	uint16_t bitRate = 0;
	bool seeking = false;
	double seek_where = 0;
	bool initialized = false;

	mp4cb = xmalloc(sizeof(mp4ff_callback_t));
	mp4cb->read = mp4_inputStreamReadCallback;
	mp4cb->seek = mp4_inputStreamSeekCallback;
	mp4cb->user_data = inStream;

	mp4fh = mp4ff_open_read(mp4cb);
	if (!mp4fh) {
		ERROR("Input does not appear to be a mp4 stream.\n");
		free(mp4cb);
		return -1;
	}

	track = mp4_getAACTrack(mp4fh);
	if (track < 0) {
		ERROR("No AAC track found in mp4 stream.\n");
		mp4ff_close(mp4fh);
		free(mp4cb);
		return -1;
	}

	decoder = faacDecOpen();

	config = faacDecGetCurrentConfiguration(decoder);
	config->outputFormat = FAAD_FMT_16BIT;
#ifdef HAVE_FAACDECCONFIGURATION_DOWNMATRIX
	config->downMatrix = 1;
#endif
#ifdef HAVE_FAACDECCONFIGURATION_DONTUPSAMPLEIMPLICITSBR
	config->dontUpSampleImplicitSBR = 0;
#endif
	faacDecSetConfiguration(decoder, config);

	audio_format.bits = 16;

	mp4Buffer = NULL;
	mp4BufferSize = 0;
	mp4ff_get_decoder_config(mp4fh, track, &mp4Buffer, &mp4BufferSize);

	if (faacDecInit2
	    (decoder, mp4Buffer, mp4BufferSize, &sample_rate, &channels) < 0) {
		ERROR("Error not a AAC stream.\n");
		faacDecClose(decoder);
		mp4ff_close(mp4fh);
		free(mp4cb);
		return -1;
	}

	audio_format.sample_rate = sample_rate;
	audio_format.channels = channels;
	file_time = mp4ff_get_track_duration_use_offsets(mp4fh, track);
	scale = mp4ff_time_scale(mp4fh, track);

	if (mp4Buffer)
		free(mp4Buffer);

	if (scale < 0) {
		ERROR("Error getting audio format of mp4 AAC track.\n");
		faacDecClose(decoder);
		mp4ff_close(mp4fh);
		free(mp4cb);
		return -1;
	}
	total_time = ((float)file_time) / scale;

	numSamples = mp4ff_num_samples(mp4fh, track);
	if (numSamples > (long)(INT_MAX / sizeof(float))) {
		 ERROR("Integer overflow.\n");
		 faacDecClose(decoder);
		 mp4ff_close(mp4fh);
		 free(mp4cb);
		 return -1;
	}

	file_time = 0.0;

	seekTable = xmalloc(sizeof(float) * numSamples);

	for (sampleId = 0; sampleId < numSamples; sampleId++) {
		if (decoder_get_command(mpd_decoder) == DECODE_COMMAND_SEEK) {
			seeking = true;
			seek_where = decoder_seek_where(mpd_decoder);
		}

		if (seeking && seekTableEnd > 1 &&
		    seekTable[seekTableEnd] >= seek_where) {
			int i = 2;
			while (seekTable[i] < seek_where)
				i++;
			sampleId = i - 1;
			file_time = seekTable[sampleId];
		}

		dur = mp4ff_get_sample_duration(mp4fh, track, sampleId);
		offset = mp4ff_get_sample_offset(mp4fh, track, sampleId);

		if (sampleId > seekTableEnd) {
			seekTable[sampleId] = file_time;
			seekTableEnd = sampleId;
		}

		if (sampleId == 0)
			dur = 0;
		if (offset > dur)
			dur = 0;
		else
			dur -= offset;
		file_time += ((float)dur) / scale;

		if (seeking && file_time > seek_where)
			seekPositionFound = true;

		if (seeking && seekPositionFound) {
			seekPositionFound = false;
			decoder_clear(mpd_decoder);
			seeking = 0;
			decoder_command_finished(mpd_decoder);
		}

		if (seeking)
			continue;

		if (mp4ff_read_sample(mp4fh, track, sampleId, &mp4Buffer,
				      &mp4BufferSize) == 0)
			break;

#ifdef HAVE_FAAD_BUFLEN_FUNCS
		sampleBuffer = faacDecDecode(decoder, &frameInfo, mp4Buffer,
					     mp4BufferSize);
#else
		sampleBuffer = faacDecDecode(decoder, &frameInfo, mp4Buffer);
#endif

		if (mp4Buffer)
			free(mp4Buffer);
		if (frameInfo.error > 0) {
			ERROR("faad2 error: %s\n",
			      faacDecGetErrorMessage(frameInfo.error));
			break;
		}

		if (!initialized) {
			channels = frameInfo.channels;
#ifdef HAVE_FAACDECFRAMEINFO_SAMPLERATE
			scale = frameInfo.samplerate;
#endif
			audio_format.sample_rate = scale;
			audio_format.channels = frameInfo.channels;
			decoder_initialized(mpd_decoder, &audio_format,
					    total_time);
			initialized = true;
		}

		if (channels * (unsigned long)(dur + offset) > frameInfo.samples) {
			dur = frameInfo.samples / channels;
			offset = 0;
		}

		sampleCount = (unsigned long)(dur * channels);

		if (sampleCount > 0) {
			initial = 0;
			bitRate = frameInfo.bytesconsumed * 8.0 *
			    frameInfo.channels * scale /
			    frameInfo.samples / 1000 + 0.5;
		}

		sampleBufferLen = sampleCount * 2;

		sampleBuffer += offset * channels * 2;

		decoder_data(mpd_decoder, inStream, 1, sampleBuffer,
			     sampleBufferLen, file_time,
			     bitRate, NULL);
		if (decoder_get_command(mpd_decoder) == DECODE_COMMAND_STOP)
			break;
	}

	free(seekTable);
	faacDecClose(decoder);
	mp4ff_close(mp4fh);
	free(mp4cb);

	if (!initialized)
		return -1;

	if (decoder_get_command(mpd_decoder) == DECODE_COMMAND_SEEK && seeking) {
		decoder_clear(mpd_decoder);
		decoder_command_finished(mpd_decoder);
	}
	decoder_flush(mpd_decoder);

	return 0;
}

static struct tag *mp4DataDup(char *file, int *mp4MetadataFound)
{
	struct tag *ret = NULL;
	InputStream inStream;
	mp4ff_t *mp4fh;
	mp4ff_callback_t *callback;
	int32_t track;
	int32_t file_time;
	int32_t scale;
	int i;

	*mp4MetadataFound = 0;

	if (openInputStream(&inStream, file) < 0) {
		DEBUG("mp4DataDup: Failed to open file: %s\n", file);
		return NULL;
	}

	callback = xmalloc(sizeof(mp4ff_callback_t));
	callback->read = mp4_inputStreamReadCallback;
	callback->seek = mp4_inputStreamSeekCallback;
	callback->user_data = &inStream;

	mp4fh = mp4ff_open_read(callback);
	if (!mp4fh) {
		free(callback);
		closeInputStream(&inStream);
		return NULL;
	}

	track = mp4_getAACTrack(mp4fh);
	if (track < 0) {
		mp4ff_close(mp4fh);
		closeInputStream(&inStream);
		free(callback);
		return NULL;
	}

	ret = tag_new();
	file_time = mp4ff_get_track_duration_use_offsets(mp4fh, track);
	scale = mp4ff_time_scale(mp4fh, track);
	if (scale < 0) {
		mp4ff_close(mp4fh);
		closeInputStream(&inStream);
		free(callback);
		tag_free(ret);
		return NULL;
	}
	ret->time = ((float)file_time) / scale + 0.5;

	for (i = 0; i < mp4ff_meta_get_num_items(mp4fh); i++) {
		char *item;
		char *value;

		mp4ff_meta_get_by_index(mp4fh, i, &item, &value);

		if (0 == strcasecmp("artist", item)) {
			tag_add_item(ret, TAG_ITEM_ARTIST, value);
			*mp4MetadataFound = 1;
		} else if (0 == strcasecmp("title", item)) {
			tag_add_item(ret, TAG_ITEM_TITLE, value);
			*mp4MetadataFound = 1;
		} else if (0 == strcasecmp("album", item)) {
			tag_add_item(ret, TAG_ITEM_ALBUM, value);
			*mp4MetadataFound = 1;
		} else if (0 == strcasecmp("track", item)) {
			tag_add_item(ret, TAG_ITEM_TRACK, value);
			*mp4MetadataFound = 1;
		} else if (0 == strcasecmp("disc", item)) {	/* Is that the correct id? */
			tag_add_item(ret, TAG_ITEM_DISC, value);
			*mp4MetadataFound = 1;
		} else if (0 == strcasecmp("genre", item)) {
			tag_add_item(ret, TAG_ITEM_GENRE, value);
			*mp4MetadataFound = 1;
		} else if (0 == strcasecmp("date", item)) {
			tag_add_item(ret, TAG_ITEM_DATE, value);
			*mp4MetadataFound = 1;
		}

		free(item);
		free(value);
	}

	mp4ff_close(mp4fh);
	closeInputStream(&inStream);

	return ret;
}

static struct tag *mp4TagDup(char *file)
{
	struct tag *ret = NULL;
	int mp4MetadataFound = 0;

	ret = mp4DataDup(file, &mp4MetadataFound);
	if (!ret)
		return NULL;
	if (!mp4MetadataFound) {
		struct tag *temp = tag_id3_load(file);
		if (temp) {
			temp->time = ret->time;
			tag_free(ret);
			ret = temp;
		}
	}

	return ret;
}

static const char *mp4_suffixes[] = { "m4a", "mp4", NULL };
static const char *mp4_mimeTypes[] = { "audio/mp4", "audio/m4a", NULL };

struct decoder_plugin mp4Plugin = {
	.name = "mp4",
	.stream_decode = mp4_decode,
	.tag_dup = mp4TagDup,
	.stream_types = INPUT_PLUGIN_STREAM_FILE | INPUT_PLUGIN_STREAM_URL,
	.suffixes = mp4_suffixes,
	.mime_types = mp4_mimeTypes,
};