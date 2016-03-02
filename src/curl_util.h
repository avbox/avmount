/*
 * curl_util.h : access to the content of a remote file.
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

#ifndef __CURL_UTIL__
#define __CURL_UTIL__

/**
 * File handle structure
 */
typedef struct _CurlUtil_File CurlUtil_File;

/**
 * Initialize the cURL library.
 */
void
CurlUtil_Init();

/**
 * Opens a URL for streaming
 */
CurlUtil_File*
CurlUtil_Open(const char *url);

/**
 * Reads data from a network stream
 */
size_t
CurlUtil_Read(CurlUtil_File *file, void *ptr, size_t size);

/**
 * Seek the network stream
 */
void
CurlUtil_Seek(CurlUtil_File *file, off_t offset);

/**
 * Closes a URL
 */
void
CurlUtil_Close(CurlUtil_File *file);

#endif
