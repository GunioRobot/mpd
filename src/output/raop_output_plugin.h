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

#ifndef MPD_OUTPUT_RAOP_PLUGIN_H
#define MPD_OUTPUT_RAOP_PLUGIN_H

#include <pthread.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <openssl/aes.h>

struct key_data {
	unsigned char *key;
	unsigned char *data;
	struct key_data *next;
};

struct play_state {
	bool playing;
	unsigned short seq_num;
	unsigned int rtptime;
	unsigned int sync_src;
	unsigned int start_rtptime;
	struct timeval start_time;
	struct timeval last_send;
};

/*********************************************************************/

struct rtspcl_data {
	int fd;
	char url[128];
	int cseq;
	struct key_data *kd;
	struct key_data *exthds;
	char *session;
	char *transport;
	unsigned short server_port;
	unsigned short control_port;
	struct in_addr host_addr;
	struct in_addr local_addr;
	const char *useragent;

};

enum pause_state {
	NO_PAUSE = 0,
	OP_PAUSE,
	NODATA_PAUSE,
};

#define MINIMUM_SAMPLE_SIZE 32

#define RAOP_FD_READ (1<<0)
#define RAOP_FD_WRITE (1<<1)

/*********************************************************************/

struct encrypt_data {
	AES_KEY ctx;
	unsigned char iv[16]; // initialization vector for aes-cbc
	unsigned char nv[16]; // next vector for aes-cbc
	unsigned char key[16]; // key for aes-cbc
};

/*********************************************************************/

struct ntp_data {
	unsigned short port;
	int fd;
};

/*********************************************************************/

struct raop_data {
	struct rtspcl_data *rtspcl;
	const char *addr; // target host address
	short rtsp_port;
	struct sockaddr_in ctrl_addr;
	struct sockaddr_in data_addr;

	bool is_master;
	struct raop_data *next;

	unsigned volume;

	pthread_mutex_t control_mutex;

	bool started;
	bool paused;
};

/*********************************************************************/

struct control_data {
	unsigned short port;
	int fd;
};

bool
send_control_command(struct control_data *ctrl, struct raop_data *rd, struct play_state *state);

/*********************************************************************/

#define NUMSAMPLES 352
#define RAOP_BUFFER_SIZE NUMSAMPLES * 4
#define RAOP_HEADER_SIZE 12
#define ALAC_MAX_HEADER_SIZE 8
#define RAOP_MAX_PACKET_SIZE RAOP_BUFFER_SIZE + RAOP_HEADER_SIZE + ALAC_MAX_HEADER_SIZE

// session
struct raop_session_data {
	struct raop_data *raop_list;
	struct ntp_data ntp;
	struct control_data ctrl;
	struct encrypt_data encrypt;
	struct play_state play_state;

	int data_fd;

	unsigned char buffer[RAOP_BUFFER_SIZE];
	size_t bufferSize;

	unsigned char data[RAOP_MAX_PACKET_SIZE];
	int wblk_wsize;
	int wblk_remsize;

	pthread_mutex_t data_mutex;
	pthread_mutex_t list_mutex;
};

//static struct raop_session_data *raop_session;

/*********************************************************************/

bool
raop_set_volume(struct raop_data *rd, unsigned volume);

int
raop_get_volume(struct raop_data *rd);

#endif
