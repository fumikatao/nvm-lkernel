/*
 * BRIEF DESCRIPTION
 *
 * Extended attributes block descriptors tree.
 *
 * Copyright 2010-2011 Marco Stornelli <marco.stornelli@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/spinlock.h>
#include "desctree.h"
#include "pram.h"

/* xblock_desc_init_always()
 *
 * These are initializations that need to be done on every
 * descriptor allocation as the fields are not initialised
 * by slab allocation.
 */
void xblock_desc_init_always(struct pram_xblock_desc *desc)
{
	atomic_set(&desc->refcount, 0);
	desc->blocknr = 0;
	desc->flags = 0;
}

/* xblock_desc_init_once()
 *
 * These are initializations that only need to be done
 * once, because the fields are idempotent across use
 * of the descriptor, so let the slab aware of that.
 */
void xblock_desc_init_once(struct pram_xblock_desc *desc)
{
	mutex_init(&desc->lock);
}

/* __insert_xblock_desc()
 *
 * Insert a new descriptor in the tree.
 */
static void __insert_xblock_desc(struct pram_sb_info *sbi,
				 unsigned long blocknr, struct rb_node *node)
{
	struct rb_node **p = &(sbi->desc_tree.rb_node);
	struct rb_node *parent = NULL;
	struct pram_xblock_desc *desc;

	while (*p) {
		parent = *p;
		desc = rb_entry(parent, struct pram_xblock_desc, node);

		if (blocknr < desc->blocknr)
			p = &(*p)->rb_left;
		else if (blocknr > desc->blocknr)
			p = &(*p)->rb_right;
		else
			/* Oops...an other descriptor for the same block ? */
			BUG();
	}

	rb_link_node(node, parent, p);
	rb_insert_color(node, &sbi->desc_tree);
}

void insert_xblock_desc(struct pram_sb_info *sbi, struct pram_xblock_desc *desc)
{
	spin_lock(&sbi->desc_tree_lock);
	__insert_xblock_desc(sbi, desc->blocknr, &desc->node);
	spin_unlock(&sbi->desc_tree_lock);
};

/* __lookup_xblock_desc()
 *
 * Search an extended attribute descriptor in the tree via the
 * block number. It returns the descriptor if it's found or
 * NULL. If not found it creates a new descriptor if create is not 0.
 */
static struct pram_xblock_desc *__lookup_xblock_desc(struct pram_sb_info *sbi,
					    unsigned long blocknr,
					    struct kmem_cache *cache,
					    int create)
{
	struct rb_node *n = sbi->desc_tree.rb_node;
	struct pram_xblock_desc *desc = NULL;

	while (n) {
		desc = rb_entry(n, struct pram_xblock_desc, node);

		if (blocknr < desc->blocknr)
			n = n->rb_left;
		else if (blocknr > desc->blocknr)
			n = n->rb_right;
		else {
			atomic_inc(&desc->refcount);
			goto out;
		}
	}

	/* not found */
	if (create) {
		desc = kmem_cache_alloc(cache, GFP_NOFS);
		if (!desc)
			return ERR_PTR(-ENOMEM);
		xblock_desc_init_always(desc);
		atomic_set(&desc->refcount, 1);
		desc->blocknr = blocknr;
		__insert_xblock_desc(sbi, desc->blocknr, &desc->node);
	}
out:
	return desc;
}

struct pram_xblock_desc *lookup_xblock_desc(struct pram_sb_info *sbi,
					    unsigned long blocknr,
					    struct kmem_cache *cache,
					    int create)
{
	struct pram_xblock_desc *desc = NULL;

	spin_lock(&sbi->desc_tree_lock);
	desc = __lookup_xblock_desc(sbi, blocknr, cache, create);
	spin_unlock(&sbi->desc_tree_lock);
	return desc;
}

/* put_xblock_desc()
 *
 * Decrement the reference count and if it reaches zero and the
 * desciptor has been marked to be free, then we free it.
 * It returns 0 if the descriptor has been deleted and 1 otherwise.
 */
int put_xblock_desc(struct pram_sb_info *sbi, struct pram_xblock_desc *desc)
{
	int ret = 1;
	if (!desc)
		return ret;

	if (atomic_dec_and_lock(&desc->refcount, &sbi->desc_tree_lock)) {
		if (test_bit(FREEING, &desc->flags)) {
			rb_erase(&desc->node, &sbi->desc_tree);
			pram_dbg("erasing desc for block %lu\n", desc->blocknr);
			ret = 0;
		}
		spin_unlock(&sbi->desc_tree_lock);
	}
	return ret;
};

/* mark_free_desc()
 *
 * Mark free a descriptor. The descriptor will be deleted later in the
 * put_xblock_desc().
 */
void mark_free_desc(struct pram_xblock_desc *desc)
{
	set_bit(FREEING, &desc->flags);
}

/* erase_tree()
 *
 * Free all objects in the tree.
 */
void erase_tree(struct pram_sb_info *sbi, struct kmem_cache *cachep)
{
	struct rb_node *n;
	struct pram_xblock_desc *desc;

	spin_lock(&sbi->desc_tree_lock);
	n = rb_first(&sbi->desc_tree);
	while (n) {
		desc = rb_entry(n, struct pram_xblock_desc, node);
		rb_erase(n, &sbi->desc_tree);
		kmem_cache_free(cachep, desc);
		n = rb_next(n);
	}
	spin_unlock(&sbi->desc_tree_lock);
}
