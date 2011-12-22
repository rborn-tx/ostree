/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2011 Colin Walters <walters@verbum.org>
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ostree-mutable-tree.h"

struct _OstreeMutableTree
{
  GObject parent_instance;

  char *contents_checksum;
  char *metadata_checksum;

  GHashTable *files;
  GHashTable *subdirs;
};

G_DEFINE_TYPE (OstreeMutableTree, ostree_mutable_tree, G_TYPE_OBJECT)

static void
ostree_mutable_tree_finalize (GObject *object)
{
  OstreeMutableTree *self;

  self = OSTREE_MUTABLE_TREE (object);

  g_free (self->contents_checksum);
  g_free (self->metadata_checksum);

  g_hash_table_destroy (self->files);
  g_hash_table_destroy (self->subdirs);

  G_OBJECT_CLASS (ostree_mutable_tree_parent_class)->finalize (object);
}

static void
ostree_mutable_tree_class_init (OstreeMutableTreeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = ostree_mutable_tree_finalize;
}

static void
ostree_mutable_tree_init (OstreeMutableTree *self)
{
  self->files = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, g_free);
  self->subdirs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, (GDestroyNotify)g_object_unref);
}

void
ostree_mutable_tree_set_metadata_checksum (OstreeMutableTree *self,
                                           const char        *checksum)
{
  g_free (self->metadata_checksum);
  self->metadata_checksum = g_strdup (checksum);
}

const char *
ostree_mutable_tree_get_metadata_checksum (OstreeMutableTree *self)
{
  return self->metadata_checksum;
}

void
ostree_mutable_tree_set_contents_checksum (OstreeMutableTree *self,
                                           const char        *checksum)
{
  g_free (self->contents_checksum);
  self->contents_checksum = g_strdup (checksum);
}

const char *
ostree_mutable_tree_get_contents_checksum (OstreeMutableTree *self)
{
  return self->contents_checksum;
}

static gboolean
set_error_noent (GError **error, const char *path)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               "No such file or directory: %s",
               path);
  return FALSE;
}

gboolean
ostree_mutable_tree_replace_file (OstreeMutableTree *self,
                                  const char        *name,
                                  const char        *checksum,
                                  GError           **error)
{
  gboolean ret = FALSE;

  if (g_hash_table_lookup (self->subdirs, name))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't replace directory with file: %s", name);
      goto out;
    }

  g_hash_table_replace (self->files,
                        g_strdup (name),
                        g_strdup (checksum));

  ret = TRUE;
 out:
  return ret;
}

gboolean
ostree_mutable_tree_ensure_dir (OstreeMutableTree *self,
                                const char        *name,
                                OstreeMutableTree **out_subdir,
                                GError           **error)
{
  gboolean ret = FALSE;
  OstreeMutableTree *ret_dir = NULL;

  g_return_val_if_fail (name != NULL, FALSE);

  if (g_hash_table_lookup (self->files, name))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Can't replace file with directory: %s", name);
      goto out;
    }

  ret_dir = ot_gobject_refz (g_hash_table_lookup (self->subdirs, name));
  if (!ret_dir)
    {
      ret_dir = ostree_mutable_tree_new ();
      g_hash_table_insert (self->subdirs, g_strdup (name), g_object_ref (ret_dir));
    }
  
  ret = TRUE;
  ot_transfer_out_value (out_subdir, &ret_dir);
 out:
  g_clear_object (&ret_dir);
  return ret;
}

gboolean
ostree_mutable_tree_lookup (OstreeMutableTree   *self,
                            const char          *name,
                            char               **out_file_checksum,
                            OstreeMutableTree  **out_subdir,
                            GError             **error)
{
  gboolean ret = FALSE;
  OstreeMutableTree *ret_subdir = NULL;
  char *ret_file_checksum = NULL;
  
  ret_subdir = ot_gobject_refz (g_hash_table_lookup (self->subdirs, name));
  if (!ret_subdir)
    {
      ret_file_checksum = g_strdup (g_hash_table_lookup (self->files, name));
      if (!ret_file_checksum)
        {
          set_error_noent (error, name);
          goto out;
        }
    }

  ret = TRUE;
  ot_transfer_out_value (out_file_checksum, &ret_file_checksum);
  ot_transfer_out_value (out_subdir, &ret_subdir);
 out:
  g_free (ret_file_checksum);
  g_clear_object (&ret_subdir);
  return ret;
}

gboolean
ostree_mutable_tree_walk (OstreeMutableTree     *self,
                          GPtrArray             *split_path,
                          guint                  start,
                          OstreeMutableTree    **out_parent,
                          GError               **error)
{
  if (start >= split_path->len)
    {
      return set_error_noent (error, (char*)split_path->pdata[start]);
    }
  else if (start == split_path->len - 1)
    {
      *out_parent = g_object_ref (self);
      return TRUE;
    }
  else
    {
      OstreeMutableTree *subdir;

      subdir = g_hash_table_lookup (self->subdirs, split_path->pdata[start]);
      if (!subdir)
        return set_error_noent (error, (char*)split_path->pdata[start]);

      return ostree_mutable_tree_walk (subdir, split_path, start + 1, out_parent, error);
    }
}

GHashTable *
ostree_mutable_tree_get_subdirs (OstreeMutableTree *self)
{
  return self->subdirs;
}

GHashTable *
ostree_mutable_tree_get_files (OstreeMutableTree *self)
{
  return self->files;
}

OstreeMutableTree *
ostree_mutable_tree_new (void)
{
  return (OstreeMutableTree*)g_object_new (OSTREE_TYPE_MUTABLE_TREE, NULL);
}
