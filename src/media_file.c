/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* $Id: media_file.c 243 2006-08-05 13:25:20Z r3mi $
 *
 * Get file information for the media files representing DIDL-Lite objects.
 * This file is part of avmount.
 *
 * (C) Copyright 2016 Fernando Rodriguez
 * (C) Copyright 2005-2006 Rémi Turboult <r3mi@users.sourceforge.net>
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

#include "media_file.h"
#include "log.h"
#include "xml_util.h"
#include "talloc_util.h"
#include "string_util.h"


/******************************************************************************
 * MIME Types
 *****************************************************************************/

/*
 * MIME types below come from :
 * - base : http://freedesktop.org/wiki/Software_2fshared_2dmime_2dinfo
 * - ogg : http://www.rfc-editor.org/rfc/rfc3534.txt
 * - matroska : http://www.matroska.org/technical/specs/notes.html
 * - additional types (e.g. "text/..." subtitles) for interoperability 
 *   with GeeXboX uShare : http://ushare.geexbox.org/ (file src/mime.c)
 */

/*
 * This list is used to determine :
 *
 * 1) if a given MIME type will be served as a playlist (if .playlist field
 *    is not NULL), or directly as a raw file.
 * 2) the file extension to give to the file (if not present in the DIDL-Lite
 *    object's title). Default if NULL : use the MIME subtype, 
 *    without any "*-" prefix. Examples : "audio/x-ac3" -> "ac3"
 *    and "video/x-ms-wmv" -> "wmv".
 *
 * NOTE : this list is *ordered*, because the search on mimetype is done 
 *        by matching the begining of the string only.
 */

typedef struct _MimeType {
	const char* mimetype; 
	const char* playlist;
	const char* extension;
} MimeType;

static const MimeType MIMES[] = {
	/*
	 * Audio files
	 */
	{ "audio/mpeg",				"m3u",	"mp3"	},
	{ "audio/vnd.rn-realaudio",		"ram",	"ram"	},
	{ "audio/x-pn-realaudio",		"ram",	"ram"	}, 
	// matches also "audio/x-pn-realaudio-plugin"
	{ "audio/x-realaudio",			"ram",	"ram"	},
	{ "audio/basic",			"m3u",	"au"	},
	{ "audio/prs.sid",			"m3u",	"sid"	},
	{ "audio/x-scpls",			NULL,	"pls"	},
	{ "audio/x-mpegurl",			NULL,	"m3u"	},
	{ "audio/x-matroska",			"m3u",	"mka"	},
	// Default for all other audio files : x-aac, x-ac3, x-ogg, wav, ...
	{ "audio/",				"m3u",	NULL	},

	/*
	 * Video files
	 */
	{ "video/vnd.rn-realvideo",		"ram",	"ram"	},
	{ "video/x-msvideo",			"m3u",	"avi"	},
	{ "video/x-motion-jpeg",		"m3u",	"mjpg"	},
	{ "video/quicktime",			"m3u",	"mov"	},
	{ "video/x-matroska",			"m3u",	"mkv"	},
	{ "video/mpeg",				"m3u",	"mpg"	},
	{ "video/mp2p",				"m3u",	"vob"	},
	// Default for all other video files : asf, mpeg2, x-ms-wmv, ...
	{ "video/",				"m3u",	NULL	},

	/*
	 * Image files
	 */
	{ "image/jpeg",				NULL,	"jpg"	},
	{ "image/svg+xml",			NULL,	"svg"	},
	{ "image/x-xwindowdump",		NULL,	"xwd"	},
	{ "image/x-win-bitmap",			NULL,	"cur"	},
	{ "image/x-portable-anymap",		NULL,	"pnm"	},
	{ "image/x-portable-bitmap",		NULL,	"pbm"	},
	{ "image/x-portable-pixmap",		NULL,	"ppm"	},
	{ "image/x-portable-graymap",		NULL,	"pgm"	},
	{ "image/x-xpixmap",			NULL,	"xpm"	},
	{ "image/x-xbitmap",			NULL,	"xbm"	},
	{ "image/x-photo-cd",			NULL,	"pcd"	},
	{ "image/x-quicktime",			NULL,	"qti"	}, 
	{ "image/x-icon",			NULL,	"ico"	}, 
	{ "image/tiff",				NULL,	"tif"	},
	// Default for all other image files : bmp, gif, png, ...
	{ "image/",				NULL,	NULL	},

	/*
	 * Multimedia files
	 */
	{ "application/ogg",			"m3u",	"ogg"	},
	{ "application/vnd.rn-realmedia",	"ram",	"ram"	},
	{ "application/x-matroska",		"m3u",	"mkv"	},

	/*
	 * Text files (e.g. subtitles)
	 */
	{ "text/plain",				NULL,	"txt"	},
	// Default for all text files : sub, idx, ssa, ifo, ...
	{ "text/",				NULL,	NULL	},

	{ NULL,					NULL,	NULL	}
};


/******************************************************************************
 * MediaFile_GetPreferred
 *****************************************************************************/
bool
MediaFile_GetPreferred (const DIDLObject* const o, MediaFile* file)
{
	if (o == NULL)
		return false; // ---------->

	IXML_NodeList* const reslist = 
		ixmlElement_getElementsByTagName (o->element, "res");
	if (reslist == NULL)
		return false; // ---------->

	int i;
	// Loop until first result
	bool found = false;
	for (i = 0; i < ixmlNodeList_length (reslist) && !found; i++) {
		IXML_Element* const res = 
			(IXML_Element*) ixmlNodeList_item (reslist, i);
		
		const char* const protocol = 
			ixmlElement_getAttribute (res, "protocolInfo");
		const char* const uri = XMLUtil_GetElementValue (res);
		char mimetype [64] = "";
		if (uri == NULL || protocol == NULL || 
		    sscanf (protocol, "http-get:*:%63[^:;]", mimetype) != 1) 
			continue; // ---------->
			
		const MimeType* format = MIMES;
		while (format->mimetype != NULL && !found) {
			if (strncmp (mimetype, format->mimetype, 
				     strlen (format->mimetype)) == 0) {
				*file = (MediaFile) {
					.o         = o,
					.playlist  = format->playlist,
					.uri       = uri,
					.res       = res
				};
				// generic guess of file extension if not
				// in the list : use the MIME subtype, 
				// without any "*-" prefix. 
				const char* ext = format->extension;
				if (ext == NULL) {
					ext = mimetype + strlen (mimetype);
					// loop safely because it is guaranteed
					// that mimetype has at least '/' ...
					do {
						ext--;
					} while (*ext != '/' && *ext != '-');
					ext++;
				}
				strncpy (file->extension, ext,
					 sizeof (file->extension)-1);
				file->extension [sizeof (file->extension)-1] =
					'\0';
				found = true;
			}
			format++;
		}
	}
	ixmlNodeList_free (reslist);
	return found;
}


/******************************************************************************
 * MediaFile_GetName
 *****************************************************************************/
char*
MediaFile_GetName (void* result_context, 
		   const DIDLObject* o, const char* extension)
{
	char* name = NULL;
	if (o && o->basename) {
		// Append extension only if not already present
		if (extension && *extension) {
			const char* ptr = strrchr (o->basename, '.');
			if (ptr == NULL || strcmp (ptr+1, extension) != 0)
				name = talloc_asprintf (result_context, 
							"%s.%s", 
							o->basename, 
							extension);
		}
		if (name == NULL)
			name = talloc_strdup (result_context, o->basename);
	}
	return name;
}


/******************************************************************************
 * MediaFile_GetPlaylistContent
 *****************************************************************************/

char*
MediaFile_GetPlaylistContent (const MediaFile* const file,
			      void* result_context)
{
	if (file == NULL)
		return NULL; // ---------->

	char* str = NULL;
	
	/*
	 * See description of various playlist formats at:
	 * http://gonze.com/playlists/playlist-format-survey.html
	 */
	if (strcmp (file->playlist, "ram") == 0) {
		/*
		 * 1) "RAM" playlist - Real Audio content
		 */
		str = talloc_asprintf (result_context, "%s?title=%s\n", 
				       file->uri, file->o->title);

	} else if (strcmp (file->playlist, "m3u") == 0) {
		/*
		 * 2) "M3U" playlist - Winamp, MP3, ... 
		 *     and default for all audio files 
		 */
		const char* const duration = 
			ixmlElement_getAttribute (file->res, "duration");
		int seconds = -1;
		if (duration) {
			int hh = 0;
			unsigned int mm = 0, ss = 0;
			if (sscanf (duration, "%d:%u:%u", &hh, &mm, &ss) == 3 
			    && hh >= 0)
				seconds = ss + 60*(mm + 60*hh);
		}
		str = talloc_asprintf (result_context,
				       "#EXTM3U\n"
				       "#EXTINF:%d,%s\n"
				       "%s\n", seconds, 
				       file->o->title, file->uri);
	}
	return str;
}


/******************************************************************************
 * MediaFile_GetResSize
 *****************************************************************************/

off_t
MediaFile_GetResSize (const MediaFile* const file)
{
	const char* const str = ixmlElement_getAttribute (file->res, "size");
	off_t res;
	STRING_TO_INT (str, res, -1);
	return res;
}


