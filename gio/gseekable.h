/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_SEEKABLE_H__
#define __G_SEEKABLE_H__

#include <glib-object.h>
#include <gio/gcancellable.h>

G_BEGIN_DECLS

#define G_TYPE_SEEKABLE            (g_seekable_get_type ())
#define G_SEEKABLE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_SEEKABLE, GSeekable))
#define G_IS_SEEKABLE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_SEEKABLE))
#define G_SEEKABLE_GET_IFACE(obj)  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_SEEKABLE, GSeekableIface))

typedef struct _GSeekable        GSeekable;
typedef struct _GSeekableIface   GSeekableIface;

struct _GSeekableIface
{
  GTypeInterface g_iface;

  /* Virtual Table */
  
  goffset     (* tell)	         (GSeekable    *seekable);
  
  gboolean    (* can_seek)       (GSeekable    *seekable);
  gboolean    (* seek)	         (GSeekable    *seekable,
				  goffset       offset,
				  GSeekType     type,
				  GCancellable *cancellable,
				  GError      **error);
  
  gboolean    (* can_truncate)   (GSeekable    *seekable);
  gboolean    (* truncate)       (GSeekable    *seekable,
				  goffset       offset,
				  GCancellable *cancellable,
				  GError       **error);

  /* TODO: Async seek/truncate */
};

GType g_seekable_get_type (void) G_GNUC_CONST;

goffset  g_seekable_tell         (GSeekable     *seekable);
gboolean g_seekable_can_seek     (GSeekable     *seekable);
gboolean g_seekable_seek         (GSeekable     *seekable,
				  goffset        offset,
				  GSeekType      type,
				  GCancellable  *cancellable,
				  GError       **error);
gboolean g_seekable_can_truncate (GSeekable     *seekable);
gboolean g_seekable_truncate     (GSeekable     *seekable,
				  goffset        offset,
				  GCancellable  *cancellable,
				  GError       **error);

G_END_DECLS


#endif /* __G_SEEKABLE_H__ */