/* 
 * Written by Josh Dybnis and released to the public domain, as explained at
 * http://creativecommons.org/licenses/publicdomain
 *
 * Implementation of the lock-free skiplist data-structure created by Maurice Herlihy, Yossi Lev,
 * and Nir Shavit. See Herlihy's and Shivit's book "The Art of Multiprocessor Programming".
 * http://www.amazon.com/Art-Multiprocessor-Programming-Maurice-Herlihy/dp/0123705916/
 *
 * See also Kir Fraser's dissertation "Practical Lock Freedom".
 * www.cl.cam.ac.uk/techreports/UCAM-CL-TR-579.pdf
 *
 * I've generalized the data structure to support update operations like set() and CAS() in addition to 
 * the normal add() and remove() operations.
 *
 * Warning: This code is written for the x86 memory-model. The algorithim depends on certain stores 
 * and loads being ordered. This code won't work correctly on platforms with weaker memory models if
 * you don't add memory barriers in the right places.
 */

#include <stdio.h>
#include <string.h>

#include "common.h"
#include "skiplist.h"
#include "runtime.h"
#include "mem.h"
#include "rcu.h"

// Setting MAX_LEVEL to 0 essentially makes this data structure the Harris-Michael lock-free list (in list.c).
#define MAX_LEVEL 31

typedef struct node {
    map_key_t key;
    map_val_t val;
    int top_level;
    markable_t next[1];
} node_t;

struct sl_iter {
    node_t *next;
};

struct sl {
    node_t *head;
    const datatype_t *key_type;
    int high_water; // max level of any item in the list
};

// Marking the <next> field of a node logically removes it from the list
#if 0
static inline markable_t  MARK_NODE(node_t * x) { return TAG_VALUE((markable_t)x, 0x1); }
static inline int        HAS_MARK(markable_t x) { return (IS_TAGGED(x, 0x1) == 0x1); }
static inline node_t *   GET_NODE(markable_t x) { assert(!HAS_MARK(x)); return (node_t *)x; }
static inline node_t * STRIP_MARK(markable_t x) { return ((node_t *)STRIP_TAG(x, 0x1)); }
#else
#define  MARK_NODE(x) TAG_VALUE((markable_t)(x), 0x1)
#define   HAS_MARK(x) (IS_TAGGED((x), 0x1) == 0x1)
#define   GET_NODE(x) ((node_t *)(x))
#define STRIP_MARK(x) ((node_t *)STRIP_TAG((x), 0x1))
#endif

static int random_level (void) {
    unsigned r = nbd_rand();
    int n = __builtin_ctz(r) / 2;
    if (n > MAX_LEVEL) { n = MAX_LEVEL; }
    return n;
}

static node_t *node_alloc (int level, map_key_t key, map_val_t val) {
    assert(level >= 0 && level <= MAX_LEVEL);
    size_t sz = sizeof(node_t) + level * sizeof(node_t *);
    node_t *item = (node_t *)nbd_malloc(sz);
    memset(item, 0, sz);
    item->key = key;
    item->val = val;
    item->top_level = level;
    TRACE("s2", "node_alloc: new node %p (%llu levels)", item, level);
    return item;
}

skiplist_t *sl_alloc (const datatype_t *key_type) {
    skiplist_t *sl = (skiplist_t *)nbd_malloc(sizeof(skiplist_t));
    sl->key_type = key_type;
    sl->high_water = 0;
    sl->head = node_alloc(MAX_LEVEL, 0, 0);
    memset(sl->head->next, 0, (MAX_LEVEL+1) * sizeof(skiplist_t *));
    return sl;
}

void sl_free (skiplist_t *sl) {
    node_t *item = GET_NODE(sl->head->next[0]);
    while (item) {
        node_t *next = STRIP_MARK(item->next[0]);
        if (sl->key_type != NULL) {
            nbd_free((void *)item->key);
        }
        nbd_free(item);
        item = next;
    }
}

size_t sl_count (skiplist_t *sl) {
    size_t count = 0;
    node_t *item = GET_NODE(sl->head->next[0]);
    while (item) {
        if (!HAS_MARK(item->next[0])) {
            count++;
        }
        item = STRIP_MARK(item->next[0]);
    }
    return count;
}

static node_t *find_preds (node_t **preds, node_t **succs, int n, skiplist_t *sl, map_key_t key, int help_remove) {
    node_t *pred = sl->head;
    node_t *item = NULL;
    TRACE("s2", "find_preds: searching for key %p in skiplist (head is %p)", key, pred);
    int d = 0;
    int start_level = sl->high_water;
    if (EXPECT_FALSE(start_level < n)) {
        start_level = n;
    }

    // Traverse the levels of <sl> from the top level to the bottom
    for (int level = start_level; level >= 0; --level) {
        markable_t next = pred->next[level];
        if (next == DOES_NOT_EXIST && level > n)
            continue;
        TRACE("s3", "find_preds: traversing level %p starting at %p", level, pred);
        if (EXPECT_FALSE(HAS_MARK(next))) {
            TRACE("s2", "find_preds: pred %p is marked for removal (next %p); retry", pred, next);
            ASSERT(level == pred->top_level || HAS_MARK(pred->next[level+1]));
            return find_preds(preds, succs, n, sl, key, help_remove); // retry
        }
        item = GET_NODE(next);
        while (item != NULL) {
            next = item->next[level];

            // A tag means an item is logically removed but not physically unlinked yet.
            while (EXPECT_FALSE(HAS_MARK(next))) {
                TRACE("s3", "find_preds: found marked item %p (next is %p)", item, next);
                if (!help_remove) {

                    // Skip over logically removed items.
                    item = STRIP_MARK(next);
                    if (EXPECT_FALSE(item == NULL))
                        break;
                    next = item->next[level];
                } else {

                    // Unlink logically removed items.
                    markable_t other = SYNC_CAS(&pred->next[level], item, STRIP_MARK(next));
                    if (other == (markable_t)item) {
                        TRACE("s3", "find_preds: unlinked item from pred %p", pred, 0);
                        item = STRIP_MARK(next);
                    } else {
                        TRACE("s3", "find_preds: lost race to unlink item pred %p's link changed to %p", pred, other);
                        if (HAS_MARK(other))
                            return find_preds(preds, succs, n, sl, key, help_remove); // retry
                        item = GET_NODE(other);
                    }
                    next = (item != NULL) ? item->next[level] : DOES_NOT_EXIST;
                }
            }

            if (EXPECT_FALSE(item == NULL)) {
                TRACE("s3", "find_preds: past the last item in the skiplist", 0, 0);
                break;
            }

            TRACE("s4", "find_preds: visiting item %p (next is %p)", item, next);
            TRACE("s4", "find_preds: key %p val %p", STRIP_MARK(item->key), item->val);

            if (EXPECT_TRUE(sl->key_type == NULL)) {
                d = item->key - key;
            } else {
                d = sl->key_type->cmp((void *)item->key, (void *)key);
            }

            if (d >= 0)
                break;

            pred = item;
            item = GET_NODE(next);
        }

        TRACE("s3", "find_preds: found pred %p next %p", pred, item);

        // The cast to unsigned is for the case when n is -1.
        if ((unsigned)level <= (unsigned)n) { 
            if (preds != NULL) {
                preds[level] = pred;
            }
            if (succs != NULL) {
                succs[level] = item;
            }
        }
    }

    // fill in empty levels
    if (n == -1 && item != NULL && preds != NULL) {
        assert(item->top_level <= MAX_LEVEL);
        for (int level = start_level + 1; level <= item->top_level; ++level) {
            preds[level] = sl->head;
        }
    }

    if (d == 0) {
        TRACE("s2", "find_preds: found matching item %p in skiplist, pred is %p", item, pred);
        return item;
    }
    TRACE("s2", "find_preds: found proper place for key %p in skiplist, pred is %p. returning null", key, pred);
    return NULL;
}

static void sl_unlink (skiplist_t *sl, map_key_t key) {
    node_t *pred = sl->head;
    node_t *item = NULL;
    TRACE("s2", "sl_unlink: unlinking marked item with key %p", key, 0);
    int d = 0;

    // Traverse the levels of <sl> from the top level to the bottom
    for (int level = sl->high_water; level >= 0; --level) {
        markable_t next = pred->next[level];
        if (next == DOES_NOT_EXIST)
            continue;
        TRACE("s3", "sl_unlink: traversing level %p starting at %p", level, pred);
        if (EXPECT_FALSE(HAS_MARK(next))) {
            TRACE("s2", "sl_unlink: lost a race; pred %p is marked for removal (next %p); retry", pred, next);
            ASSERT(level == pred->top_level || HAS_MARK(pred->next[level+1]));
            return sl_unlink(sl, key); // retry
        }
        item = GET_NODE(next);
        while (item != NULL) {
            next = item->next[level];

            while (HAS_MARK(next)) {
                TRACE("s3", "sl_unlink: found marked item %p (next is %p)", item, next);

                markable_t other = SYNC_CAS(&pred->next[level], item, STRIP_MARK(next));
                if (other == (markable_t)item) {
                    TRACE("s3", "sl_unlink: unlinked item from pred %p", pred, 0);
                    item = STRIP_MARK(next);
                } else {
                    TRACE("s3", "sl_unlink: lost race to unlink item, pred %p's link changed to %p", pred, other);
                    if (HAS_MARK(other))
                        return sl_unlink(sl, key); // retry
                    item = GET_NODE(other);
                }
                next = (item != NULL) ? item->next[level] : DOES_NOT_EXIST;
            }

            if (EXPECT_FALSE(item == NULL)) {
                TRACE("s3", "sl_unlink: past the last item in the skiplist", 0, 0);
                break;
            }

            TRACE("s4", "sl_unlink: visiting item %p (next is %p)", item, next);
            TRACE("s4", "sl_unlink: key %p val %p", STRIP_MARK(item->key), item->val);

            if (EXPECT_TRUE(sl->key_type == NULL)) {
                d = item->key - key;
            } else {
                d = sl->key_type->cmp((void *)item->key, (void *)key);
            }

            if (d > 0)
                break;

            pred = item;
            item = GET_NODE(next);
        }

        TRACE("s3", "sl_unlink: at pred %p next %p", pred, item);
    }
}

// Fast find that does not help unlink partially removed nodes and does not return the node's predecessors.
map_val_t sl_lookup (skiplist_t *sl, map_key_t key) {
    TRACE("s1", "sl_lookup: searching for key %p in skiplist %p", key, sl);
    node_t *item = find_preds(NULL, NULL, 0, sl, key, FALSE);

    // If we found an <item> matching the <key> return its value.
    if (item != NULL) {
        map_val_t val = item->val;
        if (val != DOES_NOT_EXIST) {
            TRACE("s1", "sl_lookup: found item %p. val %p. returning item", item, item->val);
            return val;
        }
    }

    TRACE("l1", "sl_lookup: no item in the skiplist matched the key", 0, 0);
    return DOES_NOT_EXIST;
}

map_key_t sl_min_key (skiplist_t *sl) {
    node_t *item = GET_NODE(sl->head->next[0]);
    while (item != NULL) {
        markable_t next = item->next[0];
        if (!HAS_MARK(next))
            return item->key;
        item = STRIP_MARK(next);
    }
    return DOES_NOT_EXIST;
}

static map_val_t update_item (node_t *item, map_val_t expectation, map_val_t new_val) {
    map_val_t old_val = item->val;

    // If the item's value is DOES_NOT_EXIST it means another thread removed the node out from under us.
    if (EXPECT_FALSE(old_val == DOES_NOT_EXIST)) {
        TRACE("s2", "update_item: lost a race to another thread removing the item. retry", 0, 0);
        return DOES_NOT_EXIST; // retry
    }

    if (EXPECT_FALSE(expectation == CAS_EXPECT_DOES_NOT_EXIST)) {
        TRACE("s1", "update_item: found an item %p in the skiplist that matched the key. the expectation was "
                "not met, the skiplist was not changed", item, old_val);
        return old_val; // failure
    }

    // Use a CAS and not a SWAP. If the CAS fails it means another thread removed the node or updated its
    // value. If another thread removed the node but it is not unlinked yet and we used a SWAP, we could
    // replace DOES_NOT_EXIST with our value. Then another thread that is updating the value could think it
    // succeeded and return our value even though it should return DOES_NOT_EXIST. 
    if (old_val == SYNC_CAS(&item->val, old_val, new_val)) {
        TRACE("s1", "update_item: the CAS succeeded. updated the value of the item", 0, 0);
        return old_val; // success
    }
    TRACE("s2", "update_item: lost a race. the CAS failed. another thread changed the item's value", 0, 0);

    // retry
    return update_item(item, expectation, new_val); // tail call
}

map_val_t sl_cas (skiplist_t *sl, map_key_t key, map_val_t expectation, map_val_t new_val) {
    TRACE("s1", "sl_cas: key %p skiplist %p", key, sl);
    TRACE("s1", "sl_cas: expectation %p new value %p", expectation, new_val);
    ASSERT((int64_t)new_val > 0);

    node_t *preds[MAX_LEVEL+1];
    node_t *nexts[MAX_LEVEL+1];
    node_t *new_item = NULL;
    int n = random_level();
    node_t *old_item = find_preds(preds, nexts, n, sl, key, TRUE);

    // If there is already an item in the skiplist that matches the key just update its value.
    if (old_item != NULL) {
        map_val_t ret_val = update_item(old_item, expectation, new_val);
        if (ret_val != DOES_NOT_EXIST)
            return ret_val;

        // If we lose a race with a thread removing the item we tried to update then we have to retry.
        return sl_cas(sl, key, expectation, new_val); // tail call 
    }

    if (EXPECT_FALSE(expectation != CAS_EXPECT_DOES_NOT_EXIST && expectation != CAS_EXPECT_WHATEVER)) {
        TRACE("l1", "sl_cas: the expectation was not met, the skiplist was not changed", 0, 0);
        return DOES_NOT_EXIST; // failure, the caller expected an item for the <key> to already exist 
    }

    // Create a new node and insert it into the skiplist.
    TRACE("s3", "sl_cas: attempting to insert a new item between %p and %p", preds[0], nexts[0]);
    map_key_t new_key = sl->key_type == NULL ? key : (map_key_t)sl->key_type->clone((void *)key);
    if (n > sl->high_water) {
        n = sl->high_water + 1;
        int x = SYNC_ADD(&sl->high_water, 1);
        x = x;
        TRACE("s2", "sl_cas: incremented high water mark to %p", x, 0);
    }
    new_item = node_alloc(n, new_key, new_val);

    // Set <new_item>'s next pointers to their proper values
    markable_t next = new_item->next[0] = (markable_t)nexts[0];
    for (int level = 1; level <= new_item->top_level; ++level) {
        new_item->next[level] = (markable_t)nexts[level];
    }

    // Link <new_item> into <sl> from the bottom level up. After <new_item> is inserted into the bottom level
    // it is officially part of the skiplist.
    node_t *pred = preds[0];
    markable_t other = SYNC_CAS(&pred->next[0], next, new_item);
    if (other != next) {
        TRACE("s3", "sl_cas: failed to change pred's link: expected %p found %p", next, other);

        // Lost a race to another thread modifying the skiplist. Free the new item we allocated and retry.
        if (sl->key_type != NULL) {
            nbd_free((void *)new_key);
        }
        nbd_free(new_item); 
        return sl_cas(sl, key, expectation, new_val); // tail call
    }

    TRACE("s3", "sl_cas: successfully inserted a new item %p at the bottom level", new_item, 0);

    for (int level = 1; level <= new_item->top_level; ++level) {
        TRACE("s3", "sl_cas: inserting the new item %p at level %p", new_item, level);
        do {
            node_t *   pred = preds[level];
            ASSERT(new_item->next[level]==(markable_t)nexts[level] || new_item->next[level]==MARK_NODE(nexts[level]));
            TRACE("s3", "sl_cas: attempting to to insert the new item between %p and %p", pred, nexts[level]);

            markable_t other = SYNC_CAS(&pred->next[level], nexts[level], (markable_t)new_item);
            if (other == (markable_t)nexts[level])
                break; // successfully linked <new_item> into the skiplist at the current <level>
            TRACE("s3", "sl_cas: lost a race. failed to change pred's link. expected %p found %p", nexts[level], other);

            // Find <new_item>'s new preds and nexts.
            find_preds(preds, nexts, new_item->top_level, sl, key, TRUE);

            for (int i = level; i <= new_item->top_level; ++i) {
                markable_t old_next = new_item->next[i];
                if ((markable_t)nexts[i] == old_next)
                    continue;

                // Update <new_item>'s inconsistent next pointer before trying again. Use a CAS so if another thread
                // is trying to remove the new item concurrently we do not stomp on the mark it places on the item.
                TRACE("s3", "sl_cas: attempting to update the new item's link from %p to %p", old_next, nexts[i]);
                other = SYNC_CAS(&new_item->next[i], old_next, nexts[i]);
                ASSERT(other == old_next || other == MARK_NODE(old_next));
                
                // If another thread is removing this item we can stop linking it into to skiplist
                if (HAS_MARK(other)) {
                    sl_unlink(sl, key); // see comment below
                    return DOES_NOT_EXIST;
                }
            }
        } while (1);
    }

    // In case another thread was in the process of removing the <new_item> while we were added it, we have to
    // make sure it is completely unlinked before we return. We might have lost a race and inserted the new item
    // at some level after the other thread thought it was fully removed. That is a problem because once a thread
    // thinks it completely unlinks a node it queues it to be freed
    if (HAS_MARK(new_item->next[new_item->top_level])) {
        sl_unlink(sl, key);
    }

    return DOES_NOT_EXIST; // success, inserted a new item
}

map_val_t sl_remove (skiplist_t *sl, map_key_t key) {
    TRACE("s1", "sl_remove: removing item with key %p from skiplist %p", key, sl);
    node_t *preds[MAX_LEVEL+1];
    node_t *item = find_preds(preds, NULL, -1, sl, key, TRUE);
    if (item == NULL) {
        TRACE("s3", "sl_remove: remove failed, an item with a matching key does not exist in the skiplist", 0, 0);
        return DOES_NOT_EXIST;
    }

    // Mark <item> at each level of <sl> from the top down. If multiple threads try to concurrently remove
    // the same item only one of them should succeed. Marking the bottom level establishes which of them succeeds.
    markable_t old_next = 0;
    for (int level = item->top_level; level >= 0; --level) {
        markable_t next;
        old_next = item->next[level];
        do {
            TRACE("s3", "sl_remove: marking item at level %p (next %p)", level, old_next);
            next = old_next;
            old_next = SYNC_CAS(&item->next[level], next, MARK_NODE((node_t *)next));
            if (HAS_MARK(old_next)) {
                TRACE("s2", "sl_remove: %p is already marked for removal by another thread (next %p)", item, old_next);
                if (level == 0) 
                    return DOES_NOT_EXIST;
                break;
            }
        } while (next != old_next);
    }

    // Atomically swap out the item's value in case another thread is updating the item while we are 
    // removing it. This establishes which operation occurs first logically, the update or the remove. 
    map_val_t val = SYNC_SWAP(&item->val, DOES_NOT_EXIST); 
    TRACE("s2", "sl_remove: replaced item %p's value with DOES_NOT_EXIT", item, 0);

    // unlink the item
    sl_unlink(sl, key);

    // free the node
    if (sl->key_type != NULL) {
        rcu_defer_free((void *)item->key);
    }
    rcu_defer_free(item);

    return val;
}

void sl_print (skiplist_t *sl) {

    printf("high water: %d levels\n", sl->high_water);
    for (int level = MAX_LEVEL; level >= 0; --level) {
        node_t *item = sl->head;
        if (item->next[level] == DOES_NOT_EXIST)
            continue;
        printf("(%d) ", level);
        int i = 0;
        while (item) {
            markable_t next = item->next[level];
            printf("%s%p ", HAS_MARK(next) ? "*" : "", item);
            item = STRIP_MARK(next);
            if (i++ > 30) {
                printf("...");
                break;
            }
        }
        printf("\n");
        fflush(stdout);
    }
    node_t *item = sl->head;
    int i = 0;
    while (item) {
        int is_marked = HAS_MARK(item->next[0]);
        printf("%s%p:0x%llx ", is_marked ? "*" : "", item, (uint64_t)item->key);
        if (item != sl->head) {
            printf("[%d]", item->top_level);
        } else {
            printf("[HEAD]");
        }
        for (int level = 1; level <= item->top_level; ++level) {
            node_t *next = STRIP_MARK(item->next[level]);
            is_marked = HAS_MARK(item->next[0]);
            printf(" %p%s", next, is_marked ? "*" : "");
            if (item == sl->head && item->next[level] == DOES_NOT_EXIST)
                break;
        }
        printf("\n");
        fflush(stdout);
        item = STRIP_MARK(item->next[0]);
        if (i++ > 30) {
            printf("...\n");
            break;
        }
    }
}

sl_iter_t *sl_iter_begin (skiplist_t *sl, map_key_t key) {
    sl_iter_t *iter = (sl_iter_t *)nbd_malloc(sizeof(sl_iter_t));
    if (key != DOES_NOT_EXIST) {
        find_preds(NULL, &iter->next, 0, sl, key, FALSE);
    } else {
        iter->next = GET_NODE(sl->head->next[0]);
    }
    return iter;
}

map_val_t sl_iter_next (sl_iter_t *iter, map_key_t *key_ptr) {
    assert(iter);
    node_t *item = iter->next;
    while (item != NULL && HAS_MARK(item->next[0])) {
        item = STRIP_MARK(item->next[0]);
    }
    if (item == NULL) {
        iter->next = NULL;
        return DOES_NOT_EXIST;
    }
    iter->next = STRIP_MARK(item->next[0]);
    if (key_ptr != NULL) {
        *key_ptr = item->key;
    }
    return item->val;
}

void sl_iter_free (sl_iter_t *iter) {
    nbd_free(iter);
}