/* zn_malloc.c - zone-based malloc routines */
/* $OpenLDAP$*/
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2003-2004 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* Copyright 2004 IBM Corporation
 * All rights reserved.
 * Redisribution and use in source and binary forms, with or without
 * modification, are permitted only as  authorizd by the OpenLADP
 * Public License.
 */
/* ACKNOWLEDGEMENTS
 * This work originally developed by Jong-Hyuk Choi
 * 2004/11/29   jongchoi@OpenLDAP.org
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "slap.h"

#ifdef SLAP_ZONE_ALLOC

static int slap_zone_cmp(const void *v1, const void *v2);
void * slap_replenish_zopool(void *ctx);

static void
slap_zo_release(void *data)
{
	struct zone_object *zo = (struct zone_object *)data;
	ch_free( zo );
}

void
slap_zn_mem_destroy(
	void *ctx
)
{
	struct zone_heap *zh = ctx;
	int pad = 2*sizeof(int)-1, pad_shift;
	int order_start = -1, i, j;
	struct zone_object *zo;

	pad_shift = pad - 1;
	do {
		order_start++;
	} while (pad_shift >>= 1);

	ldap_pvt_thread_mutex_lock( &zh->zh_mutex );
	for (i = 0; i <= zh->zh_zoneorder - order_start; i++) {
		zo = LDAP_LIST_FIRST(&zh->zh_free[i]);
		while (zo) {
			struct zone_object *zo_tmp = zo;
			zo = LDAP_LIST_NEXT(zo, zo_link);
			LDAP_LIST_INSERT_HEAD(&zh->zh_zopool, zo_tmp, zo_link);
		}
	}
	ch_free(zh->zh_free);

	for (i = 0; i < zh->zh_numzones; i++) {
		for (j = 0; j < zh->zh_zoneorder - order_start; j++) {
			ch_free(zh->zh_maps[i][j]);
		}
		ch_free(zh->zh_maps[i]);
		munmap(zh->zh_zones[i], zh->zh_zonesize);
	}
	ch_free(zh->zh_maps);
	ch_free(zh->zh_zones);
	ch_free(zh->zh_seqno);

	avl_free(zh->zh_zonetree, slap_zo_release);

	zo = LDAP_LIST_FIRST(&zh->zh_zopool);
	while (zo) {
		struct zone_object *zo_tmp = zo;
		zo = LDAP_LIST_NEXT(zo, zo_link);
		if (!zo_tmp->zo_blockhead) {
			LDAP_LIST_REMOVE(zo_tmp, zo_link);
		}
	}
	zo = LDAP_LIST_FIRST(&zh->zh_zopool);
	while (zo) {
		struct zone_object *zo_tmp = zo;
		zo = LDAP_LIST_NEXT(zo, zo_link);
		ch_free(zo_tmp);
	}
	ldap_pvt_thread_mutex_unlock( &zh->zh_mutex );
	ldap_pvt_thread_mutex_destroy( &zh->zh_mutex );
	ch_free(zh);
}

void *
slap_zn_mem_create(
	ber_len_t initsize,
	ber_len_t maxsize,
	ber_len_t deltasize,
	ber_len_t zonesize
)
{
	struct zone_heap *zh = NULL;
	ber_len_t zpad;
	int pad = 2*sizeof(int)-1, pad_shift;
	int size_shift;
	int order = -1, order_start = -1, order_end = -1;
	int i, j;
	struct zone_object *zo;

	zh = (struct zone_heap *)ch_calloc(1, sizeof(struct zone_heap));

	zh->zh_fd = open("/dev/zero", O_RDWR);

	if ( zonesize ) {
		zh->zh_zonesize = zonesize;
	} else {
		zh->zh_zonesize = SLAP_ZONE_SIZE;
	}

	zpad = zh->zh_zonesize - 1;
	zh->zh_numzones = ((initsize + zpad) & ~zpad) / zh->zh_zonesize;

	if ( maxsize && maxsize >= initsize ) {
		zh->zh_maxzones = ((maxsize + zpad) & ~zpad) / zh->zh_zonesize;
	} else {
		zh->zh_maxzones = ((initsize + zpad) & ~zpad) / zh->zh_zonesize;
	}

	if ( deltasize ) {
		zh->zh_deltazones = ((deltasize + zpad) & ~zpad) / zh->zh_zonesize;
	} else {
		zh->zh_deltazones = ((SLAP_ZONE_DELTA+zpad) & ~zpad) / zh->zh_zonesize;
	}

	Debug(LDAP_DEBUG_ANY, "==> slap_zn_mem_create: %d\n",
					zh->zh_numzones, 0, 0 );

	size_shift = zh->zh_zonesize - 1;
	do {
		order_end++;
	} while (size_shift >>= 1);

	pad_shift = pad - 1;
	do {
		order_start++;
	} while (pad_shift >>= 1);

	order = order_end - order_start + 1;

	zh->zh_zones = (void **)ch_malloc(zh->zh_maxzones * sizeof(void*));
	zh->zh_maps = (unsigned char ***)ch_malloc(
					zh->zh_maxzones * sizeof(unsigned char**));

	zh->zh_zoneorder = order_end;
	zh->zh_free = (struct zh_freelist *)
					ch_malloc(order * sizeof(struct zh_freelist));
	zh->zh_seqno = (unsigned long *)ch_calloc(zh->zh_maxzones,
											sizeof(unsigned long));
	for (i = 0; i < order; i++) {
		LDAP_LIST_INIT(&zh->zh_free[i]);
	}
	LDAP_LIST_INIT(&zh->zh_zopool);

	for (i = 0; i < zh->zh_numzones; i++) {
		zh->zh_zones[i] = mmap(0, zh->zh_zonesize, PROT_READ | PROT_WRITE,
							MAP_PRIVATE, zh->zh_fd, 0);
		zh->zh_maps[i] = (unsigned char **)
					ch_malloc(order * sizeof(unsigned char *));
		for (j = 0; j < order; j++) {
			int shiftamt = order_start + 1 + j;
			int nummaps = zh->zh_zonesize >> shiftamt;
			assert(nummaps);
			nummaps /= 8;
			if (!nummaps) nummaps = 1;
			zh->zh_maps[i][j] = (unsigned char *)ch_malloc(nummaps);
			memset(zh->zh_maps[i][j], 0, nummaps);
		}

		if (LDAP_LIST_EMPTY(&zh->zh_zopool)) {
			slap_replenish_zopool(zh);
		}
		zo = LDAP_LIST_FIRST(&zh->zh_zopool);
		LDAP_LIST_REMOVE(zo, zo_link);
		zo->zo_ptr = zh->zh_zones[i];
		zo->zo_idx = i;
		LDAP_LIST_INSERT_HEAD(&zh->zh_free[order-1], zo, zo_link);

		if (LDAP_LIST_EMPTY(&zh->zh_zopool)) {
			slap_replenish_zopool(zh);
		}
		zo = LDAP_LIST_FIRST(&zh->zh_zopool);
		LDAP_LIST_REMOVE(zo, zo_link);
		zo->zo_ptr = (void*)((unsigned long)zh->zh_zones[i]>>zh->zh_zoneorder);
		zo->zo_idx = i;
		avl_insert(&zh->zh_zonetree, zo, slap_zone_cmp, avl_dup_error);
	}

	ldap_pvt_thread_mutex_init(&zh->zh_mutex);

	return zh;
}

void *
slap_zn_malloc(
    ber_len_t	size,
	void *ctx
)
{
	struct zone_heap *zh = ctx;
	ber_len_t size_shift;
	int pad = 2*sizeof(int)-1, pad_shift;
	int order = -1, order_start = -1;
	struct zone_object *zo, *zo_new, *zo_left, *zo_right;
	ber_len_t *ptr, *new;
	int idx;
	unsigned long diff;
	int i, j, k;

	if (!zh) return ber_memalloc_x(size, NULL);

	/* round up to doubleword boundary */
	size += 2*sizeof(ber_len_t) + pad;
	size &= ~pad;

	size_shift = size - 1;
	do {
		order++;
	} while (size_shift >>= 1);

	pad_shift = pad - 1;
	do {
		order_start++;
	} while (pad_shift >>= 1);

retry:

	ldap_pvt_thread_mutex_lock( &zh->zh_mutex );
	for (i = order; i <= zh->zh_zoneorder &&
			LDAP_LIST_EMPTY(&zh->zh_free[i-order_start]); i++);

	if (i == order) {
		zo_new = LDAP_LIST_FIRST(&zh->zh_free[i-order_start]);
		LDAP_LIST_REMOVE(zo_new, zo_link);
		ptr = zo_new->zo_ptr;
		idx = zo_new->zo_idx;
		diff = (unsigned long)((char*)ptr -
				(char*)zh->zh_zones[idx]) >> (order + 1);
		zh->zh_maps[idx][order-order_start][diff>>3] |= (1 << (diff & 0x7));
		*ptr++ = zh->zh_seqno[idx];
		*ptr++ = size - 2*sizeof(ber_len_t);
		zo_new->zo_ptr = NULL;
		zo_new->zo_idx = -1;
		LDAP_LIST_INSERT_HEAD(&zh->zh_zopool, zo_new, zo_link);
		ldap_pvt_thread_mutex_unlock( &zh->zh_mutex );
		return((void*)ptr);
	} else if (i <= zh->zh_zoneorder) {
		for (j = i; j > order; j--) {
			zo_left = LDAP_LIST_FIRST(&zh->zh_free[j-order_start]);
			LDAP_LIST_REMOVE(zo_left, zo_link);
			if (LDAP_LIST_EMPTY(&zh->zh_zopool)) {
				slap_replenish_zopool(zh);
			}
			zo_right = LDAP_LIST_FIRST(&zh->zh_zopool);
			LDAP_LIST_REMOVE(zo_right, zo_link);
			zo_right->zo_ptr = zo_left->zo_ptr + (1 << j);
			zo_right->zo_idx = zo_left->zo_idx;
			if (j == order + 1) {
				ptr = zo_left->zo_ptr;
				diff = (unsigned long)((char*)ptr -
						(char*)zh->zh_zones[zo_left->zo_idx]) >> (order+1);
				zh->zh_maps[zo_left->zo_idx][order-order_start][diff>>3] |=
						(1 << (diff & 0x7));
				*ptr++ = zh->zh_seqno[zo_left->zo_idx];
				*ptr++ = size - 2*sizeof(ber_len_t);
				LDAP_LIST_INSERT_HEAD(
						&zh->zh_free[j-1-order_start], zo_right, zo_link);
				LDAP_LIST_INSERT_HEAD(&zh->zh_zopool, zo_left, zo_link);
				ldap_pvt_thread_mutex_unlock( &zh->zh_mutex );
				return((void*)ptr);
			} else {
				LDAP_LIST_INSERT_HEAD(
						&zh->zh_free[j-1-order_start], zo_right, zo_link);
				LDAP_LIST_INSERT_HEAD(
						&zh->zh_free[j-1-order_start], zo_left, zo_link);
			}
		}
		assert(0);
	} else {

		if ( zh->zh_maxzones < zh->zh_numzones + zh->zh_deltazones ) {
			ldap_pvt_thread_mutex_unlock( &zh->zh_mutex );
			Debug( LDAP_DEBUG_TRACE,
				"slap_zn_malloc of %lu bytes failed, using ch_malloc\n",
				(long)size, 0, 0);
			return (void*)ch_malloc(size);
		}

		for (i = zh->zh_numzones; i < zh->zh_numzones+zh->zh_deltazones; i++) {
			zh->zh_zones[i] = mmap(0, zh->zh_zonesize, PROT_READ | PROT_WRITE,
								MAP_PRIVATE, zh->zh_fd, 0);
			zh->zh_maps[i] = (unsigned char **)
						ch_malloc((zh->zh_zoneorder - order_start +1) *
						sizeof(unsigned char *));
			for (j = 0; j < order; j++) {
				int shiftamt = order_start + 1 + j;
				int nummaps = zh->zh_zonesize >> shiftamt;
				assert(nummaps);
				nummaps /= 8;
				if (!nummaps) nummaps = 1;
				zh->zh_maps[i][j] = (unsigned char *)ch_malloc(nummaps);
				memset(zh->zh_maps[i][j], 0, nummaps);
			}
	
			if (LDAP_LIST_EMPTY(&zh->zh_zopool)) {
				slap_replenish_zopool(zh);
			}
			zo = LDAP_LIST_FIRST(&zh->zh_zopool);
			LDAP_LIST_REMOVE(zo, zo_link);
			zo->zo_ptr = zh->zh_zones[i];
			zo->zo_idx = i;
			LDAP_LIST_INSERT_HEAD(&zh->
						zh_free[zh->zh_zoneorder-order_start],zo,zo_link);
	
			if (LDAP_LIST_EMPTY(&zh->zh_zopool)) {
				slap_replenish_zopool(zh);
			}
			zo = LDAP_LIST_FIRST(&zh->zh_zopool);
			LDAP_LIST_REMOVE(zo, zo_link);
			zo->zo_ptr = (void*)((unsigned long)zh->zh_zones[i]>>
						zh->zh_zoneorder);
			zo->zo_idx = i;
			avl_insert(&zh->zh_zonetree, zo, slap_zone_cmp, avl_dup_error);
		}
		zh->zh_numzones += zh->zh_deltazones;
		ldap_pvt_thread_mutex_unlock( &zh->zh_mutex );
		goto retry;
	}
}

void *
slap_zn_calloc( ber_len_t n, ber_len_t size, void *ctx )
{
	void *new;

	new = slap_zn_malloc( n*size, ctx );
	if ( new ) {
		memset( new, 0, n*size );
	}
	return new;
}

void *
slap_zn_realloc(void *ptr, ber_len_t size, void *ctx)
{
	struct zone_heap *zh = ctx;
	int pad = 2*sizeof(int)-1, pad_shift;
	int order_start = -1, order = -1;
	struct zone_object zoi, *zoo;
	ber_len_t *p = (ber_len_t *)ptr, *new;
	unsigned long diff;
	int i;
	void *newptr = NULL;
	struct zone_heap *zone = NULL;

	if (ptr == NULL)
		return slap_zn_malloc(size, zh);

	zoi.zo_ptr = (void*)((unsigned long)p >> zh->zh_zoneorder);
	zoi.zo_idx = -1;

	if (zh) {
		ldap_pvt_thread_mutex_lock( &zh->zh_mutex );
		zoo = avl_find(zh->zh_zonetree, &zoi, slap_zone_cmp);
		ldap_pvt_thread_mutex_unlock( &zh->zh_mutex );
	}

	/* Not our memory? */
	if (!zoo) {
		/* duplicate of realloc behavior, oh well */
		new = ber_memrealloc_x(ptr, size, NULL);
		if (new) {
			return new;
		}
		Debug(LDAP_DEBUG_ANY, "ch_realloc of %lu bytes failed\n",
				(long) size, 0, 0);
		assert(0);
		exit( EXIT_FAILURE );
	}

	assert(zoo->zo_idx != -1);	

	zone = zh->zh_zones[zoo->zo_idx];

	if (size == 0) {
		slap_zn_free(ptr, zh);
		return NULL;
	}

	newptr = slap_zn_malloc(size, zh);
	if (size < p[-1]) {
		AC_MEMCPY(newptr, ptr, size);
	} else {
		AC_MEMCPY(newptr, ptr, p[-1]);
	}
	slap_zn_free(ptr, zh);
	return newptr;
}

void
slap_zn_free(void *ptr, void *ctx)
{
	struct zone_heap *zh = ctx;
	int size, size_shift, order_size;
	int pad = 2*sizeof(int)-1, pad_shift;
	ber_len_t *p = (ber_len_t *)ptr, *tmpp;
	int order_start = -1, order = -1;
	struct zone_object zoi, *zoo, *zo;
	unsigned long diff;
	int i, k, inserted = 0, idx;
	struct zone_heap *zone = NULL;

	fprintf(stderr,"slap_zn_free... 0x%x\n", ptr);
	zoi.zo_ptr = (void*)((unsigned long)p >> zh->zh_zoneorder);
	zoi.zo_idx = -1;

	if (zh) {
		ldap_pvt_thread_mutex_lock( &zh->zh_mutex );
		zoo = avl_find(zh->zh_zonetree, &zoi, slap_zone_cmp);
		ldap_pvt_thread_mutex_unlock( &zh->zh_mutex );
	}

	if (!zoo) {
		ber_memfree_x(ptr, NULL);
	} else {
		idx = zoo->zo_idx;
		assert(idx != -1);
		zone = zh->zh_zones[idx];

		size = *(--p);
		size_shift = size + 2*sizeof(ber_len_t) - 1;
		do {
			order++;
		} while (size_shift >>= 1);

		pad_shift = pad - 1;
		do {
			order_start++;
		} while (pad_shift >>= 1);

		ldap_pvt_thread_mutex_lock( &zh->zh_mutex );
		for (i = order, tmpp = p; i <= zh->zh_zoneorder; i++) {
			order_size = 1 << (i+1);
			diff = (unsigned long)((char*)tmpp - (char*)zone) >> (i+1);
			zh->zh_maps[idx][i-order_start][diff>>3] &= (~(1 << (diff & 0x7)));
			if (diff == ((diff>>1)<<1)) {
				if (!(zh->zh_maps[idx][i-order_start][(diff+1)>>3] &
						(1<<((diff+1)&0x7)))) {
					zo = LDAP_LIST_FIRST(&zh->zh_free[i-order_start]);
					while (zo) {
						if ((char*)zo->zo_ptr == (char*)tmpp) {
							LDAP_LIST_REMOVE( zo, zo_link );
						} else if ((char*)zo->zo_ptr ==
								(char*)tmpp + order_size) {
							LDAP_LIST_REMOVE(zo, zo_link);
							break;
						}
						zo = LDAP_LIST_NEXT(zo, zo_link);
					}
					if (zo) {
						if (i < zh->zh_zoneorder) {
							inserted = 1;
							zo->zo_ptr = tmpp;
							LDAP_LIST_INSERT_HEAD(&zh->zh_free[i-order_start+1],
									zo, zo_link);
						}
						continue;
					} else {
						if (LDAP_LIST_EMPTY(&zh->zh_zopool)) {
							slap_replenish_zopool(zh);
						}
						zo = LDAP_LIST_FIRST(&zh->zh_zopool);
						LDAP_LIST_REMOVE(zo, zo_link);
						zo->zo_ptr = tmpp;
						LDAP_LIST_INSERT_HEAD(&zh->zh_free[i-order_start],
								zo, zo_link);
						break;

						Debug(LDAP_DEBUG_ANY, "slap_zn_free: "
							"free object not found while bit is clear.\n",
							0, 0, 0);
						assert(zo);

					}
				} else {
					if (!inserted) {
						if (LDAP_LIST_EMPTY(&zh->zh_zopool)) {
							slap_replenish_zopool(zh);
						}
						zo = LDAP_LIST_FIRST(&zh->zh_zopool);
						LDAP_LIST_REMOVE(zo, zo_link);
						zo->zo_ptr = tmpp;
						LDAP_LIST_INSERT_HEAD(&zh->zh_free[i-order_start],
								zo, zo_link);
					}
					break;
				}
			} else {
				if (!(zh->zh_maps[idx][i-order_start][(diff-1)>>3] &
						(1<<((diff-1)&0x7)))) {
					zo = LDAP_LIST_FIRST(&zh->zh_free[i-order_start]);
					while (zo) {
						if ((char*)zo->zo_ptr == (char*)tmpp) {
							LDAP_LIST_REMOVE(zo, zo_link);
						} else if ((char*)tmpp == zo->zo_ptr + order_size) {
							LDAP_LIST_REMOVE(zo, zo_link);
							tmpp = zo->zo_ptr;
							break;
						}
						zo = LDAP_LIST_NEXT(zo, zo_link);
					}
					if (zo) {
						if (i < zh->zh_zoneorder) {
							inserted = 1;
							LDAP_LIST_INSERT_HEAD(&zh->zh_free[i-order_start+1],
									zo, zo_link);
							continue;
						}
					} else {
						if (LDAP_LIST_EMPTY(&zh->zh_zopool)) {
							slap_replenish_zopool(zh);
						}
						zo = LDAP_LIST_FIRST(&zh->zh_zopool);
						LDAP_LIST_REMOVE(zo, zo_link);
						zo->zo_ptr = tmpp;
						LDAP_LIST_INSERT_HEAD(&zh->zh_free[i-order_start],
								zo, zo_link);
						break;

						Debug(LDAP_DEBUG_ANY, "slap_zn_free: "
							"free object not found while bit is clear.\n",
							0, 0, 0 );
						assert( zo );

					}
				} else {
					if ( !inserted ) {
						if (LDAP_LIST_EMPTY(&zh->zh_zopool)) {
							slap_replenish_zopool(zh);
						}
						zo = LDAP_LIST_FIRST(&zh->zh_zopool);
						LDAP_LIST_REMOVE(zo, zo_link);
						zo->zo_ptr = tmpp;
						LDAP_LIST_INSERT_HEAD(&zh->zh_free[i-order_start],
								zo, zo_link);
					}
					break;
				}
			}
		}
		ldap_pvt_thread_mutex_unlock( &zh->zh_mutex );
	}
}

static int
slap_zone_cmp(const void *v1, const void *v2)
{
	const struct zone_object *zo1 = v1;
	const struct zone_object *zo2 = v2;

	return zo1->zo_ptr - zo2->zo_ptr;
}

void *
slap_replenish_zopool(
	void *ctx
)
{
	struct zone_heap* zh = ctx;
	struct zone_object *zo_block;
	int i;

	zo_block = (struct zone_object *)ch_malloc(
					SLAP_ZONE_ZOBLOCK * sizeof(struct zone_object));

	if ( zo_block == NULL ) {
		return NULL;
	}

	zo_block[0].zo_blockhead = 1;
	LDAP_LIST_INSERT_HEAD(&zh->zh_zopool, &zo_block[0], zo_link);
	for (i = 1; i < SLAP_ZONE_ZOBLOCK; i++) {
		zo_block[i].zo_blockhead = 0;
		LDAP_LIST_INSERT_HEAD(&zh->zh_zopool, &zo_block[i], zo_link );
	}

	return zo_block;
}
#endif /* SLAP_ZONE_ALLOC */
