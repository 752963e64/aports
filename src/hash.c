/* hash.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2005-2008 Natanael Copa <n@tanael.org>
 * Copyright (C) 2008 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation. See http://www.gnu.org/ for details.
 */

#include "apk_defines.h"
#include "apk_hash.h"

void apk_hash_init(struct apk_hash *h, const struct apk_hash_ops *ops,
		   int num_buckets)
{
	h->ops = ops;
	h->buckets = apk_hash_array_resize(NULL, num_buckets);
	h->num_items = 0;
}

void apk_hash_free(struct apk_hash *h)
{
	apk_hash_foreach(h, (apk_hash_enumerator_f) h->ops->delete_item, NULL);
	free(h->buckets);
}

int apk_hash_foreach(struct apk_hash *h, apk_hash_enumerator_f e, void *ctx)
{
	apk_hash_node *pos, *n;
	ptrdiff_t offset = h->ops->node_offset;
	int i, r;

	for (i = 0; i < h->buckets->num; i++) {
		hlist_for_each_safe(pos, n, &h->buckets->item[i]) {
			r = e(((void *) pos) - offset, ctx);
			if (r != 0 && ctx != NULL)
				return r;
		}
	}

	return 0;
}

apk_hash_item apk_hash_get(struct apk_hash *h, apk_blob_t key)
{
	ptrdiff_t offset = h->ops->node_offset;
	unsigned long hash;
	apk_hash_node *pos;
	apk_hash_item item;
	apk_blob_t itemkey;

	hash = h->ops->hash_key(key) % h->buckets->num;
	if (h->ops->compare_item != NULL) {
		hlist_for_each(pos, &h->buckets->item[hash]) {
			item = ((void *) pos) - offset;
			if (h->ops->compare_item(item, key) == 0)
				return item;
		}
	} else {
		hlist_for_each(pos, &h->buckets->item[hash]) {
			item = ((void *) pos) - offset;
			itemkey = h->ops->get_key(item);
			if (h->ops->compare(key, itemkey) == 0)
				return item;
		}
	}

	return NULL;
}

void apk_hash_insert(struct apk_hash *h, apk_hash_item item)
{
	unsigned long hash;
	apk_hash_node *node;

	if (h->ops->hash_item == NULL)
		hash = h->ops->hash_key(h->ops->get_key(item));
	else
		hash = h->ops->hash_item(item);
	hash %= h->buckets->num;
	node = (apk_hash_node *) (item + h->ops->node_offset);
	hlist_add_head(node, &h->buckets->item[hash]);
	h->num_items++;
}

void apk_hash_delete(struct apk_hash *h, apk_blob_t key)
{
}

