/*
 * curl_util.c : access to the content of a remote file.
 * This file is part of djmount.
 *
 * (C) Copyright 2016 Fernando Rodriguez
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <curl/curl.h>
#include "log.h"
#include "minmax.h"

#define ENABLE_READAHEAD 			(1)
#define READAHEAD_BUFSZ_MAX		(1024 * 1024 * 10)
#define READAHEAD_BUFSZ_STEP	(1024 * 512)
#define READAHEAD_CHUNK_SIZE	(1024 * 32)
#define READAHEAD_TRESHOLD		(5)

#if ENABLE_READAHEAD
#include <pthread.h>
#include <sys/mman.h>
#endif

/* cURL file handle */
struct _CurlUtil_File
{
	CURL*  handle;
	CURLM* multi_handle;
	int    connected;

	char*  buf;
	char*  ptr;
	size_t bufsz;
	size_t bufcnt;
	size_t offset;

#if ENABLE_READAHEAD
	pthread_t       ra_thread;
	pthread_mutex_t ra_lock;
	pthread_cond_t  ra_signal;
	char*           ra_buf;
	char*           ra_ptr;
	size_t          ra_avail;
	size_t          ra_wants;
	off_t           ra_seekto;
	int             ra_running;
	unsigned int    ra_reads;
	int             ra_abort;
	int             ra_growbuf;
	const char*     ra_bufend;
#endif
};

typedef struct _CurlUtil_File CurlUtil_File;

/* curl callback structure */
struct _CurlUtil_CallbackData
{
	char* buf;
	size_t avail;
	size_t rem;
	CurlUtil_File *file;
};

/**
 * CurlUtil_WriteCallback() -- cURL callback routine
 */
static size_t
CurlUtil_WriteCallback(char *buffer, size_t size, size_t nitems, void *userp)
{
	struct _CurlUtil_CallbackData *cbdata = (struct _CurlUtil_CallbackData*) userp;
	CurlUtil_File *file = cbdata->file;
	size_t bytes_to_copy;
	size_t sz = (size = (size * nitems));

	assert(file->bufcnt == 0);

	/* get what we need */
	if (cbdata->rem > 0) {
		bytes_to_copy = MIN(sz, cbdata->rem);
		memcpy(cbdata->buf + cbdata->avail, buffer, bytes_to_copy);
		cbdata->avail += bytes_to_copy;
		cbdata->rem -= bytes_to_copy;
		sz -= bytes_to_copy;
		buffer += bytes_to_copy;
	}

	/* if there's any leftovers save them in buffer */
	if (sz > 0) {
		size_t avail_buf = (file->bufsz - file->bufcnt);
		if (sz > avail_buf) {
			char *newbuf;
			size_t needed_buf = (sz - avail_buf);
			newbuf = realloc(file->buf, file->bufsz + needed_buf);
			if (newbuf == NULL) {
				Log_Printf(LOG_ERROR, "Failed to grow buffer");
				size -= (sz - avail_buf);
				sz = avail_buf;
			} else {
				if (file->buf == NULL) {
					file->ptr = newbuf;
				} else {
					file->ptr = newbuf + (file->ptr - file->buf);
				}
				file->buf = newbuf;
				file->bufsz += needed_buf;
			}
		}
		memcpy(file->buf + file->bufcnt, buffer, sz);
		file->bufcnt += sz;
	}
	return size;
}

/**
 * CurlUtil_FillBuffer() -- Attempt to fill buffer with streamed data
 */
static size_t
CurlUtil_FillBuffer(CurlUtil_File *file, char *buffer, size_t size)
{
	CURLMcode mc;
	fd_set fdread, fdwrite, fdexcep;
	struct timeval timeout;
	struct _CurlUtil_CallbackData cbdata;
	int rc;
	int still_running = 0;

	cbdata.buf = buffer;
	cbdata.avail = 0;
	cbdata.rem = size;
	cbdata.file = file;
	curl_easy_setopt(file->handle, CURLOPT_WRITEDATA, &cbdata);

	/* if we got data in the buffer use it first */
	if (file->bufcnt && cbdata.rem) {
		size_t bytes_to_copy = MIN(file->bufcnt, cbdata.rem);
		memcpy(cbdata.buf, file->ptr, bytes_to_copy);
		cbdata.avail += bytes_to_copy;
		cbdata.rem -= bytes_to_copy;
		file->bufcnt -= bytes_to_copy;
		file->ptr += bytes_to_copy;
		if (cbdata.rem == 0) {
			return cbdata.avail;
		}
	}

	/* if we're here then the buffer is empty 
	 * so we need to reset the buffer pointer */
	file->ptr = file->buf;
	assert(file->bufcnt == 0);

	/*
	 * if the connection has not been established
	 * then establish it now
	 */
	if (!file->connected) {
		curl_multi_perform(file->multi_handle, &still_running);
		file->connected = still_running;
		if (!still_running) {
			if (cbdata.avail == 0) {
				Log_Printf(LOG_ERROR, "Http Connection Failed!");
				curl_multi_remove_handle(file->multi_handle, file->handle);
				curl_easy_cleanup(file->handle);
				file->handle = NULL;
				return -1;
			} else {
				return cbdata.avail;
			}
		}
	}

	do {
		int maxfd = -1;
		long curl_timeo = -1;

		FD_ZERO(&fdread);
		FD_ZERO(&fdwrite);
		FD_ZERO(&fdexcep);

		/* set a suitable timeout to fail on */
		timeout.tv_sec = 60;
		timeout.tv_usec = 0;
		curl_multi_timeout(file->multi_handle, &curl_timeo);
		if(curl_timeo >= 0) {
			timeout.tv_sec = curl_timeo / 1000;
			if(timeout.tv_sec > 1) {
				timeout.tv_sec = 1;
			} else {
				timeout.tv_usec = (curl_timeo % 1000) * 1000;
			}
		}

		/* get file descriptors from the transfers */
		mc = curl_multi_fdset(file->multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);
		if (mc != CURLM_OK) {
			Log_Printf(LOG_ERROR, "curl_multi_fdset() failed, code %d.\n", mc);
			return -1;
		}

		if (maxfd == -1) {
			usleep(100 * 1000);
			continue;
		} else {
			rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);
		}

		if (rc != -1) {
			while (curl_multi_perform(file->multi_handle, &still_running) ==
				CURLM_CALL_MULTI_PERFORM);
		}
	}
	while (still_running && (cbdata.rem > 0));

	return (cbdata.avail) ? cbdata.avail : -1;
}

/**
 * CurlUtil_Init()
 */
void
CurlUtil_Init()
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

/**
 * CurlUtil_Open()
 */
CurlUtil_File*
CurlUtil_Open(const char *url)
{
	CurlUtil_File *file;

	Log_Printf(LOG_DEBUG, "CurlUtil_Open(%s)", url);

	if ((file = malloc(sizeof(CurlUtil_File))) == NULL) {
		Log_Printf(LOG_ERROR, "malloc() failed");
		return NULL;
	}

	file->handle = curl_easy_init();
	file->multi_handle = curl_multi_init();
	file->buf = NULL;
	file->bufcnt = 0;
	file->bufsz = 0;
	file->connected = 0;
	file->offset = 0;

#if ENABLE_READAHEAD
	file->ra_buf = NULL;
	file->ra_running = 0;
	file->ra_reads = 0;
	file->ra_abort = 0;
	file->ra_avail = 0;
	pthread_mutex_init(&file->ra_lock, NULL);
	pthread_cond_init(&file->ra_signal, NULL);
	pthread_cond_init(&file->ra_signal, NULL);
#endif

	if (file->handle == NULL || file->multi_handle == NULL) {
		Log_Printf(LOG_ERROR, "curl_easy_init() or curl_multi_init() failed");
		return NULL;
	}

	curl_easy_setopt (file->handle, CURLOPT_URL, url);
	curl_easy_setopt (file->handle, CURLOPT_VERBOSE, 0L);
	curl_easy_setopt (file->handle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt (file->handle, CURLOPT_WRITEFUNCTION, CurlUtil_WriteCallback);
	curl_easy_setopt (file->handle, CURLOPT_USERAGENT, "avmount/0.8");
	curl_multi_add_handle(file->multi_handle, file->handle);
	return file;
}

/**
 * CurlUtil_Seek()
 */
void
CurlUtil_Seek(CurlUtil_File *file, off_t offset)
{
	Log_Printf(LOG_DEBUG, "CurlUtil_Seek(%lx, %zd)",
		(unsigned long) file, offset);

#if 0
	Log_Printf(LOG_ERROR, "CurlUtil_Seek: running=%i seekto=%zd abort=%i",
		file->ra_running, file->ra_seekto, file->ra_abort);
#endif

#if ENABLE_READAHEAD
	if (file->ra_running) {
		pthread_mutex_lock(&file->ra_lock);
		if (file->ra_running) {
			file->ra_abort = 1;
			file->ra_seekto = offset;
			pthread_cond_signal(&file->ra_signal);
			pthread_mutex_unlock(&file->ra_lock);
			while (file->ra_seekto != -1) {
				pthread_mutex_lock(&file->ra_lock);
				pthread_cond_wait(&file->ra_signal, &file->ra_lock);
				pthread_mutex_unlock(&file->ra_lock);
			}
			return;
		} else {
			pthread_mutex_unlock(&file->ra_lock);
		}
	}
#endif

	curl_multi_remove_handle(file->multi_handle, file->handle);
	curl_easy_setopt(file->handle, CURLOPT_RESUME_FROM, (long) offset);
	curl_multi_add_handle(file->multi_handle, file->handle);
	file->bufcnt = 0;
	file->ptr = file->buf;
	file->connected = 0;
	file->offset = offset;
}

#if ENABLE_READAHEAD
static void*
CurlUtil_ReadAhead(void *f)
{
	char *ptr;
	CurlUtil_File *file = (CurlUtil_File*) f;

	if (file->ra_buf == NULL) {
		file->ra_buf = mmap(NULL, READAHEAD_BUFSZ_MAX,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (file->ra_buf == NULL) {
			Log_Printf(LOG_ERROR, "CurlUtil_ReadAhead() -- malloc() failed");
			goto THREAD_EXIT;
		}
		file->ra_bufend = file->ra_buf + READAHEAD_BUFSZ_STEP;
	}

#define READAHEAD_INIT() \
	ptr = file->ra_buf; \
	file->ra_running = 1; \
	file->ra_ptr = file->ra_buf; \
	file->ra_avail = 0; \
	file->ra_seekto = -1; \
	file->ra_growbuf = 0; \
	file->ra_abort = 0; \
	file->ra_reads = 0;
	READAHEAD_INIT();

READAHEAD_START:
	/* signal that we're up and running */
	pthread_mutex_lock(&file->ra_lock);
	pthread_cond_signal(&file->ra_signal);
	pthread_mutex_unlock(&file->ra_lock);

	while (!file->ra_abort) {
		size_t chunksz;
		size_t bytes_read;

		/* calculate the size of the next chunk */
#define CALC_CHUNKSZ() \
		chunksz = (file->ra_bufend - file->ra_buf) - file->ra_avail; \
		chunksz = MIN((file->ra_bufend - ptr), chunksz); \
		chunksz = MIN(READAHEAD_CHUNK_SIZE, chunksz); \
		chunksz = (file->ra_reads < READAHEAD_TRESHOLD) ? \
			MIN(file->ra_wants, chunksz) : chunksz;
		CALC_CHUNKSZ();

		/* if no chunk is needed wait until signaled */
		if (chunksz == 0) {
			pthread_mutex_lock(&file->ra_lock);
			CALC_CHUNKSZ();
			if (chunksz == 0) {
				pthread_cond_wait(&file->ra_signal, &file->ra_lock);
				pthread_mutex_unlock(&file->ra_lock);
				continue;
			} else {
				pthread_mutex_unlock(&file->ra_lock);
			}
		}
#undef CALC_CHUNKSZ

		/* read the chunk */
		if ((bytes_read = CurlUtil_FillBuffer(file, ptr, chunksz)) == -1) {
			goto THREAD_EXIT;
		}
		if (bytes_read) {
			if ((ptr = (ptr + bytes_read)) == file->ra_bufend) {
				if ((file->ra_bufend < (file->ra_buf + READAHEAD_BUFSZ_MAX)) && file->ra_growbuf) {
					file->ra_bufend += READAHEAD_BUFSZ_STEP;
					file->ra_growbuf = 0;
					assert(file->ra_bufend <= (file->ra_buf + READAHEAD_BUFSZ_MAX));
				} else {
					ptr = file->ra_buf;
				}
			}
			pthread_mutex_lock(&file->ra_lock);
			file->ra_avail += bytes_read;
			pthread_cond_signal(&file->ra_signal);
			pthread_mutex_unlock(&file->ra_lock);
		}

#if 0
		Log_Printf(LOG_ERROR, "CurlUtil_ReadAhead(%lx): W: %zd | R: %zd | A: %zd",
			(unsigned long) file, file->ra_wants, chunksz, file->ra_avail);
#endif
	}

THREAD_EXIT:
	pthread_mutex_lock(&file->ra_lock);

	/* if a seek was requested call CurlUtil_Seek() and
	 * return to the top of the loop */
	if (file->ra_seekto != -1) {
		Log_Printf(LOG_DEBUG, "CurlUtil_ReadAhead: Seeking to %zd",
			file->ra_seekto);
		file->ra_running = 0;
		CurlUtil_Seek(file, file->ra_seekto);
		READAHEAD_INIT();
		pthread_mutex_unlock(&file->ra_lock);
		goto READAHEAD_START;
	}

	Log_Printf(LOG_DEBUG, "CurlUtil_ReadAhead(%lx): "
		"Exited (abort=%i avail=%zd bufcnt=%zd offset=%zd)",
		(unsigned long) file, file->ra_abort, file->ra_avail,
		file->bufcnt, file->offset);

	file->ra_abort = 0;
	file->ra_running = 0;
	file->ra_reads = 0;
	pthread_cond_signal(&file->ra_signal);
	pthread_mutex_unlock(&file->ra_lock);

	return NULL;
}
#undef READAHEAD_INIT
#endif

/**
 * CurlUtil_Read()
 */
size_t
CurlUtil_Read(CurlUtil_File *file, void *ptr, size_t size)
{
	Log_Printf(LOG_DEBUG, "CurlUtil_Read(%lx, %lx, %zd) - offset=%zd",
		(unsigned long) file, (unsigned long) ptr, size, file->offset);

	if (size == 0) {
		return 0;
	}
#if ENABLE_READAHEAD

	size_t bytes_read = 0;

	/* signal worker that we want data */
	pthread_mutex_lock(&file->ra_lock);
	file->ra_wants = size;
	pthread_cond_signal(&file->ra_signal);
	pthread_mutex_unlock(&file->ra_lock);

	/* if there's no worker thread, get it going */
	if (!file->ra_running && file->ra_avail == 0) {
		assert(file->ra_abort == 0);
		pthread_mutex_lock(&file->ra_lock);
		if (pthread_create(&file->ra_thread, NULL, CurlUtil_ReadAhead, (void*) file)) {
			Log_Printf(LOG_ERROR, "CurlUtil_Read() -- pthread_create() failed!");
			return -1;
		}
		pthread_cond_wait(&file->ra_signal, &file->ra_lock);
		pthread_mutex_unlock(&file->ra_lock);
	}

	while (bytes_read < size) {
		size_t chunksz;

#define CALC_CHUNKSZ() \
		chunksz = MIN(size - bytes_read, file->ra_avail); \
		chunksz = MIN(file->ra_bufend - file->ra_ptr, chunksz)
		CALC_CHUNKSZ();

		/* wait until there's data available */
		if (chunksz == 0) {
			pthread_mutex_lock(&file->ra_lock);
			CALC_CHUNKSZ();
			if (chunksz == 0) {
				if (!file->ra_running) {
					pthread_mutex_unlock(&file->ra_lock);
					Log_Printf(LOG_DEBUG, "CurlUtil: Requested %zd but got %zd",
						size, bytes_read);
					file->offset += bytes_read;
					return bytes_read;
				}
				if (file->ra_reads > READAHEAD_TRESHOLD) {
					Log_Printf(LOG_DEBUG, "CurlUtil_Read(%lx): Waiting for data!",
						(unsigned long) file);
					if (file->ra_growbuf == 0) {
						file->ra_growbuf = 1;
					}
					pthread_cond_wait(&file->ra_signal, &file->ra_lock);
					pthread_mutex_unlock(&file->ra_lock);

					/* wait for buffer to be full */
					while (file->ra_avail < (file->ra_bufend - file->ra_buf)) {
						pthread_mutex_lock(&file->ra_lock);
						pthread_cond_wait(&file->ra_signal, &file->ra_lock);
						pthread_mutex_unlock(&file->ra_lock);
					}
				} else {
					pthread_cond_wait(&file->ra_signal, &file->ra_lock);
					pthread_mutex_unlock(&file->ra_lock);
				}
				continue;
			} else {
				pthread_mutex_unlock(&file->ra_lock);
			}
		}
#undef CALC_CHUNKSZ

		/* read the chunk */
		memcpy(ptr, file->ra_ptr, chunksz);
		ptr += chunksz;
		bytes_read += chunksz;
		if ((file->ra_ptr = (file->ra_ptr + chunksz)) == file->ra_bufend) {
			file->ra_ptr = file->ra_buf;
		}

		/* decrease count and signal worker */
		pthread_mutex_lock(&file->ra_lock);
		file->ra_avail -= chunksz;
		file->ra_wants -= chunksz;
		pthread_cond_signal(&file->ra_signal);
		pthread_mutex_unlock(&file->ra_lock);

		assert(file->ra_wants == (size - bytes_read));

#if 0
		Log_Printf(LOG_ERROR, "CurlUtil_Read(%lx, %lx, %zd): +%zd | %zd",
			(unsigned long) file, (unsigned long) ptr, size, chunksz, bytes_read);
#endif
	}
	file->offset += bytes_read;
	file->ra_reads++;
	return bytes_read;
#else
	size_t bytes_read;
	if ((bytes_read = CurlUtil_FillBuffer(file, ptr, size)) == -1) {
		return -1;
	}
	if (bytes_read < size) {
		Log_Printf(LOG_ERROR, "Requested %zd but got %zd", size, bytes_read);
	}
	return bytes_read;
#endif
}

/**
 * CurlUtil_Close()
 */
void
CurlUtil_Close(CurlUtil_File *file)
{
	Log_Printf(LOG_DEBUG, "CurlUtil_Close(%lx)", (unsigned long) file);
	Log_Printf(LOG_DEBUG, "CurlUtil: Buffer size: %zd", file->bufsz);

#if ENABLE_READAHEAD
	Log_Printf(LOG_DEBUG, "CurlUtil: Readahead buffer size: %zd",
		(file->ra_bufend - file->ra_buf));

	if (file->ra_running) {
		pthread_mutex_lock(&file->ra_lock);
		file->ra_abort = 1;
		pthread_cond_signal(&file->ra_signal);
		pthread_mutex_unlock(&file->ra_lock);
		pthread_join(file->ra_thread, NULL);
		assert(file->ra_abort == 0);
	}
	if (file->ra_buf != NULL) {
		munmap(file->ra_buf, READAHEAD_BUFSZ_MAX);
	}
#endif

	curl_multi_remove_handle(file->multi_handle, file->handle);
	curl_easy_cleanup(file->handle);
	curl_multi_cleanup(file->multi_handle);
	free(file->buf);
	free(file);
}
