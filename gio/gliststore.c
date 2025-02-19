/*
 * Copyright 2015 Lars Uebernickel
 * Copyright 2015 Ryan Lortie
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Lars Uebernickel <lars@uebernic.de>
 *     Ryan Lortie <desrt@desrt.ca>
 */

#include "config.h"

#include "gliststore.h"
#include "glistmodel.h"

/**
 * SECTION:gliststore
 * @title: GListStore
 * @short_description: A simple implementation of #GListModel
 * @include: gio/gio.h
 *
 * #GListStore is a simple implementation of #GListModel that stores all
 * items in memory.
 *
 * It provides insertions, deletions, and lookups in logarithmic time
 * with a fast path for the common case of iterating the list linearly.
 */

/**
 * GListStore:
 *
 * #GListStore is an opaque data structure and can only be accessed
 * using the following functions.
 **/

struct _GListStore
{
  GObject parent_instance;

  GType item_type;
  GSequence *items;

  /* cache */
  guint last_position;
  GSequenceIter *last_iter;
  gboolean last_position_valid;
};

enum
{
  PROP_0,
  PROP_ITEM_TYPE,
  PROP_N_ITEMS,
  N_PROPERTIES
};

static void g_list_store_iface_init (GListModelInterface *iface,
                                     gpointer             iface_data);

G_DEFINE_TYPE_WITH_CODE (GListStore, g_list_store, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_LIST_MODEL, g_list_store_iface_init));

static GParamSpec *properties[N_PROPERTIES] = { NULL, };

static void
g_list_store_items_changed (GListStore *store,
                            guint       position,
                            guint       removed,
                            guint       added)
{
  /* check if the iter cache may have been invalidated */
  if (position <= store->last_position)
    {
      store->last_iter = NULL;
      store->last_position = 0;
      store->last_position_valid = FALSE;
    }

  g_list_model_items_changed (G_LIST_MODEL (store), position, removed, added);
  if (removed != added)
    g_object_notify_by_pspec (G_OBJECT (store), properties[PROP_N_ITEMS]);
}

static void
g_list_store_dispose (GObject *object)
{
  GListStore *store = G_LIST_STORE (object);

  g_clear_pointer (&store->items, g_sequence_free);

  G_OBJECT_CLASS (g_list_store_parent_class)->dispose (object);
}

static void
g_list_store_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GListStore *store = G_LIST_STORE (object);

  switch (property_id)
    {
    case PROP_ITEM_TYPE:
      g_value_set_gtype (value, store->item_type);
      break;

    case PROP_N_ITEMS:
      g_value_set_uint (value, g_sequence_get_length (store->items));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
g_list_store_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  GListStore *store = G_LIST_STORE (object);

  switch (property_id)
    {
    case PROP_ITEM_TYPE: /* construct-only */
      g_assert (g_type_is_a (g_value_get_gtype (value), G_TYPE_OBJECT));
      store->item_type = g_value_get_gtype (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
g_list_store_class_init (GListStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = g_list_store_dispose;
  object_class->get_property = g_list_store_get_property;
  object_class->set_property = g_list_store_set_property;

  /**
   * GListStore:item-type:
   *
   * The type of items contained in this list store. Items must be
   * subclasses of #GObject.
   *
   * Since: 2.44
   **/
  properties[PROP_ITEM_TYPE] =
    g_param_spec_gtype ("item-type", "", "", G_TYPE_OBJECT,
                        G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /**
   * GListStore:n-items:
   *
   * The number of items contained in this list store.
   *
   * Since: 2.74
   **/
  properties[PROP_N_ITEMS] =
    g_param_spec_uint ("n-items", "", "", 0, G_MAXUINT, 0,
                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPERTIES, properties);
}

static GType
g_list_store_get_item_type (GListModel *list)
{
  GListStore *store = G_LIST_STORE (list);

  return store->item_type;
}

static guint
g_list_store_get_n_items (GListModel *list)
{
  GListStore *store = G_LIST_STORE (list);

  return g_sequence_get_length (store->items);
}

static gpointer
g_list_store_get_item (GListModel *list,
                       guint       position)
{
  GListStore *store = G_LIST_STORE (list);
  GSequenceIter *it = NULL;

  if (store->last_position_valid)
    {
      if (position < G_MAXUINT && store->last_position == position + 1)
        it = g_sequence_iter_prev (store->last_iter);
      else if (position > 0 && store->last_position == position - 1)
        it = g_sequence_iter_next (store->last_iter);
      else if (store->last_position == position)
        it = store->last_iter;
    }

  if (it == NULL)
    it = g_sequence_get_iter_at_pos (store->items, position);

  store->last_iter = it;
  store->last_position = position;
  store->last_position_valid = TRUE;

  if (g_sequence_iter_is_end (it))
    return NULL;
  else
    return g_object_ref (g_sequence_get (it));
}

static void
g_list_store_iface_init (GListModelInterface *iface,
                         gpointer             iface_data)
{
  iface->get_item_type = g_list_store_get_item_type;
  iface->get_n_items = g_list_store_get_n_items;
  iface->get_item = g_list_store_get_item;
}

static void
g_list_store_init (GListStore *store)
{
  store->items = g_sequence_new (g_object_unref);
  store->last_position = 0;
  store->last_position_valid = FALSE;
}

/**
 * g_list_store_new:
 * @item_type: the #GType of items in the list
 *
 * Creates a new #GListStore with items of type @item_type. @item_type
 * must be a subclass of #GObject.
 *
 * Returns: a new #GListStore
 * Since: 2.44
 */
GListStore *
g_list_store_new (GType item_type)
{
  /* We only allow GObjects as item types right now. This might change
   * in the future.
   */
  g_return_val_if_fail (g_type_is_a (item_type, G_TYPE_OBJECT), NULL);

  return g_object_new (G_TYPE_LIST_STORE,
                       "item-type", item_type,
                       NULL);
}

/**
 * g_list_store_insert:
 * @store: a #GListStore
 * @position: the position at which to insert the new item
 * @item: (type GObject): the new item
 *
 * Inserts @item into @store at @position. @item must be of type
 * #GListStore:item-type or derived from it. @position must be smaller
 * than the length of the list, or equal to it to append.
 *
 * This function takes a ref on @item.
 *
 * Use g_list_store_splice() to insert multiple items at the same time
 * efficiently.
 *
 * Since: 2.44
 */
void
g_list_store_insert (GListStore *store,
                     guint       position,
                     gpointer    item)
{
  GSequenceIter *it;

  g_return_if_fail (G_IS_LIST_STORE (store));
  g_return_if_fail (g_type_is_a (G_OBJECT_TYPE (item), store->item_type));
  g_return_if_fail (position <= (guint) g_sequence_get_length (store->items));

  it = g_sequence_get_iter_at_pos (store->items, position);
  g_sequence_insert_before (it, g_object_ref (item));

  g_list_store_items_changed (store, position, 0, 1);
}

/**
 * g_list_store_insert_sorted:
 * @store: a #GListStore
 * @item: (type GObject): the new item
 * @compare_func: (scope call): pairwise comparison function for sorting
 * @user_data: (closure): user data for @compare_func
 *
 * Inserts @item into @store at a position to be determined by the
 * @compare_func.
 *
 * The list must already be sorted before calling this function or the
 * result is undefined.  Usually you would approach this by only ever
 * inserting items by way of this function.
 *
 * This function takes a ref on @item.
 *
 * Returns: the position at which @item was inserted
 *
 * Since: 2.44
 */
guint
g_list_store_insert_sorted (GListStore       *store,
                            gpointer          item,
                            GCompareDataFunc  compare_func,
                            gpointer          user_data)
{
  GSequenceIter *it;
  guint position;

  g_return_val_if_fail (G_IS_LIST_STORE (store), 0);
  g_return_val_if_fail (g_type_is_a (G_OBJECT_TYPE (item), store->item_type), 0);
  g_return_val_if_fail (compare_func != NULL, 0);

  it = g_sequence_insert_sorted (store->items, g_object_ref (item), compare_func, user_data);
  position = g_sequence_iter_get_position (it);

  g_list_store_items_changed (store, position, 0, 1);

  return position;
}

/**
 * g_list_store_sort:
 * @store: a #GListStore
 * @compare_func: (scope call): pairwise comparison function for sorting
 * @user_data: (closure): user data for @compare_func
 *
 * Sort the items in @store according to @compare_func.
 *
 * Since: 2.46
 */
void
g_list_store_sort (GListStore       *store,
                   GCompareDataFunc  compare_func,
                   gpointer          user_data)
{
  gint n_items;

  g_return_if_fail (G_IS_LIST_STORE (store));
  g_return_if_fail (compare_func != NULL);

  g_sequence_sort (store->items, compare_func, user_data);

  n_items = g_sequence_get_length (store->items);
  g_list_store_items_changed (store, 0, n_items, n_items);
}

/**
 * g_list_store_append:
 * @store: a #GListStore
 * @item: (type GObject): the new item
 *
 * Appends @item to @store. @item must be of type #GListStore:item-type.
 *
 * This function takes a ref on @item.
 *
 * Use g_list_store_splice() to append multiple items at the same time
 * efficiently.
 *
 * Since: 2.44
 */
void
g_list_store_append (GListStore *store,
                     gpointer    item)
{
  guint n_items;

  g_return_if_fail (G_IS_LIST_STORE (store));
  g_return_if_fail (g_type_is_a (G_OBJECT_TYPE (item), store->item_type));

  n_items = g_sequence_get_length (store->items);
  g_sequence_append (store->items, g_object_ref (item));

  g_list_store_items_changed (store, n_items, 0, 1);
}

/**
 * g_list_store_remove:
 * @store: a #GListStore
 * @position: the position of the item that is to be removed
 *
 * Removes the item from @store that is at @position. @position must be
 * smaller than the current length of the list.
 *
 * Use g_list_store_splice() to remove multiple items at the same time
 * efficiently.
 *
 * Since: 2.44
 */
void
g_list_store_remove (GListStore *store,
                     guint       position)
{
  GSequenceIter *it;

  g_return_if_fail (G_IS_LIST_STORE (store));

  it = g_sequence_get_iter_at_pos (store->items, position);
  g_return_if_fail (!g_sequence_iter_is_end (it));

  g_sequence_remove (it);
  g_list_store_items_changed (store, position, 1, 0);
}

/**
 * g_list_store_remove_all:
 * @store: a #GListStore
 *
 * Removes all items from @store.
 *
 * Since: 2.44
 */
void
g_list_store_remove_all (GListStore *store)
{
  guint n_items;

  g_return_if_fail (G_IS_LIST_STORE (store));

  n_items = g_sequence_get_length (store->items);
  g_sequence_remove_range (g_sequence_get_begin_iter (store->items),
                           g_sequence_get_end_iter (store->items));

  g_list_store_items_changed (store, 0, n_items, 0);
}

/**
 * g_list_store_splice:
 * @store: a #GListStore
 * @position: the position at which to make the change
 * @n_removals: the number of items to remove
 * @additions: (array length=n_additions) (element-type GObject): the items to add
 * @n_additions: the number of items to add
 *
 * Changes @store by removing @n_removals items and adding @n_additions
 * items to it. @additions must contain @n_additions items of type
 * #GListStore:item-type.  %NULL is not permitted.
 *
 * This function is more efficient than g_list_store_insert() and
 * g_list_store_remove(), because it only emits
 * #GListModel::items-changed once for the change.
 *
 * This function takes a ref on each item in @additions.
 *
 * The parameters @position and @n_removals must be correct (ie:
 * @position + @n_removals must be less than or equal to the length of
 * the list at the time this function is called).
 *
 * Since: 2.44
 */
void
g_list_store_splice (GListStore *store,
                     guint       position,
                     guint       n_removals,
                     gpointer   *additions,
                     guint       n_additions)
{
  GSequenceIter *it;
  guint n_items;

  g_return_if_fail (G_IS_LIST_STORE (store));
  g_return_if_fail (position + n_removals >= position); /* overflow */

  n_items = g_sequence_get_length (store->items);
  g_return_if_fail (position + n_removals <= n_items);

  it = g_sequence_get_iter_at_pos (store->items, position);

  if (n_removals)
    {
      GSequenceIter *end;

      end = g_sequence_iter_move (it, n_removals);
      g_sequence_remove_range (it, end);

      it = end;
    }

  if (n_additions)
    {
      guint i;

      for (i = 0; i < n_additions; i++)
        {
          if G_UNLIKELY (!g_type_is_a (G_OBJECT_TYPE (additions[i]), store->item_type))
            {
              g_critical ("%s: item %d is a %s instead of a %s.  GListStore is now in an undefined state.",
                          G_STRFUNC, i, G_OBJECT_TYPE_NAME (additions[i]), g_type_name (store->item_type));
              return;
            }

          g_sequence_insert_before (it, g_object_ref (additions[i]));
        }
    }

  g_list_store_items_changed (store, position, n_removals, n_additions);
}

static gboolean
simple_equal (gconstpointer a,
              gconstpointer b,
              gpointer equal_func)
{
  return ((GEqualFunc) equal_func) (a, b);
}

/**
 * g_list_store_find_with_equal_func:
 * @store: a #GListStore
 * @item: (type GObject): an item
 * @equal_func: (scope call): A custom equality check function
 * @position: (out) (optional): the first position of @item, if it was found.
 *
 * Looks up the given @item in the list store by looping over the items and
 * comparing them with @equal_func until the first occurrence of @item which
 * matches. If @item was not found, then @position will not be set, and this
 * method will return %FALSE.
 *
 * Returns: Whether @store contains @item. If it was found, @position will be
 * set to the position where @item occurred for the first time.
 *
 * Since: 2.64
 */
gboolean
g_list_store_find_with_equal_func (GListStore *store,
                                   gpointer    item,
                                   GEqualFunc  equal_func,
                                   guint      *position)
{
  g_return_val_if_fail (equal_func != NULL, FALSE);

  return g_list_store_find_with_equal_func_full (store, item, simple_equal,
                                                 equal_func, position);
}

/**
 * g_list_store_find_with_equal_func_full:
 * @store: a #GListStore
 * @item: (type GObject): an item
 * @equal_func: (scope call): A custom equality check function
 * @user_data: (closure): user data for @equal_func
 * @position: (out) (optional): the first position of @item, if it was found.
 *
 * Like g_list_store_find_with_equal_func() but with an additional @user_data
 * that is passed to @equal_func.
 *
 * Returns: Whether @store contains @item. If it was found, @position will be
 * set to the position where @item occurred for the first time.
 *
 * Since: 2.74
 */
gboolean
g_list_store_find_with_equal_func_full (GListStore     *store,
                                        gpointer        item,
                                        GEqualFuncFull  equal_func,
                                        gpointer        user_data,
                                        guint          *position)
{
  GSequenceIter *iter, *begin, *end;

  g_return_val_if_fail (G_IS_LIST_STORE (store), FALSE);
  g_return_val_if_fail (g_type_is_a (G_OBJECT_TYPE (item), store->item_type),
                        FALSE);
  g_return_val_if_fail (equal_func != NULL, FALSE);

  /* NOTE: We can't use g_sequence_lookup() or g_sequence_search(), because we
   * can't assume the sequence is sorted. */
  begin = g_sequence_get_begin_iter (store->items);
  end = g_sequence_get_end_iter (store->items);

  iter = begin;
  while (iter != end)
    {
      gpointer iter_item;

      iter_item = g_sequence_get (iter);
      if (equal_func (iter_item, item, user_data))
        {
          if (position)
            *position = g_sequence_iter_get_position (iter);
          return TRUE;
        }

      iter = g_sequence_iter_next (iter);
    }

  return FALSE;
}

/**
 * g_list_store_find:
 * @store: a #GListStore
 * @item: (type GObject): an item
 * @position: (out) (optional): the first position of @item, if it was found.
 *
 * Looks up the given @item in the list store by looping over the items until
 * the first occurrence of @item. If @item was not found, then @position will
 * not be set, and this method will return %FALSE.
 *
 * If you need to compare the two items with a custom comparison function, use
 * g_list_store_find_with_equal_func() with a custom #GEqualFunc instead.
 *
 * Returns: Whether @store contains @item. If it was found, @position will be
 * set to the position where @item occurred for the first time.
 *
 * Since: 2.64
 */
gboolean
g_list_store_find (GListStore *store,
                   gpointer    item,
                   guint      *position)
{
  return g_list_store_find_with_equal_func (store,
                                            item,
                                            g_direct_equal,
                                            position);
}
