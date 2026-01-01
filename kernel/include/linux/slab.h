/* SPDX-License-Identifier: GPL-2.0 */
/**
 * Linux Compatibility Layer - Slab Allocator
 *
 * Provides Linux kernel memory allocation APIs mapped to EMBODIOS equivalents.
 * Reference: linux/include/linux/slab.h
 *
 * Part of EMBODIOS Linux Driver Compatibility Shim (~50 APIs)
 */

#ifndef _LINUX_SLAB_H
#define _LINUX_SLAB_H

#include <linux/types.h>
#include <embodios/mm.h>

/* ============================================================================
 * Internal EMBODIOS function wrappers
 * ============================================================================
 * These call the actual EMBODIOS functions before we redefine the names.
 */

/* Store references to the actual EMBODIOS functions */
static inline void *__embodios_kmalloc(size_t size)
{
    return kmalloc(size);
}

static inline void *__embodios_kzalloc(size_t size)
{
    return kzalloc(size);
}

static inline void *__embodios_krealloc(void *ptr, size_t size)
{
    return krealloc(ptr, size);
}

static inline void __embodios_kfree(void *ptr)
{
    kfree(ptr);
}

/* ============================================================================
 * Basic memory allocation - Direct mappings to EMBODIOS
 * ============================================================================ */

/*
 * __kmalloc - Internal kmalloc implementation
 * @size: how many bytes of memory are required
 * @flags: the type of memory to allocate (ignored in EMBODIOS, uses slab)
 *
 * Maps directly to EMBODIOS kmalloc() - flags are ignored as EMBODIOS
 * has a simpler single-pool allocator.
 */
static inline void *__kmalloc(size_t size, gfp_t flags)
{
    void *ptr;

    if (flags & GFP_ZERO)
        ptr = __embodios_kzalloc(size);
    else
        ptr = __embodios_kmalloc(size);

    return ptr;
}

/*
 * __kzalloc - Internal kzalloc implementation
 * @size: how many bytes of memory are required
 * @flags: the type of memory to allocate (ignored)
 *
 * Maps directly to EMBODIOS kzalloc()
 */
static inline void *__kzalloc(size_t size, gfp_t flags)
{
    (void)flags;  /* EMBODIOS ignores flags */
    return __embodios_kzalloc(size);
}

/*
 * __krealloc - Internal krealloc implementation
 * @p: pointer to the memory to reallocate
 * @new_size: new size in bytes
 * @flags: the type of memory to allocate (ignored)
 */
static inline void *__krealloc(const void *p, size_t new_size, gfp_t flags)
{
    (void)flags;
    return __embodios_krealloc((void *)p, new_size);
}

/*
 * __kfree - Internal kfree wrapper
 * @ptr: pointer to free
 */
static inline void __kfree(void *ptr)
{
    __embodios_kfree(ptr);
}

/* ============================================================================
 * Public API macros - Redefine Linux-style API
 * ============================================================================ */

/* Undefine EMBODIOS names to redefine with Linux signatures */
#undef kmalloc
#undef kzalloc
#undef krealloc
#undef kfree

/* Standard kmalloc with flags */
#define kmalloc(size, flags) __kmalloc(size, flags)

/* Standard kzalloc with flags */
#define kzalloc(size, flags) __kzalloc(size, flags)

/* Standard krealloc with flags */
#define krealloc(p, new_size, flags) __krealloc(p, new_size, flags)

/* kfree wrapper */
#define kfree(ptr) __kfree(ptr)

/*
 * kcalloc - Allocate zeroed array
 * @n: number of elements
 * @size: size of each element
 * @flags: the type of memory to allocate (ignored)
 *
 * Allocates memory for an array of @n elements of @size bytes each,
 * all initialized to zero.
 */
static inline void *kcalloc(size_t n, size_t size, gfp_t flags)
{
    size_t total;

    /* Overflow check */
    if (size != 0 && n > (size_t)-1 / size)
        return NULL;

    total = n * size;
    return __kzalloc(total, flags);
}

/*
 * kmalloc_array - Allocate array without zeroing
 * @n: number of elements
 * @size: size of each element
 * @flags: the type of memory to allocate
 */
static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
    size_t total;

    /* Overflow check */
    if (size != 0 && n > (size_t)-1 / size)
        return NULL;

    total = n * size;
    return __kmalloc(total, flags);
}

/* krealloc macro is defined above */

/*
 * kfree - Free memory
 * @objp: pointer returned by kmalloc
 *
 * Maps directly to EMBODIOS kfree()
 */
/* kfree is already defined in embodios/mm.h, so we just use it */

/*
 * kfree_sensitive - Free memory and clear it first
 * @p: pointer to memory to free
 *
 * Clears memory before freeing to prevent data leaks
 */
static inline void kfree_sensitive(const void *p)
{
    /* We don't know the size, so we can't memset. Just free. */
    /* In a full implementation, we'd track allocation sizes. */
    __kfree((void *)p);
}

/* ============================================================================
 * Size query functions
 * ============================================================================ */

/*
 * ksize - Get actual allocation size
 * @objp: pointer to the object
 *
 * Returns the actual size of the allocation.
 * EMBODIOS doesn't track this, so return 0.
 */
static inline size_t ksize(const void *objp)
{
    (void)objp;
    return 0;  /* Unknown - EMBODIOS doesn't track allocation sizes */
}

/* ============================================================================
 * Kmem cache API (simplified)
 * ============================================================================
 *
 * Linux kmem_cache provides slab caches for specific object sizes.
 * EMBODIOS uses a simplified allocator, so we emulate this with
 * regular kmalloc/kfree.
 */

struct kmem_cache {
    const char *name;
    size_t size;
    size_t align;
    unsigned long flags;
    void (*ctor)(void *);
};

/* Cache creation flags */
#define SLAB_HWCACHE_ALIGN  0x00000001
#define SLAB_PANIC          0x00000002
#define SLAB_RECLAIM_ACCOUNT 0x00000004
#define SLAB_TEMPORARY      0x00000008

/*
 * kmem_cache_create - Create a slab cache
 * @name: cache name for debugging
 * @size: object size
 * @align: alignment requirement
 * @flags: cache flags
 * @ctor: constructor function (called on allocation)
 *
 * Creates a cache descriptor (objects are still allocated via kmalloc)
 */
static inline struct kmem_cache *kmem_cache_create(
    const char *name, size_t size, size_t align,
    unsigned long flags, void (*ctor)(void *))
{
    struct kmem_cache *cache;

    cache = (struct kmem_cache *)__embodios_kmalloc(sizeof(*cache));
    if (!cache)
        return NULL;

    cache->name = name;
    cache->size = size;
    cache->align = align;
    cache->flags = flags;
    cache->ctor = ctor;

    return cache;
}

/*
 * kmem_cache_destroy - Destroy a slab cache
 * @cachep: cache to destroy
 */
static inline void kmem_cache_destroy(struct kmem_cache *cachep)
{
    if (cachep)
        __embodios_kfree(cachep);
}

/*
 * kmem_cache_alloc - Allocate from cache
 * @cachep: cache to allocate from
 * @flags: allocation flags
 */
static inline void *kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
    void *obj;

    if (!cachep)
        return NULL;

    if (flags & GFP_ZERO)
        obj = __embodios_kzalloc(cachep->size);
    else
        obj = __embodios_kmalloc(cachep->size);

    if (obj && cachep->ctor)
        cachep->ctor(obj);

    return obj;
}

/*
 * kmem_cache_zalloc - Allocate zeroed from cache
 * @cachep: cache to allocate from
 * @flags: allocation flags
 */
static inline void *kmem_cache_zalloc(struct kmem_cache *cachep, gfp_t flags)
{
    return kmem_cache_alloc(cachep, flags | GFP_ZERO);
}

/*
 * kmem_cache_free - Free object back to cache
 * @cachep: cache the object came from
 * @objp: object to free
 */
static inline void kmem_cache_free(struct kmem_cache *cachep, void *objp)
{
    (void)cachep;  /* Not needed for simple kfree */
    __embodios_kfree(objp);
}

/* ============================================================================
 * String duplication
 * ============================================================================ */

/*
 * kstrdup - Duplicate a string
 * @s: string to duplicate
 * @gfp: allocation flags
 */
static inline char *kstrdup(const char *s, gfp_t gfp)
{
    size_t len;
    char *buf;
    const char *p;

    if (!s)
        return NULL;

    /* Calculate length */
    for (p = s, len = 0; *p; p++, len++)
        ;
    len++;  /* Include null terminator */

    buf = (char *)__kmalloc(len, gfp);
    if (buf) {
        for (size_t i = 0; i < len; i++)
            buf[i] = s[i];
    }

    return buf;
}

/*
 * kstrndup - Duplicate a string with length limit
 * @s: string to duplicate
 * @max: maximum length
 * @gfp: allocation flags
 */
static inline char *kstrndup(const char *s, size_t max, gfp_t gfp)
{
    size_t len;
    char *buf;
    const char *p;

    if (!s)
        return NULL;

    /* Calculate length (up to max) */
    for (p = s, len = 0; *p && len < max; p++, len++)
        ;

    buf = (char *)__kmalloc(len + 1, gfp);
    if (buf) {
        for (size_t i = 0; i < len; i++)
            buf[i] = s[i];
        buf[len] = '\0';
    }

    return buf;
}

/*
 * kmemdup - Duplicate memory region
 * @src: source memory
 * @len: length to copy
 * @gfp: allocation flags
 */
static inline void *kmemdup(const void *src, size_t len, gfp_t gfp)
{
    void *p;

    p = __kmalloc(len, gfp);
    if (p)
        memcpy(p, src, len);

    return p;
}

/* ============================================================================
 * Convenience allocation macros
 * ============================================================================ */

/* Allocate structure */
#define kzalloc_node(size, flags, node) kzalloc(size, flags)
#define kmalloc_node(size, flags, node) __kmalloc(size, flags)

/* Allocate with specific NUMA node (ignored in EMBODIOS) */
#define kmem_cache_alloc_node(cache, flags, node) \
    kmem_cache_alloc(cache, flags)

/* Deferred free (immediate in EMBODIOS) */
#define kfree_rcu(ptr, rcu_head) kfree(ptr)
#define kvfree(ptr) kfree(ptr)
#define kvfree_sensitive(ptr) kfree_sensitive(ptr)

/* Large allocations (use same allocator in EMBODIOS) */
#define kvmalloc(size, flags) __kmalloc(size, flags)
#define kvzalloc(size, flags) __kzalloc(size, flags)
#define kvmalloc_array(n, size, flags) kmalloc_array(n, size, flags)
#define kvcalloc(n, size, flags) kcalloc(n, size, flags)

#endif /* _LINUX_SLAB_H */
