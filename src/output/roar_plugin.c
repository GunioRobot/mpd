/*
 * Copyright (C) 2003-2010 The Music Player Daemon Project
 * Copyright (C) 2010-2011 Philipp 'ph3-der-loewe' Schafft
 * Copyright (C) 2010-2011 Hans-Kristian 'maister' Arntzen
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
#include "mixer_list.h"
#include "roar_output_plugin.h"

#include <glib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>


#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "roaraudio"

static inline GQuark
roar_output_quark(void)
{
	return g_quark_from_static_string("roar_output");
}

static void
roar_configure(struct roar * self, const struct config_param *param)
{
	self->host = config_dup_block_string(param, "server", NULL);
	self->name = config_dup_block_string(param, "name", "MPD");
	char *role = config_dup_block_string(param, "role", "music");
	if (role != NULL)
	{
		self->role = roar_str2role(role);
		g_free(role);
	}
	else
		self->role = ROAR_ROLE_MUSIC;
}

static void *
roar_init(G_GNUC_UNUSED const struct audio_format *audio_format,
		const struct config_param *param,
		G_GNUC_UNUSED GError **error)
{
	GMutex *lock = g_mutex_new();

	roar_t * self = roar_mm_calloc(1, sizeof(*self));
	if (self == NULL)
	{
		g_set_error(error, roar_output_quark(), 0, "Failed to allocate memory");
		return NULL;
	}

	self->lock = lock;
	self->err = ROAR_ERROR_NONE;
	roar_configure(self, param);
	return self;
}

static void
roar_close(void *data)
{
	roar_t * self = data;
	g_mutex_lock(self->lock);
	self->alive = false;

	if (self->vss != NULL)
		roar_vs_close(self->vss, ROAR_VS_TRUE, &(self->err));
	self->vss = NULL;
	roar_disconnect(&(self->con));
	g_mutex_unlock(self->lock);
}

static void
roar_finish(void *data)
{
	roar_t * self = data;

	g_free(self->host);
	g_free(self->name);
	g_mutex_free(self->lock);

	roar_mm_free(data);
}

static bool
roar_open(void *data, struct audio_format *audio_format, GError **error)
{
	roar_t * self = data;
	g_mutex_lock(self->lock);

	if (roar_simple_connect(&(self->con), self->host, self->name) < 0)
	{
		g_set_error(error, roar_output_quark(), 0,
				"Failed to connect to Roar server");
		g_mutex_unlock(self->lock);
		return false;
	}

	self->vss = roar_vs_new_from_con(&(self->con), &(self->err));

	if (self->vss == NULL || self->err != ROAR_ERROR_NONE)
	{
		g_set_error(error, roar_output_quark(), 0,
				"Failed to connect to server");
		g_mutex_unlock(self->lock);
		return false;
	}

	self->info.rate = audio_format->sample_rate;
	self->info.channels = audio_format->channels;
	self->info.codec = ROAR_CODEC_PCM_S;

	switch (audio_format->format)
	{
		case SAMPLE_FORMAT_S8:
			self->info.bits = 8;
			break;
		case SAMPLE_FORMAT_S16:
			self->info.bits = 16;
			break;
		case SAMPLE_FORMAT_S24:
			self->info.bits = 24;
			break;
		case SAMPLE_FORMAT_S24_P32:
			self->info.bits = 32;
			audio_format->format = SAMPLE_FORMAT_S32;
			break;
		case SAMPLE_FORMAT_S32:
			self->info.bits = 32;
			break;
		default:
			self->info.bits = 16;
			audio_format->format = SAMPLE_FORMAT_S16;
	}
	audio_format->reverse_endian = 0;

	if (roar_vs_stream(self->vss, &(self->info), ROAR_DIR_PLAY,
				&(self->err)) < 0)
	{
		g_set_error(error, roar_output_quark(), 0, "Failed to start stream");
		g_mutex_unlock(self->lock);
		return false;
	}
	roar_vs_role(self->vss, self->role, &(self->err));
	self->alive = true;

	g_mutex_unlock(self->lock);
	return true;
}

static void
roar_cancel(void *data)
{
	roar_t * self = data;

	g_mutex_lock(self->lock);
	if (self->vss != NULL)
	{
		roar_vs_t *vss = self->vss;
		self->vss = NULL;
		roar_vs_close(vss, ROAR_VS_TRUE, &(self->err));
		self->alive = false;

		vss = roar_vs_new_from_con(&(self->con), &(self->err));
		if (vss)
		{
			roar_vs_stream(vss, &(self->info), ROAR_DIR_PLAY, &(self->err));
			roar_vs_role(vss, self->role, &(self->err));
			self->vss = vss;
			self->alive = true;
		}
	}
	g_mutex_unlock(self->lock);
}

static size_t
roar_play(void *data, const void *chunk, size_t size, GError **error)
{
	struct roar * self = data;
	ssize_t rc;

	if (self->vss == NULL)
	{
		g_set_error(error, roar_output_quark(), 0, "Connection is invalid");
		return 0;
	}

	rc = roar_vs_write(self->vss, chunk, size, &(self->err));
	if ( rc <= 0 )
	{
		g_set_error(error, roar_output_quark(), 0, "Failed to play data");
		return 0;
	}

	return rc;
}

static const char*
roar_tag_convert(enum tag_type type, bool *is_uuid)
{
	*is_uuid = false;
	switch (type)
	{
		case TAG_ARTIST:
		case TAG_ALBUM_ARTIST:
			return "AUTHOR";
		case TAG_ALBUM:
			return "ALBUM";
		case TAG_TITLE:
			return "TITLE";
		case TAG_TRACK:
			return "TRACK";
		case TAG_NAME:
			return "NAME";
		case TAG_GENRE:
			return "GENRE";
		case TAG_DATE:
			return "DATE";
		case TAG_PERFORMER:
			return "PERFORMER";
		case TAG_COMMENT:
			return "COMMENT";
		case TAG_DISC:
			return "DISCID";
		case TAG_COMPOSER:
#ifdef ROAR_META_TYPE_COMPOSER
			return "COMPOSER";
#else
			return "AUTHOR";
#endif
		case TAG_MUSICBRAINZ_ARTISTID:
		case TAG_MUSICBRAINZ_ALBUMID:
		case TAG_MUSICBRAINZ_ALBUMARTISTID:
		case TAG_MUSICBRAINZ_TRACKID:
			*is_uuid = true;
			return "HASH";

		default:
			return NULL;
	}
}

static void
roar_send_tag(void *data, const struct tag *meta)
{
	struct roar * self = data;

	if (self->vss == NULL)
		return;

	g_mutex_lock(self->lock);
	size_t cnt = 1;
	struct roar_keyval vals[32];
	memset(vals, 0, sizeof(vals));
	char uuid_buf[32][64];

	char timebuf[16];
	snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d",
			meta->time / 3600, (meta->time % 3600) / 60, meta->time % 60);

	vals[0].key = g_strdup("LENGTH");
	vals[0].value = timebuf;

	for (unsigned i = 0; i < meta->num_items && cnt < 32; i++)
	{
		bool is_uuid = false;
		const char *key = roar_tag_convert(meta->items[i]->type, &is_uuid);
		if (key != NULL)
		{
			if (is_uuid)
			{
				snprintf(uuid_buf[cnt], sizeof(uuid_buf[0]), "{UUID}%s",
						meta->items[i]->value);
				vals[cnt].key = g_strdup(key);
				vals[cnt].value = uuid_buf[cnt];
			}
			else
			{
				vals[cnt].key = g_strdup(key);
				vals[cnt].value = meta->items[i]->value;
			}
			cnt++;
		}
	}

	roar_vs_meta(self->vss, vals, cnt, &(self->err));

	for (unsigned i = 0; i < 32; i++)
		g_free(vals[i].key);

	g_mutex_unlock(self->lock);
}

const struct audio_output_plugin roar_output_plugin = {
	.name = "roar",
	.init = roar_init,
	.finish = roar_finish,
	.open = roar_open,
	.play = roar_play,
	.cancel = roar_cancel,
	.close = roar_close,
	.send_tag = roar_send_tag,

	.mixer_plugin = &roar_mixer_plugin
};
