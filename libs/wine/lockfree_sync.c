/*
 * Lock-free multi-word compare-and-swap for NT synchronization.
 *
 * Copyright 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "wine/lockfree_sync.h"

#define LF_DESC_BIT       (UINT64_C(1) << 63)
#define LF_DESC_INDEX_BITS 20
#define LF_DESC_INDEX_MASK ((UINT64_C(1) << LF_DESC_INDEX_BITS) - 1)
#define LF_DESC_GEN_MASK   ((UINT64_C(1) << (63 - LF_DESC_INDEX_BITS)) - 1)

#define LF_CONTROL_STATUS_MASK UINT64_C(3)
#define LF_CONTROL_GEN_SHIFT   2

#define LF_LIFETIME_REF_MASK UINT64_C(0xffffffff)
#define LF_LIFETIME_GEN_SHIFT 32

static uint64_t load_u64( const uint64_t *ptr )
{
    return __atomic_load_n( ptr, __ATOMIC_ACQUIRE );
}

static int cas_u64( uint64_t *ptr, uint64_t *expected, uint64_t desired )
{
    return __atomic_compare_exchange_n( ptr, expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE );
}

static uint64_t desc_tag( uint32_t index, uint32_t generation )
{
    return LF_DESC_BIT | ((uint64_t)generation << LF_DESC_INDEX_BITS) | index;
}

static uint32_t tag_index( uint64_t tag )
{
    return tag & LF_DESC_INDEX_MASK;
}

static uint32_t tag_generation( uint64_t tag )
{
    return (tag >> LF_DESC_INDEX_BITS) & LF_DESC_GEN_MASK;
}

static uint64_t make_control( uint32_t generation, enum lf_sync_mcas_status status )
{
    return ((uint64_t)generation << LF_CONTROL_GEN_SHIFT) | status;
}

static enum lf_sync_mcas_status control_status( uint64_t control )
{
    return control & LF_CONTROL_STATUS_MASK;
}

static uint32_t control_generation( uint64_t control )
{
    return control >> LF_CONTROL_GEN_SHIFT;
}

static int acquire_desc( struct lf_sync_mcas *desc, uint32_t generation )
{
    uint64_t lifetime, desired;
    uint32_t refs;

    for (;;)
    {
        lifetime = load_u64( &desc->lifetime );
        if ((lifetime >> LF_LIFETIME_GEN_SHIFT) != generation) return 0;
        refs = lifetime & LF_LIFETIME_REF_MASK;
        if (!refs || refs == UINT32_MAX) return 0;
        desired = lifetime + 1;
        if (cas_u64( &desc->lifetime, &lifetime, desired )) return 1;
    }
}

static void release_desc( struct lf_sync_mcas *desc )
{
    uint64_t previous = __atomic_fetch_sub( &desc->lifetime, 1, __ATOMIC_RELEASE );
    assert( (previous & LF_LIFETIME_REF_MASK) != 0 );
}

static enum lf_sync_mcas_status help_mcas( const struct lf_sync_arena *arena,
                                           uint32_t index, uint32_t generation );

static enum lf_sync_mcas_status help_mcas_acquired( const struct lf_sync_arena *arena,
                                                    uint32_t index, uint32_t generation )
{
    struct lf_sync_mcas *desc;
    uint64_t active, control, tag;
    uint32_t i, count;

    if (!generation || index >= arena->desc_count) return LF_SYNC_MCAS_ABORTED;

    desc = &arena->descs[index];
    active = make_control( generation, LF_SYNC_MCAS_ACTIVE );
    control = load_u64( &desc->control );
    if (control_generation( control ) != generation) return LF_SYNC_MCAS_ABORTED;

    count = __atomic_load_n( &desc->count, __ATOMIC_ACQUIRE );
    if (!count || count > LF_SYNC_MCAS_MAX_WORDS) return LF_SYNC_MCAS_ABORTED;
    tag = desc_tag( index, generation );

    for (i = 0; i < count; ++i)
    {
        const struct lf_sync_mcas_entry *entry = &desc->entries[i];
        uint64_t value;

        if (entry->word >= arena->word_count) break;

        for (;;)
        {
            value = load_u64( &arena->words[entry->word].value );
            if (value == tag) break;

            if (value & LF_DESC_BIT)
            {
                help_mcas( arena, tag_index( value ), tag_generation( value ) );
                continue;
            }

            if (value != entry->expected) break;
            if (cas_u64( &arena->words[entry->word].value, &value, tag ))
            {
                value = tag;
                break;
            }
        }

        if (value != tag) break;
        if (load_u64( &desc->control ) != active) break;
    }

    control = active;
    cas_u64( &desc->control, &control,
             make_control( generation, i == count ? LF_SYNC_MCAS_COMMITTED : LF_SYNC_MCAS_ABORTED ) );
    control = load_u64( &desc->control );

    /* A descriptor owner may reuse its slot after this function returns.
     * Snapshot each entry before touching its target, and only replace our
     * generation's exact tag. A stale helper can therefore do no damage. */
    for (i = 0; i < count; ++i)
    {
        struct lf_sync_mcas_entry entry = desc->entries[i];
        uint64_t expected = tag;
        uint64_t value = control_status( control ) == LF_SYNC_MCAS_COMMITTED ? entry.desired : entry.expected;

        if (entry.word < arena->word_count) cas_u64( &arena->words[entry.word].value, &expected, value );
    }

    return control_status( control );
}

static enum lf_sync_mcas_status help_mcas( const struct lf_sync_arena *arena,
                                           uint32_t index, uint32_t generation )
{
    struct lf_sync_mcas *desc;
    enum lf_sync_mcas_status status;

    if (!generation || index >= arena->desc_count) return LF_SYNC_MCAS_ABORTED;
    desc = &arena->descs[index];
    if (!acquire_desc( desc, generation )) return LF_SYNC_MCAS_ABORTED;
    status = help_mcas_acquired( arena, index, generation );
    release_desc( desc );
    return status;
}

int lf_sync_value_is_valid( uint64_t value )
{
    return !(value & LF_DESC_BIT);
}

int lf_sync_is_lock_free(void)
{
    return __atomic_always_lock_free( sizeof(uint64_t), 0 );
}

uint64_t lf_sync_load( const struct lf_sync_arena *arena, uint32_t word )
{
    uint64_t value;

    assert( word < arena->word_count );
    for (;;)
    {
        value = load_u64( &arena->words[word].value );
        if (!(value & LF_DESC_BIT)) return value;
        help_mcas( arena, tag_index( value ), tag_generation( value ) );
    }
}

int lf_sync_compare_exchange( const struct lf_sync_arena *arena, uint32_t word,
                              uint64_t expected, uint64_t desired )
{
    uint64_t value;

    if (word >= arena->word_count || !lf_sync_value_is_valid( expected ) ||
        !lf_sync_value_is_valid( desired )) return 0;

    for (;;)
    {
        value = load_u64( &arena->words[word].value );
        if (value & LF_DESC_BIT)
        {
            help_mcas( arena, tag_index( value ), tag_generation( value ) );
            continue;
        }
        if (value != expected) return 0;
        return cas_u64( &arena->words[word].value, &value, desired );
    }
}

static int alloc_desc( const struct lf_sync_arena *arena, uint32_t *index, uint32_t *generation )
{
    uint32_t i;

    for (i = 0; i < arena->desc_count && i <= LF_DESC_INDEX_MASK; ++i)
    {
        struct lf_sync_mcas *desc = &arena->descs[i];
        uint64_t lifetime = load_u64( &desc->lifetime ), desired;
        uint32_t next_generation;

        if (lifetime & LF_LIFETIME_REF_MASK) continue;
        next_generation = (lifetime >> LF_LIFETIME_GEN_SHIFT) + 1;
        if (!next_generation) next_generation = 1;
        desired = ((uint64_t)next_generation << LF_LIFETIME_GEN_SHIFT) | 1;
        if (!cas_u64( &desc->lifetime, &lifetime, desired )) continue;
        *index = i;
        *generation = next_generation;
        return 1;
    }
    return 0;
}

int lf_sync_mcas( const struct lf_sync_arena *arena,
                  const struct lf_sync_mcas_entry *entries, uint32_t count )
{
    struct lf_sync_mcas *desc;
    enum lf_sync_mcas_status status;
    uint32_t generation, i, index;

    if (!count || count > LF_SYNC_MCAS_MAX_WORDS)
        return 0;

    for (i = 0; i < count; ++i)
    {
        if (entries[i].word >= arena->word_count || !lf_sync_value_is_valid( entries[i].expected ) ||
            !lf_sync_value_is_valid( entries[i].desired ) || (i && entries[i - 1].word >= entries[i].word))
            return 0;
    }

    if (!alloc_desc( arena, &index, &generation )) return -1;
    desc = &arena->descs[index];

    memcpy( desc->entries, entries, count * sizeof(*entries) );
    __atomic_store_n( &desc->count, count, __ATOMIC_RELAXED );
    __atomic_store_n( &desc->control, make_control( generation, LF_SYNC_MCAS_ACTIVE ), __ATOMIC_RELEASE );

    status = help_mcas_acquired( arena, index, generation );
    release_desc( desc );
    return status == LF_SYNC_MCAS_COMMITTED;
}

#define LF_MUTEX_OWNER_MASK UINT64_C(0xffffffff)
#define LF_MUTEX_COUNT_SHIFT 32
#define LF_MUTEX_COUNT_MASK UINT64_C(0x7fffffff)
#define LF_MUTEX_ABANDONED_OWNER UINT32_MAX

static uint64_t mutex_value( uint32_t owner, uint32_t count, int abandoned )
{
    if (abandoned) return LF_MUTEX_ABANDONED_OWNER;
    return owner | ((uint64_t)count << LF_MUTEX_COUNT_SHIFT);
}

static uint32_t mutex_owner( uint64_t value )
{
    return value & LF_MUTEX_OWNER_MASK;
}

static uint32_t mutex_count( uint64_t value )
{
    return (value >> LF_MUTEX_COUNT_SHIFT) & LF_MUTEX_COUNT_MASK;
}

static int mutex_abandoned( uint64_t value )
{
    return !mutex_count( value ) && mutex_owner( value ) == LF_MUTEX_ABANDONED_OWNER;
}

static void init_object( const struct lf_sync_arena *arena, struct lf_sync_object *object,
                         uint32_t word, enum lf_sync_object_type type, uint32_t limit,
                         uint32_t flags, uint64_t value )
{
    assert( word < arena->word_count );
    object->word = word;
    object->type = type;
    object->limit = limit;
    object->flags = flags;
    memset( object->waiters, 0, sizeof(object->waiters) );
    __atomic_store_n( &object->pulse, 0, __ATOMIC_RELEASE );
    __atomic_store_n( &arena->words[word].value, value, __ATOMIC_RELEASE );
}

void lf_sync_init_event( const struct lf_sync_arena *arena, struct lf_sync_object *object,
                         uint32_t word, int manual, int signaled )
{
    init_object( arena, object, word, LF_SYNC_EVENT, 0, manual ? LF_SYNC_EVENT_MANUAL : 0, !!signaled );
}

void lf_sync_init_semaphore( const struct lf_sync_arena *arena, struct lf_sync_object *object,
                             uint32_t word, uint32_t initial, uint32_t maximum )
{
    assert( maximum && initial <= maximum );
    init_object( arena, object, word, LF_SYNC_SEMAPHORE, maximum, 0, initial );
}

void lf_sync_init_mutex( const struct lf_sync_arena *arena, struct lf_sync_object *object,
                         uint32_t word, uint32_t owner, uint32_t count )
{
    assert( count <= LF_MUTEX_COUNT_MASK );
    assert( (!owner && !count) || (owner && owner != LF_MUTEX_ABANDONED_OWNER && count) );
    init_object( arena, object, word, LF_SYNC_MUTEX, LF_MUTEX_COUNT_MASK, 0,
                 mutex_value( owner, count, 0 ) );
}

static enum lf_sync_result update_event( const struct lf_sync_arena *arena,
                                         const struct lf_sync_object *object, int signaled,
                                         uint32_t *previous )
{
    uint64_t value;

    if (object->type != LF_SYNC_EVENT) return LF_SYNC_INVALID;
    for (;;)
    {
        value = lf_sync_load( arena, object->word );
        if (previous) *previous = !!value;
        if (value == !!signaled || lf_sync_compare_exchange( arena, object->word, value, !!signaled ))
            return LF_SYNC_SUCCESS;
    }
}

enum lf_sync_result lf_sync_set_event( const struct lf_sync_arena *arena,
                                       const struct lf_sync_object *object, uint32_t *previous )
{
    return update_event( arena, object, 1, previous );
}

enum lf_sync_result lf_sync_reset_event( const struct lf_sync_arena *arena,
                                         const struct lf_sync_object *object, uint32_t *previous )
{
    return update_event( arena, object, 0, previous );
}

enum lf_sync_result lf_sync_release_semaphore( const struct lf_sync_arena *arena,
                                               const struct lf_sync_object *object, uint32_t count,
                                               uint32_t *previous )
{
    uint64_t value;

    if (object->type != LF_SYNC_SEMAPHORE || !count) return LF_SYNC_INVALID;
    for (;;)
    {
        value = lf_sync_load( arena, object->word );
        if (value > object->limit || count > object->limit - value) return LF_SYNC_LIMIT_EXCEEDED;
        if (previous) *previous = value;
        if (lf_sync_compare_exchange( arena, object->word, value, value + count )) return LF_SYNC_SUCCESS;
    }
}

enum lf_sync_result lf_sync_release_mutex( const struct lf_sync_arena *arena,
                                           const struct lf_sync_object *object, uint32_t owner,
                                           uint32_t *previous )
{
    uint64_t value, desired;
    uint32_t count;

    if (object->type != LF_SYNC_MUTEX || !owner || owner == LF_MUTEX_ABANDONED_OWNER)
        return LF_SYNC_INVALID;
    for (;;)
    {
        value = lf_sync_load( arena, object->word );
        count = mutex_count( value );
        if (!count || mutex_owner( value ) != owner) return LF_SYNC_NOT_OWNER;
        if (previous) *previous = count;
        desired = count == 1 ? 0 : mutex_value( owner, count - 1, 0 );
        if (lf_sync_compare_exchange( arena, object->word, value, desired )) return LF_SYNC_SUCCESS;
    }
}

enum lf_sync_result lf_sync_abandon_mutex( const struct lf_sync_arena *arena,
                                           const struct lf_sync_object *object, uint32_t owner )
{
    uint64_t value;

    if (object->type != LF_SYNC_MUTEX || !owner || owner == LF_MUTEX_ABANDONED_OWNER)
        return LF_SYNC_INVALID;
    for (;;)
    {
        value = lf_sync_load( arena, object->word );
        if (!mutex_count( value ) || mutex_owner( value ) != owner) return LF_SYNC_NOT_OWNER;
        if (lf_sync_compare_exchange( arena, object->word, value, mutex_value( 0, 0, 1 ) ))
            return LF_SYNC_SUCCESS;
    }
}

uint32_t lf_sync_abandon_owned_mutexes( const struct lf_sync_dispatcher *dispatcher,
                                        uint32_t owner )
{
    uint32_t count, i, abandoned = 0;

    if (!dispatcher->shared || !owner || owner == LF_MUTEX_ABANDONED_OWNER) return 0;
    count = __atomic_load_n( &dispatcher->shared->next_object, __ATOMIC_ACQUIRE );
    if (count > dispatcher->object_count) count = dispatcher->object_count;

    for (i = 0; i < count; ++i)
    {
        const struct lf_sync_object *object = &dispatcher->objects[i];

        if (object->type == LF_SYNC_MUTEX &&
            lf_sync_abandon_mutex( &dispatcher->arena, object, owner ) == LF_SYNC_SUCCESS)
        {
            ++abandoned;
            lf_sync_wake_object( dispatcher, i );
        }
    }
    return abandoned;
}

enum lf_sync_result lf_sync_query_event( const struct lf_sync_arena *arena,
                                         const struct lf_sync_object *object,
                                         uint32_t *manual, uint32_t *signaled )
{
    uint64_t value;

    if (object->type != LF_SYNC_EVENT) return LF_SYNC_INVALID;
    value = lf_sync_load( arena, object->word );
    if (manual) *manual = !!(object->flags & LF_SYNC_EVENT_MANUAL);
    if (signaled) *signaled = !!value;
    return LF_SYNC_SUCCESS;
}

enum lf_sync_result lf_sync_query_semaphore( const struct lf_sync_arena *arena,
                                             const struct lf_sync_object *object,
                                             uint32_t *count, uint32_t *maximum )
{
    if (object->type != LF_SYNC_SEMAPHORE) return LF_SYNC_INVALID;
    if (count) *count = lf_sync_load( arena, object->word );
    if (maximum) *maximum = object->limit;
    return LF_SYNC_SUCCESS;
}

enum lf_sync_result lf_sync_query_mutex( const struct lf_sync_arena *arena,
                                         const struct lf_sync_object *object, uint32_t owner,
                                         uint32_t *count, uint32_t *owned, uint32_t *abandoned )
{
    uint64_t value;

    if (object->type != LF_SYNC_MUTEX) return LF_SYNC_INVALID;
    value = lf_sync_load( arena, object->word );
    if (count) *count = mutex_count( value );
    if (owned) *owned = mutex_owner( value ) == owner && mutex_count( value );
    if (abandoned) *abandoned = mutex_abandoned( value );
    return LF_SYNC_SUCCESS;
}

static enum lf_sync_result prepare_acquire( const struct lf_sync_arena *arena,
                                            const struct lf_sync_object *object, uint32_t owner,
                                            struct lf_sync_mcas_entry *entry )
{
    uint64_t value = lf_sync_load( arena, object->word );
    uint32_t count;

    entry->word = object->word;
    entry->pad = 0;
    entry->expected = value;

    switch (object->type)
    {
    case LF_SYNC_EVENT:
        if (!value) return LF_SYNC_UNSATISFIED;
        entry->desired = object->flags & LF_SYNC_EVENT_MANUAL ? value : 0;
        return LF_SYNC_SUCCESS;
    case LF_SYNC_SEMAPHORE:
        if (!value) return LF_SYNC_UNSATISFIED;
        entry->desired = value - 1;
        return LF_SYNC_SUCCESS;
    case LF_SYNC_MUTEX:
        count = mutex_count( value );
        if (count && mutex_owner( value ) != owner) return LF_SYNC_UNSATISFIED;
        if (count == LF_MUTEX_COUNT_MASK) return LF_SYNC_LIMIT_EXCEEDED;
        entry->desired = mutex_value( owner, count + 1, 0 );
        return mutex_abandoned( value ) ? LF_SYNC_ABANDONED : LF_SYNC_SUCCESS;
    default:
        return LF_SYNC_INVALID;
    }
}

static void sort_entries( struct lf_sync_mcas_entry *entries, uint32_t *indices, uint32_t count )
{
    uint32_t i;

    for (i = 1; i < count; ++i)
    {
        struct lf_sync_mcas_entry entry = entries[i];
        uint32_t index = indices[i], j = i;

        while (j && entries[j - 1].word > entry.word)
        {
            entries[j] = entries[j - 1];
            indices[j] = indices[j - 1];
            --j;
        }
        entries[j] = entry;
        indices[j] = index;
    }
}

uint64_t lf_sync_wait_value( uint64_t generation, enum lf_sync_wait_status status, uint32_t index )
{
    return (generation << LF_SYNC_WAIT_STATUS_BITS) | ((index & 0xff) << 8) | status;
}

static enum lf_sync_result try_wait( const struct lf_sync_arena *arena,
                                     const struct lf_sync_object *const *objects,
                                     uint32_t count, int wait_all, uint32_t owner, uint32_t *index,
                                     uint32_t status_word, uint64_t waiting )
{
    struct lf_sync_mcas_entry entries[LF_SYNC_MCAS_MAX_WORDS];
    uint32_t indices[LF_SYNC_MCAS_MAX_WORDS];
    enum lf_sync_result result, final_result;
    int mcas;
    uint32_t i;

    if (!count || count > 64 || !owner || owner == LF_MUTEX_ABANDONED_OWNER)
        return LF_SYNC_INVALID;

    if (!wait_all)
    {
        for (i = 0; i < count; ++i)
        {
            result = prepare_acquire( arena, objects[i], owner, entries );
            if (result == LF_SYNC_UNSATISFIED) continue;
            if (result != LF_SYNC_SUCCESS && result != LF_SYNC_ABANDONED) return result;
            if (status_word < arena->word_count)
            {
                entries[1].word = status_word;
                entries[1].pad = 0;
                entries[1].expected = waiting;
                entries[1].desired = lf_sync_wait_value( waiting >> LF_SYNC_WAIT_STATUS_BITS,
                    result == LF_SYNC_ABANDONED ? LF_SYNC_WAIT_ABANDONED : LF_SYNC_WAIT_COMPLETE, i );
                indices[0] = i;
                indices[1] = count;
                sort_entries( entries, indices, 2 );
            }
            mcas = lf_sync_mcas( arena, entries, status_word < arena->word_count ? 2 : 1 );
            if (mcas < 0) return LF_SYNC_RETRY;
            if (mcas)
            {
                if (index) *index = i;
                return result;
            }
        }
        return LF_SYNC_UNSATISFIED;
    }

    final_result = LF_SYNC_SUCCESS;
    for (i = 0; i < count; ++i)
    {
        indices[i] = i;
        result = prepare_acquire( arena, objects[i], owner, &entries[i] );
        if (result == LF_SYNC_ABANDONED) final_result = LF_SYNC_ABANDONED;
        else if (result != LF_SYNC_SUCCESS) return result;
    }
    if (status_word < arena->word_count)
    {
        entries[count].word = status_word;
        entries[count].pad = 0;
        entries[count].expected = waiting;
        entries[count].desired = lf_sync_wait_value( waiting >> LF_SYNC_WAIT_STATUS_BITS,
            final_result == LF_SYNC_ABANDONED ? LF_SYNC_WAIT_ABANDONED : LF_SYNC_WAIT_COMPLETE, 0 );
        indices[count] = count;
        ++count;
    }
    sort_entries( entries, indices, count );
    for (i = 1; i < count; ++i) if (entries[i - 1].word == entries[i].word) return LF_SYNC_INVALID;
    mcas = lf_sync_mcas( arena, entries, count );
    if (mcas < 0) return LF_SYNC_RETRY;
    if (!mcas) return LF_SYNC_UNSATISFIED;
    if (index) *index = 0;
    return final_result;
}

enum lf_sync_result lf_sync_try_wait( const struct lf_sync_arena *arena,
                                      const struct lf_sync_object *const *objects,
                                      uint32_t count, int wait_all, uint32_t owner, uint32_t *index )
{
    return try_wait( arena, objects, count, wait_all, owner, index,
                     arena->word_count, 0 );
}

enum lf_sync_result lf_sync_try_wait_status( const struct lf_sync_arena *arena,
                                             const struct lf_sync_object *const *objects,
                                             uint32_t count, int wait_all, uint32_t owner,
                                             uint32_t status_word, uint64_t waiting )
{
    if (status_word >= arena->word_count || (waiting & 0xff) != LF_SYNC_WAITING) return LF_SYNC_INVALID;
    return try_wait( arena, objects, count, wait_all, owner, NULL,
                     status_word, waiting );
}

static int acquire_wait( struct lf_sync_wait *wait, uint32_t generation )
{
    uint64_t lifetime, desired;
    uint32_t refs;

    for (;;)
    {
        lifetime = load_u64( &wait->lifetime );
        if ((lifetime >> LF_LIFETIME_GEN_SHIFT) != generation) return 0;
        refs = lifetime & LF_LIFETIME_REF_MASK;
        if (!refs || refs == UINT32_MAX) return 0;
        desired = lifetime + 1;
        if (cas_u64( &wait->lifetime, &lifetime, desired )) return 1;
    }
}

static void release_wait( struct lf_sync_wait *wait )
{
    uint64_t previous = __atomic_fetch_sub( &wait->lifetime, 1, __ATOMIC_RELEASE );
    assert( (previous & LF_LIFETIME_REF_MASK) != 0 );
}

static void register_object_wait( struct lf_sync_object *object, uint32_t slot )
{
    __atomic_fetch_or( &object->waiters[slot / 64], UINT64_C(1) << (slot % 64), __ATOMIC_RELEASE );
}

static void unregister_object_wait( struct lf_sync_object *object, uint32_t slot )
{
    __atomic_fetch_and( &object->waiters[slot / 64], ~(UINT64_C(1) << (slot % 64)), __ATOMIC_RELEASE );
}

static void unregister_wait( const struct lf_sync_dispatcher *dispatcher,
                             const struct lf_sync_wait *wait, uint32_t slot )
{
    uint32_t i;

    for (i = 0; i < wait->count; ++i)
        if (wait->objects[i] < dispatcher->object_count)
            unregister_object_wait( &dispatcher->objects[wait->objects[i]], slot );
    if (wait->alert_object < dispatcher->object_count)
        unregister_object_wait( &dispatcher->objects[wait->alert_object], slot );
}

static int pulse_wait( const struct lf_sync_dispatcher *dispatcher, uint32_t slot,
                       uint32_t generation, uint32_t pulsed_object, uint64_t pulse_generation );

static int alloc_wait( const struct lf_sync_dispatcher *dispatcher, uint32_t *slot, uint32_t *generation )
{
    uint32_t i;

    for (i = 0; i < dispatcher->wait_count; ++i)
    {
        struct lf_sync_wait *wait = &dispatcher->waits[i];
        uint64_t lifetime = load_u64( &wait->lifetime ), desired;
        uint32_t next_generation;

        if (lifetime & LF_LIFETIME_REF_MASK) continue;
        next_generation = (lifetime >> LF_LIFETIME_GEN_SHIFT) + 1;
        if (!next_generation) next_generation = 1;
        desired = ((uint64_t)next_generation << LF_LIFETIME_GEN_SHIFT) | 1;
        if (!cas_u64( &wait->lifetime, &lifetime, desired )) continue;
        *slot = i;
        *generation = next_generation;
        return 1;
    }
    return 0;
}

static void retry_wait( const struct lf_sync_dispatcher *dispatcher, uint32_t slot, uint32_t generation )
{
    const struct lf_sync_object *objects[LF_SYNC_MAX_WAIT_OBJECTS];
    struct lf_sync_mcas_entry alert_entries[2];
    struct lf_sync_wait *wait;
    uint32_t indices[2] = {0, 1}, i;
    enum lf_sync_result result;
    int mcas;

    if (slot >= dispatcher->wait_count) return;
    wait = &dispatcher->waits[slot];
    if (!acquire_wait( wait, generation )) return;
    if (load_u64( &wait->published ) != generation) goto done;
    if (!wait->count || wait->count > LF_SYNC_MAX_WAIT_OBJECTS) goto done;

    for (i = 0; i < wait->count; ++i)
    {
        if (wait->objects[i] >= dispatcher->object_count) goto done;
        objects[i] = &dispatcher->objects[wait->objects[i]];
    }

    if (wait->alert_object < dispatcher->object_count)
    {
        result = prepare_acquire( &dispatcher->arena, &dispatcher->objects[wait->alert_object],
                                  wait->owner, &alert_entries[0] );
        if (result == LF_SYNC_SUCCESS || result == LF_SYNC_ABANDONED)
        {
            alert_entries[1].word = dispatcher->status_word_base + slot;
            alert_entries[1].pad = 0;
            alert_entries[1].expected = lf_sync_wait_value( generation, LF_SYNC_WAITING, 0 );
            alert_entries[1].desired = lf_sync_wait_value( generation, LF_SYNC_WAIT_ALERTED, 0 );
            sort_entries( alert_entries, indices, 2 );
            do { mcas = lf_sync_mcas( &dispatcher->arena, alert_entries, 2 ); }
            while (mcas < 0 && load_u64( &wait->published ) == generation);
        }
    }

    while ((result = lf_sync_try_wait_status( &dispatcher->arena, objects, wait->count, wait->wait_all,
                                              wait->owner, dispatcher->status_word_base + slot,
                                              lf_sync_wait_value( generation, LF_SYNC_WAITING, 0 ) )) == LF_SYNC_RETRY)
        if (load_u64( &wait->published ) != generation) break;

    if (result == LF_SYNC_LIMIT_EXCEEDED || result == LF_SYNC_INVALID)
    {
        uint64_t waiting = lf_sync_wait_value( generation, LF_SYNC_WAITING, 0 );
        uint64_t failed = lf_sync_wait_value( generation,
            result == LF_SYNC_LIMIT_EXCEEDED ? LF_SYNC_WAIT_LIMIT_EXCEEDED : LF_SYNC_WAIT_INVALID, 0 );
        lf_sync_compare_exchange( &dispatcher->arena, dispatcher->status_word_base + slot,
                                  waiting, failed );
    }

    if ((lf_sync_load( &dispatcher->arena, dispatcher->status_word_base + slot ) & 0xff) != LF_SYNC_WAITING)
    {
        __atomic_add_fetch( &wait->park_seq, 1, __ATOMIC_RELEASE );
        if (dispatcher->wake) dispatcher->wake( &wait->park_seq );
    }
done:
    release_wait( wait );
}

int lf_sync_wait_begin_alert( const struct lf_sync_dispatcher *dispatcher, const uint32_t *objects,
                              uint32_t count, int wait_all, uint32_t owner, uint32_t alert_object,
                              struct lf_sync_wait_ticket *ticket )
{
    struct lf_sync_wait *wait;
    uint32_t generation, i, slot;

    if (!count || count > LF_SYNC_MAX_WAIT_OBJECTS || !owner ||
        dispatcher->status_word_base + dispatcher->wait_count > dispatcher->arena.word_count)
        return 0;
    for (i = 0; i < count; ++i) if (objects[i] >= dispatcher->object_count) return 0;
    if (alert_object != ~0u && alert_object >= dispatcher->object_count) return 0;
    if (!alloc_wait( dispatcher, &slot, &generation )) return 0;

    wait = &dispatcher->waits[slot];
    wait->count = count;
    wait->owner = owner;
    wait->wait_all = !!wait_all;
    wait->alert_object = alert_object;
    for (i = 0; i < count; ++i)
    {
        wait->objects[i] = objects[i];
        wait->object_generations[i] = load_u64( &dispatcher->objects[objects[i]].pulse ) >> 1;
    }
    ticket->slot = slot;
    ticket->generation = generation;
    ticket->waiting = lf_sync_wait_value( generation, LF_SYNC_WAITING, 0 );
    __atomic_store_n( &dispatcher->arena.words[dispatcher->status_word_base + slot].value,
                      ticket->waiting, __ATOMIC_RELEASE );
    __atomic_store_n( &wait->published, generation, __ATOMIC_RELEASE );
    for (i = 0; i < count; ++i) register_object_wait( &dispatcher->objects[objects[i]], slot );
    if (alert_object < dispatcher->object_count)
        register_object_wait( &dispatcher->objects[alert_object], slot );

    /* This retry pairs with signal-side scanning and closes the registration
     * window in which an object can become signaled before publication. */
    retry_wait( dispatcher, slot, generation );
    for (i = 0; i < count; ++i)
    {
        uint64_t pulse_generation = load_u64( &dispatcher->objects[objects[i]].pulse ) >> 1;
        if (pulse_generation != wait->object_generations[i])
            pulse_wait( dispatcher, slot, generation, objects[i], pulse_generation );
    }
    return 1;
}

int lf_sync_wait_begin( const struct lf_sync_dispatcher *dispatcher, const uint32_t *objects,
                        uint32_t count, int wait_all, uint32_t owner,
                        struct lf_sync_wait_ticket *ticket )
{
    return lf_sync_wait_begin_alert( dispatcher, objects, count, wait_all, owner, ~0u, ticket );
}

uint64_t lf_sync_wait_poll( const struct lf_sync_dispatcher *dispatcher,
                            const struct lf_sync_wait_ticket *ticket )
{
    if (ticket->slot >= dispatcher->wait_count) return 0;
    return lf_sync_load( &dispatcher->arena, dispatcher->status_word_base + ticket->slot );
}

int lf_sync_wait_park( const struct lf_sync_dispatcher *dispatcher,
                       const struct lf_sync_wait_ticket *ticket, const void *timeout )
{
    struct lf_sync_wait *wait;
    uint32_t sequence;

    if (ticket->slot >= dispatcher->wait_count || !dispatcher->park) return -1;
    wait = &dispatcher->waits[ticket->slot];
    sequence = __atomic_load_n( &wait->park_seq, __ATOMIC_ACQUIRE );
    if (lf_sync_wait_poll( dispatcher, ticket ) != ticket->waiting) return 0;
    return dispatcher->park( &wait->park_seq, sequence, timeout );
}

int lf_sync_wait_timeout( const struct lf_sync_dispatcher *dispatcher,
                          const struct lf_sync_wait_ticket *ticket )
{
    uint64_t timed_out;

    if (ticket->slot >= dispatcher->wait_count) return 0;
    timed_out = lf_sync_wait_value( ticket->generation, LF_SYNC_WAIT_TIMED_OUT, 0 );
    return lf_sync_compare_exchange( &dispatcher->arena, dispatcher->status_word_base + ticket->slot,
                                     ticket->waiting, timed_out );
}

void lf_sync_wait_end( const struct lf_sync_dispatcher *dispatcher,
                       const struct lf_sync_wait_ticket *ticket )
{
    struct lf_sync_wait *wait;
    uint64_t published;

    if (ticket->slot >= dispatcher->wait_count) return;
    wait = &dispatcher->waits[ticket->slot];
    if ((load_u64( &wait->lifetime ) >> LF_LIFETIME_GEN_SHIFT) != ticket->generation) return;
    published = ticket->generation;
    if (!cas_u64( &wait->published, &published, 0 )) return;
    /* Ensure a descriptor in the status word has been helped before allowing
     * this slot's generation to be reused. */
    lf_sync_wait_poll( dispatcher, ticket );
    unregister_wait( dispatcher, wait, ticket->slot );
    release_wait( wait );
}

void lf_sync_wake_object( const struct lf_sync_dispatcher *dispatcher, uint32_t object )
{
    uint64_t bits;
    uint32_t bit, i, slot;

    if (object >= dispatcher->object_count) return;
    for (i = 0; i < (dispatcher->wait_count + 63) / 64; ++i)
    {
        bits = load_u64( &dispatcher->objects[object].waiters[i] );
        while (bits)
        {
            bit = __builtin_ctzll( bits );
            slot = i * 64 + bit;
            if (slot < dispatcher->wait_count)
            {
                uint64_t generation = load_u64( &dispatcher->waits[slot].published );
                if (generation) retry_wait( dispatcher, slot, generation );
            }
            bits &= bits - 1;
        }
    }
}

/* Complete a waiter from a pulse without publishing a signaled event state.
 * The status word participates in the same MCAS as every other WaitAll
 * acquisition, so timeout/APC can still win at exactly one CAS. */
static int pulse_wait( const struct lf_sync_dispatcher *dispatcher, uint32_t slot,
                       uint32_t generation, uint32_t pulsed_object, uint64_t pulse_generation )
{
    struct lf_sync_object *event;
    const struct lf_sync_object *object;
    struct lf_sync_mcas_entry entries[LF_SYNC_MCAS_MAX_WORDS];
    struct lf_sync_wait *wait = &dispatcher->waits[slot];
    enum lf_sync_result result, final_result = LF_SYNC_SUCCESS;
    uint64_t pulse, unclaimed, claimed, waiting, value;
    uint32_t indices[LF_SYNC_MCAS_MAX_WORDS], count = 0, pulse_index = ~0u, i;
    int auto_claimed = 0, mcas;

    if (!acquire_wait( wait, generation )) return 0;
    if (load_u64( &wait->published ) != generation) goto failed;
    waiting = lf_sync_wait_value( generation, LF_SYNC_WAITING, 0 );

    /* Preserve WaitAny input priority for objects that are independently
     * signaled before using the transient pulse. */
    retry_wait( dispatcher, slot, generation );
    if (lf_sync_load( &dispatcher->arena, dispatcher->status_word_base + slot ) != waiting)
        goto failed;

    for (i = 0; i < wait->count; ++i)
        if (wait->objects[i] == pulsed_object)
        {
            pulse_index = i;
            break;
        }
    if (pulse_index == ~0u) goto failed;
    if (wait->object_generations[pulse_index] >= pulse_generation) goto failed;

    event = &dispatcher->objects[pulsed_object];
    if (!(event->flags & LF_SYNC_EVENT_MANUAL))
    {
        unclaimed = (uint64_t)pulse_generation << 1;
        claimed = unclaimed | 1;
        pulse = unclaimed;
        if (!cas_u64( &event->pulse, &pulse, claimed )) goto failed;
        auto_claimed = 1;
    }

    if (!wait->wait_all)
    {
        value = lf_sync_wait_value( generation, LF_SYNC_WAIT_COMPLETE, pulse_index );
        mcas = lf_sync_compare_exchange( &dispatcher->arena,
                                         dispatcher->status_word_base + slot, waiting, value );
    }
    else
    {
        for (i = 0; i < wait->count; ++i)
        {
            if (i == pulse_index) continue;
            object = &dispatcher->objects[wait->objects[i]];
            result = prepare_acquire( &dispatcher->arena, object, wait->owner, &entries[count] );
            if (result == LF_SYNC_ABANDONED) final_result = LF_SYNC_ABANDONED;
            else if (result != LF_SYNC_SUCCESS) goto failed;
            indices[count] = count;
            ++count;
        }
        entries[count].word = dispatcher->status_word_base + slot;
        entries[count].pad = 0;
        entries[count].expected = waiting;
        entries[count].desired = lf_sync_wait_value( generation,
            final_result == LF_SYNC_ABANDONED ? LF_SYNC_WAIT_ABANDONED : LF_SYNC_WAIT_COMPLETE, 0 );
        indices[count] = count;
        ++count;
        sort_entries( entries, indices, count );
        for (i = 1; i < count; ++i) if (entries[i - 1].word == entries[i].word) goto failed;
        do { mcas = lf_sync_mcas( &dispatcher->arena, entries, count ); }
        while (mcas < 0 && load_u64( &wait->published ) == generation);
    }

    if (mcas)
    {
        __atomic_add_fetch( &wait->park_seq, 1, __ATOMIC_RELEASE );
        if (dispatcher->wake) dispatcher->wake( &wait->park_seq );
        release_wait( wait );
        return 1;
    }

failed:
    if (auto_claimed)
    {
        pulse = claimed;
        cas_u64( &event->pulse, &pulse, unclaimed );
    }
    release_wait( wait );
    return 0;
}

enum lf_sync_result lf_sync_pulse_event( const struct lf_sync_dispatcher *dispatcher,
                                         uint32_t object, uint32_t *previous )
{
    struct lf_sync_object *event;
    uint64_t pulse, desired;
    uint64_t generation;
    uint32_t i;

    if (object >= dispatcher->object_count) return LF_SYNC_INVALID;
    event = &dispatcher->objects[object];
    if (event->type != LF_SYNC_EVENT) return LF_SYNC_INVALID;

    /* Clear persistent state before advancing the pulse generation. A waiter
     * records that generation during registration and either the pulser or
     * the registering thread helps complete every pre-generation waiter. */
    lf_sync_reset_event( &dispatcher->arena, event, previous );
    for (;;)
    {
        pulse = load_u64( &event->pulse );
        generation = (pulse >> 1) + 1;
        if (generation > (UINT64_MAX >> 1)) generation = 1;
        desired = (uint64_t)generation << 1;
        if (cas_u64( &event->pulse, &pulse, desired )) break;
    }

    for (i = 0; i < (dispatcher->wait_count + 63) / 64; ++i)
    {
        uint64_t bits = load_u64( &event->waiters[i] );

        while (bits)
        {
            uint32_t bit = __builtin_ctzll( bits ), slot = i * 64 + bit;
            uint32_t wait_generation = slot < dispatcher->wait_count ?
                                       load_u64( &dispatcher->waits[slot].published ) : 0;

            if (wait_generation && pulse_wait( dispatcher, slot, wait_generation, object, generation ) &&
                !(event->flags & LF_SYNC_EVENT_MANUAL))
                return LF_SYNC_SUCCESS;
            bits &= bits - 1;
        }
    }
    return LF_SYNC_SUCCESS;
}

void lf_sync_abandon_waits( const struct lf_sync_dispatcher *dispatcher, uint32_t owner )
{
    uint32_t i;

    if (!owner) return;
    for (i = 0; i < dispatcher->wait_count; ++i)
    {
        struct lf_sync_wait *wait = &dispatcher->waits[i];
        uint32_t generation = load_u64( &wait->published );
        uint64_t published, waiting, failed;

        if (!generation || !acquire_wait( wait, generation )) continue;
        if (wait->owner != owner) goto done;
        published = generation;
        if (!cas_u64( &wait->published, &published, 0 )) goto done;
        unregister_wait( dispatcher, wait, i );
        waiting = lf_sync_wait_value( generation, LF_SYNC_WAITING, 0 );
        failed = lf_sync_wait_value( generation, LF_SYNC_WAIT_INVALID, 0 );
        lf_sync_compare_exchange( &dispatcher->arena, dispatcher->status_word_base + i,
                                  waiting, failed );
        __atomic_add_fetch( &wait->park_seq, 1, __ATOMIC_RELEASE );
        if (dispatcher->wake) dispatcher->wake( &wait->park_seq );
        /* Drop the registration reference owned by the dead thread. */
        lf_sync_load( &dispatcher->arena, dispatcher->status_word_base + i );
        release_wait( wait );
done:
        release_wait( wait );
    }
}

void lf_sync_init_shared( struct lf_sync_shared *shared )
{
    memset( shared, 0, sizeof(*shared) );
    shared->version = LF_SYNC_SHARED_VERSION;
    __atomic_store_n( &shared->magic, LF_SYNC_SHARED_MAGIC, __ATOMIC_RELEASE );
}

int lf_sync_open_shared( struct lf_sync_dispatcher *dispatcher, struct lf_sync_shared *shared,
                         lf_sync_park_func park, lf_sync_wake_func wake )
{
    if (load_u64( &shared->magic ) != LF_SYNC_SHARED_MAGIC ||
        shared->version != LF_SYNC_SHARED_VERSION || !lf_sync_is_lock_free()) return 0;

    memset( dispatcher, 0, sizeof(*dispatcher) );
    dispatcher->shared = shared;
    dispatcher->arena.words = shared->words;
    dispatcher->arena.word_count = LF_SYNC_SHARED_WORDS;
    dispatcher->arena.descs = shared->descs;
    dispatcher->arena.desc_count = LF_SYNC_SHARED_DESCS;
    dispatcher->objects = shared->objects;
    dispatcher->object_count = LF_SYNC_SHARED_OBJECTS;
    dispatcher->waits = shared->waits;
    dispatcher->wait_count = LF_SYNC_SHARED_WAITS;
    dispatcher->status_word_base = LF_SYNC_SHARED_OBJECTS;
    dispatcher->park = park;
    dispatcher->wake = wake;
    return 1;
}

int lf_sync_alloc_object( struct lf_sync_dispatcher *dispatcher, enum lf_sync_object_type type,
                          uint32_t initial, uint32_t limit, uint32_t flags, uint32_t *index )
{
    uint32_t object;

    if (!dispatcher->shared) return 0;
    object = __atomic_fetch_add( &dispatcher->shared->next_object, 1, __ATOMIC_RELAXED );
    if (object >= LF_SYNC_SHARED_OBJECTS) return 0;

    switch (type)
    {
    case LF_SYNC_EVENT:
        lf_sync_init_event( &dispatcher->arena, &dispatcher->objects[object], object,
                            flags & LF_SYNC_EVENT_MANUAL, initial );
        break;
    case LF_SYNC_SEMAPHORE:
        if (!limit || initial > limit) return 0;
        lf_sync_init_semaphore( &dispatcher->arena, &dispatcher->objects[object], object, initial, limit );
        break;
    case LF_SYNC_MUTEX:
        lf_sync_init_mutex( &dispatcher->arena, &dispatcher->objects[object], object,
                            initial ? flags : 0, initial );
        break;
    default:
        return 0;
    }
    *index = object;
    return 1;
}
