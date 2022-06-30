/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "memcached.h"
#include "bipbuffer.h"
#include "slab_automove.h"
#include "storage.h"
#ifdef EXTSTORE
#include "slab_automove_extstore.h"
#endif
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <poll.h>


#define LARGEST_ID POWER_LARGEST

static uint64_t cas_id = 0;

static pthread_mutex_t cas_id_lock = PTHREAD_MUTEX_INITIALIZER;

/* Get the next CAS id for a new item. */
/* TODO: refactor some atomics for this. */
uint64_t get_cas_id(void) {
    pthread_mutex_lock(&cas_id_lock);
    uint64_t next_id = ++cas_id;
    pthread_mutex_unlock(&cas_id_lock);
    return next_id;
}

void set_cas_id(uint64_t new_cas) {
    pthread_mutex_lock(&cas_id_lock);
    cas_id = new_cas;
    pthread_mutex_unlock(&cas_id_lock);
}

int item_is_flushed(item *it) {
    rel_time_t oldest_live = settings.oldest_live;
    uint64_t cas = ITEM_get_cas(it);
    uint64_t oldest_cas = settings.oldest_cas;
    if (oldest_live == 0 || oldest_live > current_time)
        return 0;
    if ((it->time <= oldest_live)
            || (oldest_cas != 0 && cas != 0 && cas < oldest_cas)) {
        return 1;
    }
    return 0;
}


/* Enable this for reference-count debugging. */
#if 0
# define DEBUG_REFCNT(it,op) \
                fprintf(stderr, "item %x refcnt(%c) %d %c%c%c\n", \
                        it, op, it->refcount, \
                        (it->it_flags & ITEM_LINKED) ? 'L' : ' ', \
                        (it->it_flags & ITEM_SLABBED) ? 'S' : ' ')
#else
# define DEBUG_REFCNT(it,op) while(0)
#endif

/**
 * Generates the variable-sized part of the header for an object.
 *
 * nkey    - The length of the key
 * flags   - key flags
 * nbytes  - Number of bytes to hold value and addition CRLF terminator
 * suffix  - Buffer for the "VALUE" line suffix (flags, size).
 * nsuffix - The length of the suffix is stored here.
 *
 * Returns the total size of the header.
 */
static size_t item_make_header(const uint8_t nkey, const unsigned int flags, const int nbytes,
                     char *suffix, uint8_t *nsuffix) {
    if (flags == 0) {
        *nsuffix = 0;
    } else {
        *nsuffix = sizeof(flags);
    }
    return sizeof(item) + nkey + *nsuffix + nbytes;
}

item *do_item_alloc_pull(const size_t ntotal, const unsigned int id) {
    item *it = NULL;
    it = slabs_alloc(ntotal, id, 0);
    assert(it);
    return it;
}

/* Chain another chunk onto this chunk. */
/* slab mover: if it finds a chunk without ITEM_CHUNK flag, and no ITEM_LINKED
 * flag, it counts as busy and skips.
 * I think it might still not be safe to do linking outside of the slab lock
 */
item_chunk *do_item_alloc_chunk(item_chunk *ch, const size_t bytes_remain) {
    // TODO: Should be a cleaner way of finding real size with slabber calls
    size_t size = bytes_remain + sizeof(item_chunk);
    if (size > settings.slab_chunk_size_max)
        size = settings.slab_chunk_size_max;
    unsigned int id = slabs_clsid(size);

    item_chunk *nch = (item_chunk *) do_item_alloc_pull(size, id);
    if (nch == NULL)
        return NULL;
    nch->payload = (item_chunk_payload *) Montage_malloc(sizeof(struct _item_chunk_payload) + size - sizeof(item_chunk));

    // link in.
    // ITEM_CHUNK[ED] bits need to be protected by the slabs lock.
    slabs_mlock();
    nch->head = ch->head;
    ch->next = nch;
    nch->prev = ch;
    nch->next = 0;
    nch->used = 0;
    nch->slabs_clsid = id;
    nch->size = size - sizeof(item_chunk);
    nch->it_flags |= ITEM_CHUNK;
    slabs_munlock();
    return nch;
}

item *do_item_alloc(char *key, const size_t nkey, const unsigned int flags,
                    const rel_time_t exptime, const int nbytes) {
    uint8_t nsuffix;
    item *it = NULL;
    char suffix[40];
    // Avoid potential underflows.
    if (nbytes < 2)
        return 0;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes, suffix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    unsigned int id = slabs_clsid(ntotal);
    unsigned int hdr_id = 0;
    if (id == 0)
        return 0;

    /* This is a large item. Allocate a header object now, lazily allocate
     *  chunks while reading the upload.
     */
    if (ntotal > settings.slab_chunk_size_max) {
        /* We still link this item into the LRU for the larger slab class, but
         * we're pulling a header from an entirely different slab class. The
         * free routines handle large items specifically.
         */
        int htotal = nkey + 1 + nsuffix + sizeof(item) + sizeof(item_chunk);
        if (settings.use_cas) {
            htotal += sizeof(uint64_t);
        }
#ifdef NEED_ALIGN
        // header chunk needs to be padded on some systems
        int remain = htotal % 8;
        if (remain != 0) {
            htotal += 8 - remain;
        }
#endif
        hdr_id = slabs_clsid(htotal);
        it = do_item_alloc_pull(htotal, hdr_id);
        /* setting ITEM_CHUNKED is fine here because we aren't LINKED yet. */
        if (it != NULL)
            it->it_flags |= ITEM_CHUNKED;
    } else {
        it = do_item_alloc_pull(ntotal, id);
        if (it) {
            it->payload = (item_payload *) Montage_malloc(
                sizeof(struct _itempayload) + nkey + 1 + nbytes + nsuffix +
                (settings.use_cas? sizeof(uint64_t) : 0));
        }
    }

    if (it == NULL) {
        return NULL;
    }

    assert(it->it_flags == 0 || it->it_flags == ITEM_CHUNKED);
    //assert(it != heads[id]);

    /* Refcount is seeded to 1 by slabs_alloc() */
    it->next = it->prev = 0;

    /* Items are initially loaded into the HOT_LRU. This is '0' but I want at
     * least a note here. Compiler (hopefully?) optimizes this out.
     */
    if (settings.temp_lru &&
            exptime - current_time <= settings.temporary_ttl) {
        id |= TEMP_LRU;
    } else if (settings.lru_segmented) {
        id |= HOT_LRU;
    } else {
        /* There is only COLD in compat-mode */
        id |= COLD_LRU;
    }
    it->slabs_clsid = id;

    DEBUG_REFCNT(it, '*');
    it->it_flags |= settings.use_cas ? ITEM_CAS : 0;
    it->it_flags |= nsuffix != 0 ? ITEM_CFLAGS : 0;
    it->nkey = nkey;
    it->nbytes = nbytes;
    memcpy(ITEM_key(it), key, nkey); // Hs: no need for Montage accessors here; `it` is newlly allocated.
    it->exptime = exptime;
    if (nsuffix > 0) {
        memcpy(ITEM_suffix(it), &flags, sizeof(flags)); // Hs: no need for Montage accessors here; `it` is newlly allocated.
    }

    /* Initialize internal chunk. */
    if (it->it_flags & ITEM_CHUNKED) {
        item_chunk *chunk = (item_chunk *) ITEM_schunk(it);

        chunk->next = 0;
        chunk->prev = 0;
        chunk->used = 0;
        chunk->size = 0;
        chunk->head = it;
        chunk->orig_clsid = hdr_id;
    }
    it->h_next = 0;

    return it;
}

void item_free(item *it) {
    size_t ntotal = ITEM_ntotal(it);
    unsigned int clsid;
    assert((it->it_flags & ITEM_LINKED) == 0);
    assert(it->refcount == 0);

    /* so slab size changer can tell later if item is already free or not */
    clsid = ITEM_clsid(it);
    DEBUG_REFCNT(it, 'F');
    slabs_free(it, ntotal, clsid);
}

/**
 * Returns true if an item will fit in the cache (its size does not exceed
 * the maximum for a cache entry.)
 */
bool item_size_ok(const size_t nkey, const int flags, const int nbytes) {
    char prefix[40];
    uint8_t nsuffix;
    if (nbytes < 2)
        return false;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes,
                                     prefix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    return slabs_clsid(ntotal) != 0;
}

/* fixing stats/references during warm start */
void do_item_link_fixup(item *it) {
    int ntotal = ITEM_ntotal_transient(it);
    uint32_t hv = hash(ITEM_key(it), it->nkey);
    assoc_insert(it, hv);

    STATS_LOCK();
    stats_state.curr_bytes += ntotal;
    stats_state.curr_items += 1;
    stats.total_items += 1;
    STATS_UNLOCK();


    return;
}

int do_item_link(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_LINK(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & (ITEM_LINKED|ITEM_SLABBED)) == 0);
    it->it_flags |= ITEM_LINKED;
    it->time = current_time;

    STATS_LOCK();
    stats_state.curr_bytes += ITEM_ntotal(it);
    stats_state.curr_items += 1;
    stats.total_items += 1;
    STATS_UNLOCK();

    /* Allocate a new CAS ID on link. */
    ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
    assoc_insert(it, hv);
    refcount_incr(it);

    return 1;
}

void do_item_unlink(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {
        it->it_flags &= ~ITEM_LINKED;
        STATS_LOCK();
        stats_state.curr_bytes -= ITEM_ntotal(it);
        stats_state.curr_items -= 1;
        STATS_UNLOCK();
        assoc_delete(ITEM_key(it), it->nkey, hv);
        do_item_remove(it);
    }
}

/* FIXME: Is it necessary to keep this copy/pasted code? */
void do_item_unlink_nolock(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {
        it->it_flags &= ~ITEM_LINKED;
        STATS_LOCK();
        stats_state.curr_bytes -= ITEM_ntotal(it);
        stats_state.curr_items -= 1;
        STATS_UNLOCK();
        assoc_delete(ITEM_key(it), it->nkey, hv);
        do_item_remove(it);
    }
}

void do_item_remove(item *it) {
    MEMCACHED_ITEM_REMOVE(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);
    assert(it->refcount > 0);

    if (refcount_decr(it) == 0) {
        item_free(it);
    }
}

int do_item_replace(item *it, item *new_it, const uint32_t hv) {
    MEMCACHED_ITEM_REPLACE(ITEM_key(it), it->nkey, it->nbytes,
                           ITEM_key(new_it), new_it->nkey, new_it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);

    do_item_unlink(it, hv);
    return do_item_link(new_it, hv);
}



/** wrapper around assoc_find which does the lazy expiration logic */
item *do_item_get(const char *key, const size_t nkey, const uint32_t hv, conn *c, const bool do_update) {
    item *it = assoc_find(key, nkey, hv);
    if (it != NULL) {
        refcount_incr(it);
        /* Optimization for slab reassignment. prevents popular items from
         * jamming in busy wait. Can only do this here to satisfy lock order
         * of item_lock, slabs_lock. */
        /* This was made unsafe by removal of the cache_lock:
         * slab_rebalance_signal and slab_rebal.* are modified in a separate
         * thread under slabs_lock. If slab_rebalance_signal = 1, slab_start =
         * NULL (0), but slab_end is still equal to some value, this would end
         * up unlinking every item fetched.
         * This is either an acceptable loss, or if slab_rebalance_signal is
         * true, slab_start/slab_end should be put behind the slabs_lock.
         * Which would cause a huge potential slowdown.
         * Could also use a specific lock for slab_rebal.* and
         * slab_rebalance_signal (shorter lock?)
         */
        /*if (slab_rebalance_signal &&
            ((void *)it >= slab_rebal.slab_start && (void *)it < slab_rebal.slab_end)) {
            do_item_unlink(it, hv);
            do_item_remove(it);
            it = NULL;
        }*/
    }
    int was_found = 0;

    if (settings.verbose > 2) {
        int ii;
        if (it == NULL) {
            fprintf(stderr, "> NOT FOUND ");
        } else {
            fprintf(stderr, "> FOUND KEY ");
        }
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }
    }

    if (it != NULL) {
        was_found = 1;
        if (item_is_flushed(it)) {
            do_item_unlink(it, hv);
            STORAGE_delete(c->thread->storage, it);
            do_item_remove(it);
            it = NULL;
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.get_flushed++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            if (settings.verbose > 2) {
                fprintf(stderr, " -nuked by flush");
            }
            was_found = 2;
        } else if (it->exptime != 0 && it->exptime <= current_time) {
            do_item_unlink(it, hv);
            STORAGE_delete(c->thread->storage, it);
            do_item_remove(it);
            it = NULL;
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.get_expired++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            if (settings.verbose > 2) {
                fprintf(stderr, " -nuked by expire");
            }
            was_found = 3;
        } else {
            DEBUG_REFCNT(it, '+');
        }
    }

    if (settings.verbose > 2)
        fprintf(stderr, "\n");
    /* For now this is in addition to the above verbose logging. */
    LOGGER_LOG(c->thread->l, LOG_FETCHERS, LOGGER_ITEM_GET, NULL, was_found, key,
               nkey, (it) ? it->nbytes : 0, (it) ? ITEM_clsid(it) : 0, c->sfd);

    return it;
}

item *do_item_touch(const char *key, size_t nkey, uint32_t exptime,
                    const uint32_t hv, conn *c) {
    item *it = do_item_get(key, nkey, hv, c, DO_UPDATE);
    if (it != NULL) {
        it->exptime = exptime;
    }
    return it;
}
