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
 * Author: Vlad Grecescu <b100dian@gmail.com>
 * 
 */

#include "config.h"
#include "gwin32directorymonitor.h"
#include "giomodule.h"
#include <windows.h>

G_DEFINE_TYPE_WITH_CODE (GWin32DirectoryMonitor, g_win32_directory_monitor, G_TYPE_LOCAL_DIRECTORY_MONITOR,
g_io_extension_point_implement (G_LOCAL_DIRECTORY_MONITOR_EXTENSION_POINT_NAME,
							 g_define_type_id,
							 "readdirectorychanges",
							 20))

struct _GWin32DirectoryMonitorPrivate {
	OVERLAPPED overlapped;
	DWORD buffer_allocated_bytes;
	gchar* file_notify_buffer;
	DWORD buffer_filled_bytes;
	HANDLE hDirectory;
	/** needed in the APC where we only have this private struct */
	GFileMonitor * self;
};

static void g_win32_directory_monitor_finalize (GObject* base);
static gboolean g_win32_directory_monitor_cancel (GFileMonitor* base);
static GObject * g_win32_directory_monitor_constructor (GType type, guint n_construct_properties, GObjectConstructParam * construct_properties);

static gboolean g_win32_directory_monitor_is_supported(void) {
	return TRUE;
}

static void g_win32_directory_monitor_finalize (GObject* base) {
	GWin32DirectoryMonitor * self;
	self = G_WIN32_DIRECTORY_MONITOR (base);

	g_free (self->priv->file_notify_buffer);
	g_free (self->priv);
	
	if (G_OBJECT_CLASS (g_win32_directory_monitor_parent_class)->finalize)
		(*G_OBJECT_CLASS (g_win32_directory_monitor_parent_class)->finalize) (base);
}


static gboolean g_win32_directory_monitor_cancel (GFileMonitor* base) {
	GWin32DirectoryMonitor * self;
	self = G_WIN32_DIRECTORY_MONITOR (base);
	
	/* this triggers a last callback() with nBytes=0 */ 
	CloseHandle (self->priv->hDirectory);

	if (G_FILE_MONITOR_CLASS (g_win32_directory_monitor_parent_class)->cancel)
    	(*G_FILE_MONITOR_CLASS (g_win32_directory_monitor_parent_class)->cancel) (base);
	return TRUE;
}

void CALLBACK g_win32_directory_monitor_callback (DWORD error, DWORD nBytes, GWin32DirectoryMonitorPrivate* lpOverlapped)
{
	gulong offset;
	PFILE_NOTIFY_INFORMATION pfile_notify_walker;
	gulong file_name_len;
	gchar* file_name;
	GFile * file;

	static GFileMonitorEvent events[] = {0, 
		G_FILE_MONITOR_EVENT_CREATED, /* FILE_ACTION_ADDED            */
		G_FILE_MONITOR_EVENT_DELETED, /* FILE_ACTION_REMOVED          */
		G_FILE_MONITOR_EVENT_CHANGED, /* FILE_ACTION_MODIFIED         */
		G_FILE_MONITOR_EVENT_DELETED, /* FILE_ACTION_RENAMED_OLD_NAME */
		G_FILE_MONITOR_EVENT_CREATED, /* FILE_ACTION_RENAMED_NEW_NAME */
	};

	if (!nBytes) /* monitor was cancelled/finalized */
		return;
	
	if (g_file_monitor_is_cancelled (G_FILE_MONITOR (lpOverlapped->self)))
		return; /* and ReadDirectoryChangesW doesn't get called this time */

	offset = 0;
	do {
		pfile_notify_walker = (PFILE_NOTIFY_INFORMATION)(lpOverlapped->file_notify_buffer + offset);
		offset += pfile_notify_walker->NextEntryOffset;
		file_name = g_utf16_to_utf8 (pfile_notify_walker->FileName, pfile_notify_walker->FileNameLength / sizeof(WCHAR), NULL, &file_name_len, NULL);
		file = g_file_new_for_path (file_name);	
		g_file_monitor_emit_event (lpOverlapped->self, file, NULL, events [pfile_notify_walker->Action]);
		g_object_unref (file);
		g_free (file_name);
	} while (pfile_notify_walker->NextEntryOffset);
	
	ReadDirectoryChangesW (lpOverlapped->hDirectory, (gpointer)lpOverlapped->file_notify_buffer, lpOverlapped->buffer_allocated_bytes, FALSE, 
		FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
		FILE_NOTIFY_CHANGE_SIZE, &lpOverlapped->buffer_filled_bytes, &lpOverlapped->overlapped, g_win32_directory_monitor_callback);
}

static GObject * g_win32_directory_monitor_constructor (GType type, guint n_construct_properties, GObjectConstructParam * construct_properties) {
	GObject * obj;
	GWin32DirectoryMonitorClass * klass;
	GObjectClass * parent_class;
	GWin32DirectoryMonitor * self;
	gchar * dirname;
	gboolean result;
	
	klass = G_WIN32_DIRECTORY_MONITOR_CLASS (g_type_class_peek (G_TYPE_WIN32_DIRECTORY_MONITOR));
	parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));
	obj = parent_class->constructor (type, n_construct_properties, construct_properties);
	self = G_WIN32_DIRECTORY_MONITOR (obj);
	dirname = G_LOCAL_DIRECTORY_MONITOR (obj)->dirname;
	
	self->priv->hDirectory = CreateFile (dirname, FILE_LIST_DIRECTORY, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL); 
	g_assert (self->priv->hDirectory != INVALID_HANDLE_VALUE); /* harsh */

	result = ReadDirectoryChangesW (self->priv->hDirectory, (gpointer)self->priv->file_notify_buffer, self->priv->buffer_allocated_bytes, FALSE, 
		FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
		FILE_NOTIFY_CHANGE_SIZE, &self->priv->buffer_filled_bytes, &self->priv->overlapped, g_win32_directory_monitor_callback);
	g_assert (result); /* harsh */

	return obj;
}

static void g_win32_directory_monitor_class_init (GWin32DirectoryMonitorClass * klass) {
	
	g_win32_directory_monitor_parent_class = g_type_class_peek_parent (klass);

	G_OBJECT_CLASS (klass)->constructor = g_win32_directory_monitor_constructor;
	G_OBJECT_CLASS (klass)->finalize = g_win32_directory_monitor_finalize;
	G_FILE_MONITOR_CLASS (klass)->cancel = g_win32_directory_monitor_cancel;
	
	G_LOCAL_DIRECTORY_MONITOR_CLASS (klass)->mount_notify = FALSE;
	G_LOCAL_DIRECTORY_MONITOR_CLASS (klass)->is_supported = g_win32_directory_monitor_is_supported;
}

static void g_win32_directory_monitor_init (GWin32DirectoryMonitor * self) 
{
	self->priv = (GWin32DirectoryMonitorPrivate*)g_new0 (GWin32DirectoryMonitorPrivate, 1);
	g_assert (self->priv != 0);
	
	self->priv->buffer_allocated_bytes = 32768;
	self->priv->file_notify_buffer = g_new0 (gchar, self->priv->buffer_allocated_bytes);
	g_assert (self->priv->file_notify_buffer);
	
	self->priv->self = G_FILE_MONITOR (self);
}