/*
 * stream.c : access to the content of a remote file.
 * This file is part of avmount.
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

#ifdef HAVE_CONFIG_H
#	include "../config.h"
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <curl/curl.h>
#include <talloc.h>
#include "log.h"
#include "minmax.h"
#include "linkedlist.h"

#define MB 			(1024 * 1024)
#define KB			(1024)
#define READAHEAD_BUFSZ_START 	(64 * KB)
#define READAHEAD_BUFSZ_STEP	( 2 * MB)
#define READAHEAD_BUFSZ_MAX	((10 * MB) + READAHEAD_BUFSZ_START)
#define READAHEAD_CHUNK_SIZE	(8 * KB)
#define READAHEAD_TRESHOLD	(5)
#define READAHEAD_FSEEK_MAX   	(64 * KB)
#define RETRIES_MAX		(3)

/* Macros for optimizing likely branches */
#define LIKELY(x)		(__builtin_expect(!!(x), 1))
#define UNLIKELY(x)		(__builtin_expect(!!(x), 0))

/* Stream handle */
LISTABLE_TYPE(Stream,
	CURL*           handle;
	CURLM*          multi_handle;
	CURLcode        result;

	int             connected;
	int             eof;
	char*           buf;
	char*           ptr;
	size_t          bufsz;
	size_t          bufcnt;
	size_t          offset;
	int             retries;
	pthread_mutex_t read_lock;

	/* readahead stuff */
	pthread_t       ra_thread;
	pthread_mutex_t ra_lock;
	pthread_cond_t  ra_signal;
	char*           ra_buf;
	char*           ra_ptr;
	size_t          ra_avail;
	size_t          ra_wants;
	off_t           ra_seekto;
	off_t           ra_offset;
	int             ra_running;
	unsigned int    ra_reads;
	int             ra_abort;
	int             ra_growbuf;
	const char*     ra_bufend;
);

static pthread_mutex_t streams_lock = PTHREAD_MUTEX_INITIALIZER;
LIST_DECLARE_STATIC(streams);

static void *context = NULL;

/* curl callback structure */
struct _Stream_CallbackData
{
	char* buf;
	size_t avail;
	size_t rem;
	Stream *file;
};

/*
 * cURL memory allocation functions
 */
static void*
curl_malloc_func(size_t size)
{
	void *ret = talloc_size(context, size);
#ifdef DEBUG
	if (ret != NULL) {
		talloc_set_name(ret, "libcurl");
	}
#endif
	return ret;
}
static void
curl_free_func(void *ptr)
{
	talloc_free(ptr);
}
static void*
curl_realloc_func(void *ptr, size_t size)
{
	void *ret = talloc_realloc_size(context, ptr, size);
#ifdef DEBUG
	if (ret != NULL) {
		talloc_set_name(ret, "libcurl");
	}
#endif
	return ret;
}
static char*
curl_strdup_func(const char *str)
{
	void *ret = talloc_strdup(context, str);
#ifdef DEBUG
	if (ret != NULL) {
		talloc_set_name(ret, "libcurl");
	}
#endif
	return ret;
}
static void*
curl_calloc_func(size_t nmemb, size_t size)
{
	void *ret = talloc_zero_size(context, nmemb * size);
#ifdef DEBUG
	if (ret != NULL) {
		talloc_set_name(ret, "libcurl");
	}
#endif
	return ret;
}

/**
 * Stream_AddToList() -- Add stream to list.
 */
static void
Stream_AddToList(Stream *stream)
{
	pthread_mutex_lock(&streams_lock);
	LIST_ADD(&streams, stream);
	pthread_mutex_unlock(&streams_lock);
}

/**
 * Stream_RemoveFromList() -- Remove stream from list
 */
static void
Stream_RemoveFromList(Stream *stream)
{
	pthread_mutex_lock(&streams_lock);
	LIST_REMOVE(stream);
	pthread_mutex_unlock(&streams_lock);
}

/**
 * Stream_Free() -- Free's a stream handle and all it's
 * associated buffers.
 */
static void
Stream_Free(Stream *file)
{
	if (file->ra_buf != NULL) {
		munmap(file->ra_buf, READAHEAD_BUFSZ_MAX);
	}
	curl_multi_remove_handle(file->multi_handle, file->handle);
	curl_easy_cleanup(file->handle);
	curl_multi_cleanup(file->multi_handle);
	talloc_free(file);
}

/**
 * Stream_Destroy() -- Free's all stream buffers. This
 * should be called after forking by the child process.
 */
void
Stream_Destroy()
{
	Stream *stream;
	Log_Print(LOG_DEBUG, "Stream_Destroy() running");
	LIST_FOREACH_SAFE(Stream*, stream, &streams, {
		Stream_RemoveFromList(stream);
		Stream_Free(stream);
	});

	curl_global_cleanup();
	talloc_free(context);
}

/**
 * Stream_WriteCallback() -- cURL callback routine
 */
static size_t
Stream_WriteCallback(char *buffer, size_t size, size_t nitems, void *userp)
{
	struct _Stream_CallbackData *cbdata = (struct _Stream_CallbackData*) userp;
	Stream *file = cbdata->file;
	size_t bytes_to_copy;
	size_t sz = (size = (size * nitems));

	assert(file->bufcnt == 0);

	/* get what we need */
	if (LIKELY(cbdata->rem > 0)) {
		bytes_to_copy = MIN(sz, cbdata->rem);
		if (cbdata->buf != NULL) {
			memcpy(cbdata->buf + cbdata->avail, buffer, bytes_to_copy);
		}
		cbdata->avail += bytes_to_copy;
		cbdata->rem -= bytes_to_copy;
		sz -= bytes_to_copy;
		buffer += bytes_to_copy;
	}

	/* if there's any leftovers save them in buffer */
	if (LIKELY(sz > 0)) {
		size_t avail_buf = (file->bufsz - file->bufcnt);
		if (UNLIKELY(sz > avail_buf)) {
			char *newbuf;
			size_t needed_buf = (sz - avail_buf);
			newbuf = talloc_realloc_size(file, file->buf, file->bufsz + needed_buf);
			if (UNLIKELY(newbuf == NULL)) {
				Log_Printf(LOG_ERROR, "Failed to grow buffer");
				size -= (sz - avail_buf);
				sz = avail_buf;
			} else {
#ifdef DEBUG
				talloc_set_name(newbuf, "Buffer");
#endif
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
 * Stream_FillBuffer() -- Attempt to fill buffer with streamed data
 */
static size_t
Stream_FillBuffer(Stream *file, char *buffer, size_t size)
{
	CURLMcode mc;
	fd_set fdread, fdwrite, fdexcep;
	struct timeval timeout;
	struct _Stream_CallbackData cbdata;
	int rc;
	int still_running = 0;

	cbdata.buf = buffer;
	cbdata.avail = 0;
	cbdata.rem = size;
	cbdata.file = file;
	curl_easy_setopt(file->handle, CURLOPT_WRITEDATA, &cbdata);

	/* if we got data in the buffer use it first */
	if (LIKELY(file->bufcnt && cbdata.rem)) {
		size_t bytes_to_copy = MIN(file->bufcnt, cbdata.rem);
		if (LIKELY(cbdata.buf != NULL)) {
			memcpy(cbdata.buf, file->ptr, bytes_to_copy);
		}
		cbdata.avail += bytes_to_copy;
		cbdata.rem -= bytes_to_copy;
		file->bufcnt -= bytes_to_copy;
		file->ptr += bytes_to_copy;
		if (UNLIKELY(cbdata.rem == 0)) {
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
	if (UNLIKELY(!file->connected)) {
		curl_easy_setopt(file->handle, CURLOPT_RESUME_FROM, (long) file->ra_offset);
		curl_multi_perform(file->multi_handle, &still_running);
		file->connected = still_running;
		if (UNLIKELY(!still_running)) {
			if (cbdata.avail == 0) {
				Log_Printf(LOG_ERROR, "Stream_FillBuffer() -- Http Connection Failed!");
				curl_multi_remove_handle(file->multi_handle, file->handle);
				curl_easy_cleanup(file->handle);
				file->handle = NULL;
				/* TODO: check that this is actually what happened */
				file->result = CURLE_COULDNT_CONNECT;
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
		timeout.tv_sec = 5;
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
		if (UNLIKELY(mc != CURLM_OK)) {
			Log_Printf(LOG_ERROR, "curl_multi_fdset() failed, code %d.\n", mc);
			return -1;
		}

		if (maxfd == -1) {
			usleep(100 * 1000);
			continue;
		} else {
			rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);
		}

		if (LIKELY(rc != -1)) {
			while (UNLIKELY((rc = curl_multi_perform(file->multi_handle,
				&still_running)) == CURLM_CALL_MULTI_PERFORM));
			if (UNLIKELY(rc != CURLM_OK)) {
				Log_Printf(LOG_ERROR, "Stream_FillBuffer() -- "
					"curl_multi_perform() returned %i", rc);
			}
			if (UNLIKELY(!still_running)) {
				CURLMsg *m;
				int msgcnt;
				while ((m = curl_multi_info_read(file->multi_handle, &msgcnt)) != NULL) {
					assert(msgcnt == 0);
					Log_Printf(LOG_DEBUG, "Stream_FillBuffer() -- "
						"curl: msg=%i result=%i", m->msg, m->data.result);
					file->result = m->data.result;
				}
			}
		}
	}
	while (LIKELY(still_running && (cbdata.rem > 0)));

	return (cbdata.avail) ? cbdata.avail : -1;
}

/**
 * Stream_Init()
 */
void
Stream_Init()
{
	LIST_INIT(&streams);

	context = talloc_named(NULL, 0, "Streamer");
	if (context == NULL) {
		Log_Print(LOG_ERROR, "Stream_Init() -- Out of memory");
		exit(1);
	}
	curl_global_init_mem(CURL_GLOBAL_DEFAULT,
		curl_malloc_func,
		curl_free_func,
		curl_realloc_func,
		curl_strdup_func,
		curl_calloc_func);
}

/**
 * Stream_Open()
 */
Stream*
Stream_Open(const char *url)
{
	Stream *file;

	Log_Printf(LOG_DEBUG, "Stream_Open(%s)", url);

	if (UNLIKELY((file = talloc_zero(context, Stream)) == NULL)) {
		Log_Printf(LOG_ERROR, "Stream_Open(): Out of memory");
		return NULL;
	}

#ifdef DEBUG
	talloc_set_name(file, "Stream:%s", url);
#endif

	assert(CURLE_OK == 0);
	file->handle = curl_easy_init();
	file->multi_handle = curl_multi_init();

	pthread_mutex_init(&file->read_lock, NULL);
	pthread_mutex_init(&file->ra_lock, NULL);
	pthread_cond_init(&file->ra_signal, NULL);
	pthread_cond_init(&file->ra_signal, NULL);

	if (UNLIKELY(file->handle == NULL || file->multi_handle == NULL)) {
		Log_Printf(LOG_ERROR, "curl_easy_init() or curl_multi_init() failed");
		return NULL;
	}

	Stream_AddToList(file);

	curl_easy_setopt (file->handle, CURLOPT_URL, url);
	curl_easy_setopt (file->handle, CURLOPT_VERBOSE, 0L);
	curl_easy_setopt (file->handle, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt (file->handle, CURLOPT_WRITEFUNCTION, Stream_WriteCallback);
	curl_easy_setopt (file->handle, CURLOPT_USERAGENT, "avmount/0.8");
	curl_multi_add_handle(file->multi_handle, file->handle);
	return file;
}

/**
 * Stream_Seek()
 */
void
Stream_Seek(Stream *file, off_t offset)
{
	Log_Printf(LOG_DEBUG, "Stream_Seek(%lx, %zd)",
		(unsigned long) file, offset);

	/*
	 * if the readahead thread is running send the
	 * seek request to it
	 */
	if (LIKELY(file->ra_running)) {
		pthread_mutex_lock(&file->ra_lock);
		if (LIKELY(file->ra_running)) {
			file->ra_abort = 1;
			file->ra_seekto = offset;
			pthread_cond_signal(&file->ra_signal);
			pthread_mutex_unlock(&file->ra_lock);
			while (UNLIKELY(file->ra_seekto != -1)) {
				pthread_mutex_lock(&file->ra_lock);
				if (LIKELY(file->ra_seekto != -1)) {
					pthread_cond_wait(&file->ra_signal, &file->ra_lock);
				}
				pthread_mutex_unlock(&file->ra_lock);
			}
			return;
		} else {
			pthread_mutex_unlock(&file->ra_lock);
		}
	}

	curl_multi_remove_handle(file->multi_handle, file->handle);
	curl_easy_setopt(file->handle, CURLOPT_RESUME_FROM, (long) offset);
	curl_multi_add_handle(file->multi_handle, file->handle);
	file->bufcnt = 0;
	file->ptr = file->buf;
	file->connected = 0;
	file->eof = 0;
	file->offset = offset;
	file->ra_offset = offset;
}

/**
 * Stream_ReadAhead() -- Reads and buffers stream in
 * background thread.
 */
static void*
Stream_ReadAhead(void *f)
{
	char *ptr = NULL;
	Stream *file = (Stream*) f;

	if (file->ra_buf == NULL) {
		Log_Printf(LOG_DEBUG, "ReadAhead(file=%lx): Mapping %zd KiB buffer...",
			(unsigned long) file, (size_t) READAHEAD_BUFSZ_MAX / 1024);
		file->ra_buf = mmap(NULL, READAHEAD_BUFSZ_MAX,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (file->ra_buf == MAP_FAILED) {
			Log_Printf(LOG_ERROR, "Stream_ReadAhead() -- malloc() failed");
			file->ra_buf = NULL;
			goto THREAD_EXIT;
		}
		file->ra_bufend = file->ra_buf + READAHEAD_BUFSZ_START;
		file->result = CURLE_OK;
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

	while (LIKELY(!file->ra_abort)) {
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
		if (UNLIKELY(chunksz == 0)) {
			pthread_mutex_lock(&file->ra_lock);
			CALC_CHUNKSZ();
			if (LIKELY(chunksz == 0)) {
				pthread_cond_wait(&file->ra_signal, &file->ra_lock);
				pthread_mutex_unlock(&file->ra_lock);
				continue;
			} else {
				pthread_mutex_unlock(&file->ra_lock);
			}
		}
#undef CALC_CHUNKSZ

		/* read the chunk */
		if (UNLIKELY((bytes_read = Stream_FillBuffer(file, ptr, chunksz)) == -1)) {
			Log_Printf(LOG_DEBUG, "Stream_ReadAhead: Stream_FillBuffer returned %zd",
				bytes_read);
			switch (file->result) {
			case CURLE_OK:
				goto THREAD_EXIT;

			case CURLE_COULDNT_CONNECT:
				Log_Printf(LOG_DEBUG, "Stream_ReadAhead: Sleeping...");
				sleep(5);
				/* fall through */

			default:
				file->connected = 0;
				if (file->retries++ <= RETRIES_MAX) {
					Log_Printf(LOG_DEBUG, "Stream_ReadAhead: Retrying... (file=%lx)",
						(unsigned long) file);
					goto READAHEAD_START;
				} else {
					goto THREAD_EXIT;
				}
			}
			goto THREAD_EXIT;
		}

		if (LIKELY(bytes_read)) {
			if (UNLIKELY((ptr = (ptr + bytes_read)) == file->ra_bufend)) {
				if ((file->ra_bufend < (file->ra_buf + READAHEAD_BUFSZ_MAX)) && file->ra_growbuf) {
					file->ra_bufend += READAHEAD_BUFSZ_STEP;
					file->ra_growbuf = 0;
					assert(file->ra_bufend <= (file->ra_buf + READAHEAD_BUFSZ_MAX));
				} else {
					ptr = file->ra_buf;
				}
			}
			file->ra_offset += bytes_read;
			file->retries = 0;
			pthread_mutex_lock(&file->ra_lock);
			file->ra_avail += bytes_read;
			pthread_cond_signal(&file->ra_signal);
			pthread_mutex_unlock(&file->ra_lock);
		}

#if 0
		Log_Printf(LOG_ERROR, "Stream_ReadAhead(%lx): W: %zd | R: %zd | A: %zd",
			(unsigned long) file, file->ra_wants, chunksz, file->ra_avail);
#endif
	}

THREAD_EXIT:
	pthread_mutex_lock(&file->ra_lock);

	/* if a seek was requested call Stream_Seek() and
	 * return to the top of the loop */
	if (file->ra_seekto != -1) {
		Log_Printf(LOG_DEBUG, "Stream_ReadAhead: Seeking to %zd",
			file->ra_seekto);
		if (file->ra_seekto > file->offset) {
			if (file->ra_seekto <= file->ra_offset) {
				/* we already have the data */
				Log_Printf(LOG_DEBUG, "Stream_ReadAhead(file=%lx): In-buffer seek",
					(unsigned long) file);
				size_t bytes_to_skip = file->ra_seekto - file->offset;
				file->ra_ptr += bytes_to_skip;
				if (file->ra_ptr >= file->ra_bufend) {
					file->ra_ptr = file->ra_buf + (file->ra_ptr - file->ra_bufend);
				}
				file->ra_avail -= bytes_to_skip;
				file->offset = file->ra_seekto;
				file->ra_seekto = -1;
				file->ra_abort = 0;
				pthread_mutex_unlock(&file->ra_lock);
				goto READAHEAD_START;
			} else if ((file->ra_seekto - file->ra_offset) < READAHEAD_FSEEK_MAX) {
				/* short seek */
				Log_Printf(LOG_DEBUG, "Stream_ReadAhead(file=%lx): Short seek",
					(unsigned long) file);
				size_t bytes_to_skip = file->ra_seekto - file->ra_offset;
				size_t bytes_read = 0;
				file->ra_ptr = ptr = file->ra_buf;
				file->ra_avail = 0;
				file->offset = file->ra_seekto;
				file->ra_seekto = -1;
				file->ra_abort = 0;

				assert(bytes_to_skip > 0);

				if ((bytes_read = Stream_FillBuffer(file, NULL, bytes_to_skip)) == -1) {
					pthread_mutex_unlock(&file->ra_lock);
					goto THREAD_EXIT;
				}
				file->ra_offset += bytes_read;
				assert(bytes_read == bytes_to_skip);
				pthread_mutex_unlock(&file->ra_lock);
				goto READAHEAD_START;
			}
		}
		file->ra_running = 0;
		Stream_Seek(file, file->ra_seekto);
		READAHEAD_INIT();
		pthread_mutex_unlock(&file->ra_lock);
		goto READAHEAD_START;
	}

	Log_Printf(LOG_DEBUG, "Stream_ReadAhead(%lx): "
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

/**
 * Stream_Read()
 */
ssize_t
Stream_Read(Stream *file, void *ptr, size_t size)
{
	ssize_t bytes_read = 0;

	pthread_mutex_lock(&file->read_lock);

	Log_Printf(LOG_DEBUG, "Stream_Read(%lx, %lx, %zd) - offset=%zd",
		(unsigned long) file, (unsigned long) ptr, size, file->offset);

	if (UNLIKELY(size == 0)) {
		goto STREAM_READ_EXIT;
	}

	/* Make sure size is not too big to handle */
	if (UNLIKELY(size > (SIZE_MAX / 2))) {
		Log_Printf(LOG_WARNING, "Stream_Read() -- Size would cause overflow. Truncating to %zd",
			SIZE_MAX / 2);
		size = (SIZE_MAX / 2);
	}

	/* signal worker that we want data */
	pthread_mutex_lock(&file->ra_lock);
	file->ra_wants = size;
	pthread_cond_signal(&file->ra_signal);
	pthread_mutex_unlock(&file->ra_lock);

	/* if there's no worker thread, get it going */
	if (UNLIKELY(!file->ra_running && file->ra_avail == 0)) {
		assert(file->ra_abort == 0);
		pthread_mutex_lock(&file->ra_lock);
		if (pthread_create(&file->ra_thread, NULL, Stream_ReadAhead, (void*) file)) {
			Log_Printf(LOG_ERROR, "Stream_Read() -- pthread_create() failed!");
			bytes_read = -1;
			goto STREAM_READ_EXIT;
		}
		pthread_cond_wait(&file->ra_signal, &file->ra_lock);
		pthread_mutex_unlock(&file->ra_lock);
	}

	while (LIKELY(bytes_read < size)) {
		size_t chunksz;

#define CALC_CHUNKSZ() \
		chunksz = MIN(size - bytes_read, file->ra_avail); \
		chunksz = MIN(file->ra_bufend - file->ra_ptr, chunksz)
		CALC_CHUNKSZ();

		/* wait until there's data available */
		if (UNLIKELY(chunksz == 0)) {
			pthread_mutex_lock(&file->ra_lock);
			CALC_CHUNKSZ();
			if (LIKELY(chunksz == 0)) {
				if (UNLIKELY(!file->ra_running)) {
					pthread_mutex_unlock(&file->ra_lock);
					if (bytes_read == 0) {
						if (file->result == CURLE_OK && !file->eof) {
							Log_Printf(LOG_DEBUG, "Stream_Read: Returning 0 (eof)");
							file->eof = 1;
							bytes_read = 0;
							goto STREAM_READ_EXIT;
						}
						Log_Printf(LOG_ERROR, "Stream: Read failed! (result=%i retries=%i)",
							file->result, file->retries);
						bytes_read = -1;
						goto STREAM_READ_EXIT;
					} else {
						Log_Printf(LOG_DEBUG, "Stream: Requested %zd but got %zd",
							size, bytes_read);
						file->offset += bytes_read;
						goto STREAM_READ_EXIT;
					}
				}
				if (LIKELY(file->ra_reads > READAHEAD_TRESHOLD)) {
					Log_Printf(LOG_DEBUG, "Stream_Read(%lx): Waiting for data!",
						(unsigned long) file);
					if (file->ra_growbuf == 0) {
						file->ra_growbuf = 1;
					}
					pthread_cond_wait(&file->ra_signal, &file->ra_lock);
					pthread_mutex_unlock(&file->ra_lock);

					/* wait for buffer to be full */
					while (file->ra_running &&
						file->ra_avail < (file->ra_bufend - file->ra_buf)) {
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
		if (UNLIKELY((file->ra_ptr = (file->ra_ptr + chunksz)) == file->ra_bufend)) {
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
		Log_Printf(LOG_ERROR, "Stream_Read(%lx, %lx, %zd): +%zd | %zd",
			(unsigned long) file, (unsigned long) ptr, size, chunksz, bytes_read);
#endif
	}
	file->offset += bytes_read;
	file->ra_reads++;

STREAM_READ_EXIT:
	pthread_mutex_unlock(&file->read_lock);
	return bytes_read;
}

/**
 * Stream_Close()
 */
void
Stream_Close(Stream *file)
{
	Log_Printf(LOG_DEBUG, "Stream_Close(%lx)", (unsigned long) file);
	Log_Printf(LOG_DEBUG, "Stream: Buffer size: %zd", file->bufsz);
	Log_Printf(LOG_DEBUG, "Stream: Readahead buffer size: %zd",
		(file->ra_bufend - file->ra_buf));

	if (file->ra_running) {
		pthread_mutex_lock(&file->ra_lock);
		file->ra_abort = 1;
		pthread_cond_signal(&file->ra_signal);
		pthread_mutex_unlock(&file->ra_lock);
		pthread_join(file->ra_thread, NULL);
		assert(file->ra_abort == 0);
	}

	Stream_RemoveFromList(file);
	Stream_Free(file);
}
