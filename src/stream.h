/*
 * stream.h : access to the content of a remote file.
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

#ifndef __STREAM_H__
#define __STREAM_H__

/**
 * File handle structure
 */
typedef struct _Stream Stream;

/**
 * Initialize the cURL library.
 */
void
Stream_Init();

/**
 * Opens a URL for streaming
 */
Stream*
Stream_Open(const char *url);

/**
 * Reads data from a network stream
 */
ssize_t
Stream_Read(Stream *file, void *ptr, size_t size);

/**
 * Seek the network stream
 */
void
Stream_Seek(Stream *file, off_t offset);

/**
 * Closes a URL
 */
void
Stream_Close(Stream *file);

/**
 * Free all memory allocated by all streams.
 */
void
Stream_Destroy();

#endif
