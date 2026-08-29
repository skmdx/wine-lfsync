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

#define LF_CONTROL_GEN_MASK UINT64_C(0x0fffffff)

#define LF_LIFETIME_REF_MASK UINT64_C(0xffffffff)
#define LF_LIFETIME_GEN_SHIFT 32

#define LF_OWNER_DEAD UINT64_C(1)
#define LF_OWNER_GEN_SHIFT 1
#define LF_OWNER_GEN_MASK ((UINT64_C(1) << 62) - 1)

static uint64_t load_u64( const uint64_t *ptr )
{
    return __atomic_load_n( ptr, __ATOMIC_ACQUIRE );
}

static int cas_u64( uint64_t *ptr, uint64_t *expected, uint64_t desired )
{
    return __atomic_compare_exchange_n( ptr, expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE );
}

static int arena_owner_word( const struct lf_sync_arena *arena, uint32_t owner, uint32_t *word )
{
    uint32_t index;

    if (!owner || !arena->owner_count || (owner & 3)) return 0;
    index = owner >> 2;
    if (index >= arena->owner_count || arena->owner_word_base + index >= arena->word_count) return 0;
    *word = arena->owner_word_base + index;
    return 1;
}

static int arena_owner_dead( const struct lf_sync_arena *arena, uint32_t owner )
{
    uint32_t word;

    if (!arena_owner_word( arena, owner, &word )) return 0;
    return !!(lf_sync_load( arena, word ) & LF_OWNER_DEAD);
}

static uint64_t arena_owner_state( const struct lf_sync_arena *arena, uint32_t owner )
{
    uint32_t word;

    if (!arena_owner_word( arena, owner, &word )) return 0;
    return lf_sync_load( arena, word );
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

static uint64_t make_control( uint32_t owner, uint32_t generation,
                              enum lf_sync_mcas_status status, int owner_ref )
{
    return ((uint64_t)owner << LF_SYNC_MCAS_CONTROL_OWNER_SHIFT) |
           ((uint64_t)generation << LF_SYNC_MCAS_CONTROL_GEN_SHIFT) | status |
           (owner_ref ? LF_SYNC_MCAS_CONTROL_OWNER_REF : 0);
}

static enum lf_sync_mcas_status control_status( uint64_t control )
{
    return control & LF_SYNC_MCAS_CONTROL_STATUS_MASK;
}

static uint32_t control_generation( uint64_t control )
{
    return (control >> LF_SYNC_MCAS_CONTROL_GEN_SHIFT) & LF_CONTROL_GEN_MASK;
}

static uint32_t control_owner( uint64_t control )
{
    return LF_SYNC_MCAS_CONTROL_OWNER( control );
}

static void release_desc( const struct lf_sync_arena *arena, uint32_t index, uint32_t generation );

static int control_can_be_helped( uint64_t control, uint32_t generation )
{
    enum lf_sync_mcas_status status;

    if (control_generation( control ) != generation) return 0;
    status = control_status( control );
    if (status == LF_SYNC_MCAS_ACTIVE) return !!(control & LF_SYNC_MCAS_CONTROL_OWNER_REF);
    return status == LF_SYNC_MCAS_COMMITTED || status == LF_SYNC_MCAS_ABORTED;
}

static int acquire_desc( const struct lf_sync_arena *arena, uint32_t index, uint32_t generation )
{
    struct lf_sync_mcas *desc;
    uint64_t lifetime, desired;
    uint32_t refs;

    if (!generation || index >= arena->desc_count) return 0;
    desc = &arena->descs[index];
    for (;;)
    {
        uint64_t control = load_u64( &desc->control );

        if (!control_can_be_helped( control, generation )) return 0;
        lifetime = load_u64( &desc->lifetime );
        if ((lifetime >> LF_LIFETIME_GEN_SHIFT) != generation) return 0;
        refs = lifetime & LF_LIFETIME_REF_MASK;
        if (!refs || refs == UINT32_MAX) return 0;
        desired = lifetime + 1;
        if (cas_u64( &desc->lifetime, &lifetime, desired ))
        {
            control = load_u64( &desc->control );
            if (control_can_be_helped( control, generation )) return 1;
            release_desc( arena, index, generation );
            return 0;
        }
    }
}

static void cleanup_mcas( const struct lf_sync_arena *arena, uint32_t index, uint32_t generation )
{
    struct lf_sync_mcas *desc = &arena->descs[index];
    uint64_t control = load_u64( &desc->control ), tag = desc_tag( index, generation );
    uint32_t count, i;

    if (control_generation( control ) != generation) return;
    if (control_status( control ) != LF_SYNC_MCAS_COMMITTED &&
        control_status( control ) != LF_SYNC_MCAS_ABORTED) return;
    count = __atomic_load_n( &desc->count, __ATOMIC_ACQUIRE );
    if (count > LF_SYNC_MCAS_MAX_WORDS) count = LF_SYNC_MCAS_MAX_WORDS;
    for (i = 0; i < count; ++i)
    {
        struct lf_sync_mcas_entry entry = desc->entries[i];
        uint64_t expected = tag;
        uint64_t value = control_status( control ) == LF_SYNC_MCAS_COMMITTED ?
                         entry.desired : entry.expected;

        if (entry.word < arena->word_count)
            cas_u64( &arena->words[entry.word].value, &expected, value );
    }
}

static void release_desc( const struct lf_sync_arena *arena, uint32_t index, uint32_t generation )
{
    struct lf_sync_mcas *desc = &arena->descs[index];
    uint64_t control, lifetime, desired;
    uint32_t refs;

    for (;;)
    {
        lifetime = load_u64( &desc->lifetime );
        assert( (lifetime >> LF_LIFETIME_GEN_SHIFT) == generation );
        refs = lifetime & LF_LIFETIME_REF_MASK;
        assert( refs );
        control = load_u64( &desc->control );
        if (refs == 1 ||
            (refs == 2 && control_generation( control ) == generation &&
             control_status( control ) != LF_SYNC_MCAS_ACTIVE &&
             (control & LF_SYNC_MCAS_CONTROL_OWNER_REF)))
            cleanup_mcas( arena, index, generation );
        desired = lifetime - 1;
        if (cas_u64( &desc->lifetime, &lifetime, desired )) return;
    }
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
    active = make_control( control_owner( load_u64( &desc->control ) ), generation,
                           LF_SYNC_MCAS_ACTIVE, 1 );
    control = load_u64( &desc->control );
    if (control_generation( control ) != generation)
        return LF_SYNC_MCAS_ABORTED;
    if (control_status( control ) != LF_SYNC_MCAS_ACTIVE)
    {
        cleanup_mcas( arena, index, generation );
        return control_status( control );
    }
    active = control;

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
             make_control( control_owner( active ), generation,
                           i == count ? LF_SYNC_MCAS_COMMITTED : LF_SYNC_MCAS_ABORTED, 1 ) );
    control = load_u64( &desc->control );

    return control_status( control );
}

static enum lf_sync_mcas_status help_mcas( const struct lf_sync_arena *arena,
                                           uint32_t index, uint32_t generation )
{
    enum lf_sync_mcas_status status;

    if (!generation || index >= arena->desc_count) return LF_SYNC_MCAS_ABORTED;
    if (!acquire_desc( arena, index, generation )) return LF_SYNC_MCAS_ABORTED;
    status = help_mcas_acquired( arena, index, generation );
    release_desc( arena, index, generation );
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

static void release_desc_owner( const struct lf_sync_arena *arena, struct lf_sync_mcas *desc,
                                uint32_t index, uint32_t generation );

static int alloc_desc( const struct lf_sync_arena *arena, uint32_t owner,
                       uint32_t *index, uint32_t *generation )
{
    uint32_t count, i, n, start;

    if (arena_owner_dead( arena, owner )) return 0;
    count = arena->desc_count;
    if (count > LF_DESC_INDEX_MASK + 1) count = LF_DESC_INDEX_MASK + 1;
    if (!count) return 0;
    start = owner ? (owner >> 2) % count : 0;
    for (n = 0; n < count; ++n)
    {
        struct lf_sync_mcas *desc;
        uint64_t control, desired_control, lifetime, desired_lifetime;
        uint32_t next_generation;

        i = start + n;
        if (i >= count) i -= count;
        desc = &arena->descs[i];

        control = load_u64( &desc->control );
        if ((control & LF_SYNC_MCAS_CONTROL_OWNER_REF) &&
            (control_status( control ) == LF_SYNC_MCAS_ACTIVE ||
             control_status( control ) == LF_SYNC_MCAS_PREPARING) &&
            arena_owner_dead( arena, control_owner( control ) ))
        {
            lf_sync_abandon_descriptors( arena, control_owner( control ) );
            control = load_u64( &desc->control );
        }
        lifetime = load_u64( &desc->lifetime );
        if (lifetime & LF_LIFETIME_REF_MASK) continue;
        if (control & LF_SYNC_MCAS_CONTROL_OWNER_REF) continue;
        if (control_status( control ) == LF_SYNC_MCAS_ACTIVE ||
            control_status( control ) == LF_SYNC_MCAS_PREPARING) continue;
        next_generation = control_generation( control ) + 1;
        if (next_generation > LF_CONTROL_GEN_MASK) next_generation = 1;
        if (!next_generation) next_generation = 1;
        desired_control = make_control( owner, next_generation, LF_SYNC_MCAS_PREPARING, 1 );
        if (!cas_u64( &desc->control, &control, desired_control )) continue;
        if (arena_owner_dead( arena, owner ))
        {
            control = desired_control;
            cas_u64( &desc->control, &control,
                     make_control( owner, next_generation, LF_SYNC_MCAS_ABORTED, 0 ) );
            continue;
        }

        desired_lifetime = ((uint64_t)next_generation << LF_LIFETIME_GEN_SHIFT) | 1;
        if (!cas_u64( &desc->lifetime, &lifetime, desired_lifetime ))
        {
            control = desired_control;
            cas_u64( &desc->control, &control,
                     make_control( owner, next_generation, LF_SYNC_MCAS_ABORTED, 0 ) );
            continue;
        }
        control = load_u64( &desc->control );
        if (control != desired_control || arena_owner_dead( arena, owner ))
        {
            if (control == desired_control)
            {
                cas_u64( &desc->control, &control,
                         make_control( owner, next_generation, LF_SYNC_MCAS_ABORTED, 0 ) );
            }
            lifetime = desired_lifetime;
            cas_u64( &desc->lifetime, &lifetime, (uint64_t)next_generation << LF_LIFETIME_GEN_SHIFT );
            continue;
        }
        *index = i;
        *generation = next_generation;
        return 1;
    }
    return 0;
}

static void release_desc_owner( const struct lf_sync_arena *arena, struct lf_sync_mcas *desc,
                                uint32_t index, uint32_t generation )
{
    uint64_t control, desired;

    for (;;)
    {
        control = load_u64( &desc->control );
        if (control_generation( control ) != generation ||
            !(control & LF_SYNC_MCAS_CONTROL_OWNER_REF)) return;
        desired = control & ~LF_SYNC_MCAS_CONTROL_OWNER_REF;
        if (cas_u64( &desc->control, &control, desired ))
        {
            release_desc( arena, index, generation );
            return;
        }
    }
}

int lf_sync_mcas_owned( const struct lf_sync_arena *arena,
                        const struct lf_sync_mcas_entry *entries, uint32_t count, uint32_t owner )
{
    struct lf_sync_mcas_entry owned_entries[LF_SYNC_MCAS_MAX_WORDS];
    const struct lf_sync_mcas_entry *transaction_entries = entries;
    struct lf_sync_mcas *desc;
    enum lf_sync_mcas_status status;
    uint32_t generation, i, index, owner_word;
    uint64_t owner_state;

    if (!count || count > LF_SYNC_MCAS_MAX_WORDS)
        return 0;

    for (i = 0; i < count; ++i)
    {
        if (entries[i].word >= arena->word_count || !lf_sync_value_is_valid( entries[i].expected ) ||
            !lf_sync_value_is_valid( entries[i].desired ) || (i && entries[i - 1].word >= entries[i].word))
            return 0;
    }

    /* Order thread death with every owned transaction. If death publishes
     * DEAD first this read-only entry fails comparison; if the transaction
     * tags the owner word first, the death-side CAS helps it to a decision. */
    if (arena_owner_word( arena, owner, &owner_word ))
    {
        if (count == LF_SYNC_MCAS_MAX_WORDS) return 0;
        owner_state = lf_sync_load( arena, owner_word );
        if (owner_state & LF_OWNER_DEAD) return -1;
        memcpy( owned_entries, entries, count * sizeof(*entries) );
        i = count;
        while (i && owned_entries[i - 1].word > owner_word)
        {
            owned_entries[i] = owned_entries[i - 1];
            --i;
        }
        if ((i && owned_entries[i - 1].word == owner_word) ||
            (i < count && owned_entries[i].word == owner_word)) return 0;
        owned_entries[i] = (struct lf_sync_mcas_entry){owner_word, 0, owner_state, owner_state};
        transaction_entries = owned_entries;
        ++count;
    }

    if (!alloc_desc( arena, owner, &index, &generation )) return -1;
    desc = &arena->descs[index];

    memcpy( desc->entries, transaction_entries, count * sizeof(*entries) );
    __atomic_store_n( &desc->count, count, __ATOMIC_RELAXED );
    {
        uint64_t preparing = make_control( owner, generation, LF_SYNC_MCAS_PREPARING, 1 );
        uint64_t active = make_control( owner, generation, LF_SYNC_MCAS_ACTIVE, 1 );

        if (!cas_u64( &desc->control, &preparing, active ))
        {
            release_desc_owner( arena, desc, index, generation );
            return -1;
        }
    }

    status = help_mcas_acquired( arena, index, generation );
    release_desc_owner( arena, desc, index, generation );
    return status == LF_SYNC_MCAS_COMMITTED;
}

int lf_sync_mcas( const struct lf_sync_arena *arena,
                  const struct lf_sync_mcas_entry *entries, uint32_t count )
{
    return lf_sync_mcas_owned( arena, entries, count, 0 );
}

uint32_t lf_sync_abandon_descriptors( const struct lf_sync_arena *arena, uint32_t owner )
{
    uint32_t abandoned = 0, generation, i;

    if (!owner) return 0;
    for (i = 0; i < arena->desc_count; ++i)
    {
        struct lf_sync_mcas *desc = &arena->descs[i];
        uint64_t control = load_u64( &desc->control ), desired;

        if (control_owner( control ) != owner || !(control & LF_SYNC_MCAS_CONTROL_OWNER_REF)) continue;
        generation = control_generation( control );
        if (control_status( control ) == LF_SYNC_MCAS_PREPARING)
        {
            uint64_t lifetime = ((uint64_t)generation << LF_LIFETIME_GEN_SHIFT) | 1;

            desired = make_control( owner, generation, LF_SYNC_MCAS_ABORTED, 0 );
            if (cas_u64( &desc->control, &control, desired ))
            {
                cas_u64( &desc->lifetime, &lifetime,
                         (uint64_t)generation << LF_LIFETIME_GEN_SHIFT );
                ++abandoned;
            }
            continue;
        }
        else if (control_status( control ) == LF_SYNC_MCAS_ACTIVE)
            help_mcas( arena, i, generation );

        control = load_u64( &desc->control );
        if (control_owner( control ) != owner || control_generation( control ) != generation ||
            !(control & LF_SYNC_MCAS_CONTROL_OWNER_REF)) continue;
        desired = control & ~LF_SYNC_MCAS_CONTROL_OWNER_REF;
        if (cas_u64( &desc->control, &control, desired ))
        {
            uint64_t lifetime = load_u64( &desc->lifetime );
            if ((lifetime >> LF_LIFETIME_GEN_SHIFT) == generation &&
                (lifetime & LF_LIFETIME_REF_MASK)) release_desc( arena, i, generation );
            ++abandoned;
        }
    }
    return abandoned;
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

static enum lf_sync_result prepare_acquire_value( const struct lf_sync_object *object,
                                                  uint32_t owner, uint64_t value,
                                                  struct lf_sync_mcas_entry *entry )
{
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

static enum lf_sync_result prepare_acquire( const struct lf_sync_arena *arena,
                                            const struct lf_sync_object *object, uint32_t owner,
                                            struct lf_sync_mcas_entry *entry )
{
    return prepare_acquire_value( object, owner, lf_sync_load( arena, object->word ), entry );
}

static enum lf_sync_result prepare_signal_value( const struct lf_sync_object *object,
                                                 uint32_t owner, uint64_t value,
                                                 struct lf_sync_mcas_entry *entry )
{
    uint32_t count;

    entry->word = object->word;
    entry->pad = 0;
    entry->expected = value;

    switch (object->type)
    {
    case LF_SYNC_EVENT:
        entry->desired = 1;
        return LF_SYNC_SUCCESS;
    case LF_SYNC_SEMAPHORE:
        if (value >= object->limit) return LF_SYNC_LIMIT_EXCEEDED;
        entry->desired = value + 1;
        return LF_SYNC_SUCCESS;
    case LF_SYNC_MUTEX:
        count = mutex_count( value );
        if (!count || mutex_owner( value ) != owner) return LF_SYNC_NOT_OWNER;
        entry->desired = count == 1 ? 0 : mutex_value( owner, count - 1, 0 );
        return LF_SYNC_SUCCESS;
    default:
        return LF_SYNC_INVALID;
    }
}

static void swap_entries( struct lf_sync_mcas_entry *left, struct lf_sync_mcas_entry *right )
{
    struct lf_sync_mcas_entry entry = *left;

    *left = *right;
    *right = entry;
}

static void sift_entries( struct lf_sync_mcas_entry *entries, uint32_t root, uint32_t count )
{
    for (;;)
    {
        uint32_t child = root * 2 + 1;

        if (child >= count) return;
        if (child + 1 < count && entries[child].word < entries[child + 1].word) ++child;
        if (entries[root].word >= entries[child].word) return;
        swap_entries( &entries[root], &entries[child] );
        root = child;
    }
}

static void sort_entries( struct lf_sync_mcas_entry *entries, uint32_t count )
{
    uint32_t i;

    if (count <= 8)
    {
        for (i = 1; i < count; ++i)
        {
            struct lf_sync_mcas_entry entry = entries[i];
            uint32_t j = i;

            while (j && entries[j - 1].word > entry.word)
            {
                entries[j] = entries[j - 1];
                --j;
            }
            entries[j] = entry;
        }
        return;
    }

    /* Insertion sort minimizes work for small, usually ordered transactions.
     * WaitAll accepts up to 64 objects, so bound larger unordered waits to
     * O(n log n) with an in-place heap. */
    for (i = count / 2; i; --i) sift_entries( entries, i - 1, count );
    for (i = count; i > 1; --i)
    {
        swap_entries( &entries[0], &entries[i - 1] );
        sift_entries( entries, 0, i - 1 );
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
                sort_entries( entries, 2 );
            }
            mcas = lf_sync_mcas_owned( arena, entries, status_word < arena->word_count ? 2 : 1, owner );
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
        ++count;
    }
    sort_entries( entries, count );
    for (i = 1; i < count; ++i) if (entries[i - 1].word == entries[i].word) return LF_SYNC_INVALID;
    mcas = lf_sync_mcas_owned( arena, entries, count, owner );
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
    if (status_word >= arena->word_count ||
        ((waiting & 0xff) != LF_SYNC_WAITING && (waiting & 0xff) != LF_SYNC_WAIT_PREPARED))
        return LF_SYNC_INVALID;
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

static struct lf_sync_waiter_bucket *get_waiter_bucket( const struct lf_sync_dispatcher *dispatcher,
                                                        uint32_t object )
{
    uint32_t count = dispatcher->waiter_bucket_count;

    if (!count) return NULL;
    if (!(count & (count - 1))) object &= count - 1;
    else object %= count;
    return &dispatcher->waiter_buckets[object];
}

static void register_object_wait( const struct lf_sync_dispatcher *dispatcher,
                                  uint32_t object, uint32_t slot )
{
    struct lf_sync_waiter_bucket *bucket = get_waiter_bucket( dispatcher, object );
    uint32_t word = slot / 64;

    if (!bucket) return;
    __atomic_fetch_or( &bucket->waiters[word], UINT64_C(1) << (slot % 64), __ATOMIC_RELEASE );
    __atomic_fetch_or( &bucket->summary, 1u << word, __ATOMIC_RELEASE );
}

static void unregister_object_wait( const struct lf_sync_dispatcher *dispatcher,
                                    uint32_t object, uint32_t slot )
{
    struct lf_sync_waiter_bucket *bucket = get_waiter_bucket( dispatcher, object );
    uint32_t word = slot / 64, summary_bit = 1u << word;
    uint64_t bit = UINT64_C(1) << (slot % 64), previous;

    if (!bucket) return;
    previous = __atomic_fetch_and( &bucket->waiters[word], ~bit, __ATOMIC_RELEASE );
    if (!(previous & ~bit))
    {
        __atomic_fetch_and( &bucket->summary, ~summary_bit, __ATOMIC_ACQ_REL );
        /* Serialize the recheck with the registration RMW. A plain load could
         * legally observe an older value from a concurrent registration. */
        if (__atomic_fetch_or( &bucket->waiters[word], 0, __ATOMIC_ACQUIRE ))
            __atomic_fetch_or( &bucket->summary, summary_bit, __ATOMIC_RELEASE );
    }
}

static void unregister_wait( const struct lf_sync_dispatcher *dispatcher,
                             const struct lf_sync_wait *wait, uint32_t slot )
{
    uint32_t i;

    for (i = 0; i < wait->count; ++i)
        if (wait->objects[i] < dispatcher->object_count)
            unregister_object_wait( dispatcher, wait->objects[i], slot );
    if (wait->alert_object < dispatcher->object_count)
        unregister_object_wait( dispatcher, wait->alert_object, slot );
}

static int pulse_wait( const struct lf_sync_dispatcher *dispatcher, uint32_t slot,
                       uint32_t generation, uint32_t pulsed_object, uint64_t pulse_generation );

static void wake_wait( const struct lf_sync_dispatcher *dispatcher, struct lf_sync_wait *wait )
{
    if (!__atomic_exchange_n( &wait->parked, 0, __ATOMIC_ACQ_REL )) return;
    __atomic_add_fetch( &wait->park_seq, 1, __ATOMIC_RELEASE );
    if (dispatcher->wake) dispatcher->wake( &wait->park_seq );
}

static int alloc_wait( const struct lf_sync_dispatcher *dispatcher, uint32_t owner,
                       uint32_t *slot, uint32_t *generation )
{
    uint32_t count = dispatcher->wait_count, i, n, start;

    if (!count) return 0;
    start = owner ? (owner >> 2) % count : 0;
    for (n = 0; n < count; ++n)
    {
        struct lf_sync_wait *wait;
        uint64_t lifetime, desired;
        uint32_t next_generation;

        i = start + n;
        if (i >= count) i -= count;
        wait = &dispatcher->waits[i];
        lifetime = load_u64( &wait->lifetime );
        if (lifetime & LF_LIFETIME_REF_MASK)
        {
            uint32_t published = load_u64( &wait->published );

            if (published && wait->owner &&
                (wait->owner_state != arena_owner_state( &dispatcher->arena, wait->owner ) ||
                 (wait->owner_state & LF_OWNER_DEAD)))
            {
                lf_sync_abandon_waits( dispatcher, wait->owner );
                lifetime = load_u64( &wait->lifetime );
            }
            if (lifetime & LF_LIFETIME_REF_MASK) continue;
        }
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

static void retry_wait_status( const struct lf_sync_dispatcher *dispatcher, uint32_t slot,
                               uint32_t generation, enum lf_sync_wait_status expected_status,
                               uint32_t trigger_object )
{
    const struct lf_sync_object *objects[LF_SYNC_MAX_WAIT_OBJECTS];
    struct lf_sync_mcas_entry alert_entries[2];
    struct lf_sync_wait *wait;
    uint32_t i;
    enum lf_sync_result result;
    int mcas;

    if (slot >= dispatcher->wait_count) return;
    wait = &dispatcher->waits[slot];
    if (!acquire_wait( wait, generation )) return;
    if (load_u64( &wait->published ) != generation) goto done;
    if (!wait->count || wait->count > LF_SYNC_MAX_WAIT_OBJECTS) goto done;

    /* Waiter buckets are hashed. Reject a primary-hash collision before
     * loading object state or allocating an MCAS descriptor. */
    if (trigger_object != ~0u && wait->alert_object != trigger_object)
    {
        for (i = 0; i < wait->count; ++i)
            if (wait->objects[i] == trigger_object) break;
        if (i == wait->count) goto done;
    }

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
            alert_entries[1].expected = lf_sync_wait_value( generation, expected_status, 0 );
            alert_entries[1].desired = lf_sync_wait_value( generation, LF_SYNC_WAIT_ALERTED, 0 );
            sort_entries( alert_entries, 2 );
            do { mcas = lf_sync_mcas_owned( &dispatcher->arena, alert_entries, 2, wait->owner ); }
            while (mcas < 0 && load_u64( &wait->published ) == generation &&
                   wait->owner_state == arena_owner_state( &dispatcher->arena, wait->owner ) &&
                   !(wait->owner_state & LF_OWNER_DEAD));
        }
    }

    while ((result = lf_sync_try_wait_status( &dispatcher->arena, objects, wait->count, wait->wait_all,
                                              wait->owner, dispatcher->status_word_base + slot,
                                              lf_sync_wait_value( generation, expected_status, 0 ) )) == LF_SYNC_RETRY)
    {
        uint64_t owner_state = arena_owner_state( &dispatcher->arena, wait->owner );

        if (load_u64( &wait->published ) != generation) break;
        if ((owner_state & LF_OWNER_DEAD) || owner_state != wait->owner_state)
        {
            lf_sync_abandon_waits( dispatcher, wait->owner );
            break;
        }
    }

    if (result == LF_SYNC_LIMIT_EXCEEDED || result == LF_SYNC_INVALID)
    {
        uint64_t waiting = lf_sync_wait_value( generation, expected_status, 0 );
        uint64_t failed = lf_sync_wait_value( generation,
            result == LF_SYNC_LIMIT_EXCEEDED ? LF_SYNC_WAIT_LIMIT_EXCEEDED : LF_SYNC_WAIT_INVALID, 0 );
        lf_sync_compare_exchange( &dispatcher->arena, dispatcher->status_word_base + slot,
                                  waiting, failed );
    }

    if ((lf_sync_load( &dispatcher->arena, dispatcher->status_word_base + slot ) & 0xff) != expected_status)
        wake_wait( dispatcher, wait );
done:
    release_wait( wait );
}

static void retry_wait( const struct lf_sync_dispatcher *dispatcher, uint32_t slot, uint32_t generation )
{
    retry_wait_status( dispatcher, slot, generation, LF_SYNC_WAITING, ~0u );
}

static void retry_wait_for_object( const struct lf_sync_dispatcher *dispatcher, uint32_t slot,
                                   uint32_t generation, uint32_t object )
{
    retry_wait_status( dispatcher, slot, generation, LF_SYNC_WAITING, object );
}

static int register_wait( const struct lf_sync_dispatcher *dispatcher, const uint32_t *objects,
                          uint32_t count, int wait_all, uint32_t owner, uint32_t alert_object,
                          enum lf_sync_wait_status initial_status, int register_objects,
                          struct lf_sync_wait_ticket *ticket )
{
    struct lf_sync_wait *wait;
    uint64_t expected, invalid;
    uint32_t generation, i, slot;

    if (!count || count > LF_SYNC_MAX_WAIT_OBJECTS || !owner ||
        !lf_sync_owner_alive( dispatcher, owner ) ||
        dispatcher->status_word_base + dispatcher->wait_count > dispatcher->arena.word_count)
        return 0;
    for (i = 0; i < count; ++i) if (objects[i] >= dispatcher->object_count) return 0;
    if (alert_object != ~0u && alert_object >= dispatcher->object_count) return 0;
    if (!alloc_wait( dispatcher, owner, &slot, &generation )) return 0;

    wait = &dispatcher->waits[slot];
    if (!acquire_wait( wait, generation ))
    {
        release_wait( wait );
        return 0;
    }
    wait->count = count;
    wait->owner = owner;
    wait->owner_state = arena_owner_state( &dispatcher->arena, owner );
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
                      lf_sync_wait_value( generation, initial_status, 0 ), __ATOMIC_RELEASE );
    __atomic_store_n( &wait->published, generation, __ATOMIC_RELEASE );
    if (register_objects)
    {
        for (i = 0; i < count; ++i) register_object_wait( dispatcher, objects[i], slot );
        if (alert_object < dispatcher->object_count)
            register_object_wait( dispatcher, alert_object, slot );
    }

    if (wait->owner_state != arena_owner_state( &dispatcher->arena, owner ) ||
        (wait->owner_state & LF_OWNER_DEAD) || load_u64( &wait->published ) != generation)
    {
        expected = generation;
        if (cas_u64( &wait->published, &expected, 0 ))
        {
            invalid = lf_sync_wait_value( generation, LF_SYNC_WAIT_INVALID, 0 );
            lf_sync_compare_exchange( &dispatcher->arena, dispatcher->status_word_base + slot,
                                      lf_sync_wait_value( generation, initial_status, 0 ), invalid );
            if (register_objects) unregister_wait( dispatcher, wait, slot );
            release_wait( wait ); /* registration reference */
        }
        else if (register_objects) unregister_wait( dispatcher, wait, slot );
        release_wait( wait ); /* temporary publication reference */
        return 0;
    }

    release_wait( wait ); /* temporary publication reference */
    return 1;
}

int lf_sync_wait_begin_alert( const struct lf_sync_dispatcher *dispatcher, const uint32_t *objects,
                              uint32_t count, int wait_all, uint32_t owner, uint32_t alert_object,
                              struct lf_sync_wait_ticket *ticket )
{
    struct lf_sync_mcas_entry entry;
    struct lf_sync_wait *wait;
    uint64_t prepared;
    uint32_t i;
    int mcas;

    if (!register_wait( dispatcher, objects, count, wait_all, owner, alert_object,
                        LF_SYNC_WAIT_PREPARED, 0, ticket )) return 0;
    wait = &dispatcher->waits[ticket->slot];
    prepared = lf_sync_wait_value( ticket->generation, LF_SYNC_WAIT_PREPARED, 0 );

    /* Ready waits complete without ever publishing into a waiter bucket. */
    retry_wait_status( dispatcher, ticket->slot, ticket->generation, LF_SYNC_WAIT_PREPARED, ~0u );
    if (lf_sync_wait_poll( dispatcher, ticket ) != prepared) return 1;

    for (i = 0; i < count; ++i) register_object_wait( dispatcher, objects[i], ticket->slot );
    if (alert_object < dispatcher->object_count)
        register_object_wait( dispatcher, alert_object, ticket->slot );

    entry.word = dispatcher->status_word_base + ticket->slot;
    entry.pad = 0;
    entry.expected = prepared;
    entry.desired = ticket->waiting;
    do { mcas = lf_sync_mcas_owned( &dispatcher->arena, &entry, 1, owner ); }
    while (mcas < 0 && load_u64( &wait->published ) == ticket->generation &&
           wait->owner_state == arena_owner_state( &dispatcher->arena, owner ) &&
           !(wait->owner_state & LF_OWNER_DEAD));
    if (mcas <= 0)
    {
        lf_sync_wait_end( dispatcher, ticket );
        return 0;
    }

    /* This retry pairs with signal-side scanning and closes the registration
     * window in which an object can become signaled before publication. */
    retry_wait( dispatcher, ticket->slot, ticket->generation );
    for (i = 0; i < count; ++i)
    {
        uint64_t pulse_generation = load_u64( &dispatcher->objects[objects[i]].pulse ) >> 1;
        if (pulse_generation != wait->object_generations[i])
            pulse_wait( dispatcher, ticket->slot, ticket->generation, objects[i], pulse_generation );
    }
    return 1;
}

int lf_sync_wait_begin( const struct lf_sync_dispatcher *dispatcher, const uint32_t *objects,
                        uint32_t count, int wait_all, uint32_t owner,
                        struct lf_sync_wait_ticket *ticket )
{
    return lf_sync_wait_begin_alert( dispatcher, objects, count, wait_all, owner, ~0u, ticket );
}

enum lf_sync_result lf_sync_signal_and_wait_begin( const struct lf_sync_dispatcher *dispatcher,
                                                   uint32_t signal_object, uint32_t wait_object,
                                                   uint32_t owner, uint32_t alert_object,
                                                   struct lf_sync_wait_ticket *ticket )
{
    const struct lf_sync_object *signal, *wait;
    struct lf_sync_mcas_entry entries[3], wait_entry;
    struct lf_sync_wait *registered;
    enum lf_sync_result result, wait_result;
    uint32_t count, object = wait_object;
    uint64_t signal_value, prepared, pulse_generation;
    enum lf_sync_wait_status status;
    int mcas;

    if (signal_object >= dispatcher->object_count || wait_object >= dispatcher->object_count ||
        !owner || owner == LF_MUTEX_ABANDONED_OWNER) return LF_SYNC_INVALID;
    if (!register_wait( dispatcher, &object, 1, 0, owner, alert_object,
                        LF_SYNC_WAIT_PREPARED, 1, ticket )) return LF_SYNC_RETRY;

    signal = &dispatcher->objects[signal_object];
    wait = &dispatcher->objects[wait_object];
    registered = &dispatcher->waits[ticket->slot];
    prepared = lf_sync_wait_value( ticket->generation, LF_SYNC_WAIT_PREPARED, 0 );

    for (;;)
    {
        signal_value = lf_sync_load( &dispatcher->arena, signal->word );
        result = prepare_signal_value( signal, owner, signal_value, &entries[0] );
        if (result != LF_SYNC_SUCCESS)
        {
            lf_sync_wait_end( dispatcher, ticket );
            return result;
        }

        count = 1;
        if (signal_object == wait_object)
        {
            wait_result = prepare_acquire_value( wait, owner, entries[0].desired, &wait_entry );
            if (wait_result != LF_SYNC_SUCCESS && wait_result != LF_SYNC_ABANDONED)
            {
                lf_sync_wait_end( dispatcher, ticket );
                return wait_result;
            }
            entries[0].desired = wait_entry.desired;
            status = wait_result == LF_SYNC_ABANDONED ? LF_SYNC_WAIT_ABANDONED : LF_SYNC_WAIT_COMPLETE;
        }
        else
        {
            wait_result = prepare_acquire( &dispatcher->arena, wait, owner, &wait_entry );
            if (wait_result == LF_SYNC_SUCCESS || wait_result == LF_SYNC_ABANDONED)
            {
                entries[count] = wait_entry;
                ++count;
                status = wait_result == LF_SYNC_ABANDONED ? LF_SYNC_WAIT_ABANDONED : LF_SYNC_WAIT_COMPLETE;
            }
            else if (wait_result == LF_SYNC_UNSATISFIED) status = LF_SYNC_WAITING;
            else if (wait_result == LF_SYNC_LIMIT_EXCEEDED) status = LF_SYNC_WAIT_LIMIT_EXCEEDED;
            else status = LF_SYNC_WAIT_INVALID;
        }

        entries[count].word = dispatcher->status_word_base + ticket->slot;
        entries[count].pad = 0;
        entries[count].expected = prepared;
        entries[count].desired = lf_sync_wait_value( ticket->generation, status, 0 );
        ++count;
        sort_entries( entries, count );
        mcas = lf_sync_mcas_owned( &dispatcher->arena, entries, count, owner );
        if (mcas > 0) break;
        if (mcas < 0 && (registered->owner_state & LF_OWNER_DEAD ||
            registered->owner_state != arena_owner_state( &dispatcher->arena, owner )))
        {
            lf_sync_wait_end( dispatcher, ticket );
            return LF_SYNC_RETRY;
        }
        if (lf_sync_load( &dispatcher->arena, dispatcher->status_word_base + ticket->slot ) != prepared)
        {
            lf_sync_wait_end( dispatcher, ticket );
            return LF_SYNC_INVALID;
        }
    }

    lf_sync_wake_object( dispatcher, signal_object );
    if (status == LF_SYNC_WAITING)
    {
        retry_wait( dispatcher, ticket->slot, ticket->generation );
        pulse_generation = load_u64( &wait->pulse ) >> 1;
        if (pulse_generation != registered->object_generations[0])
            pulse_wait( dispatcher, ticket->slot, ticket->generation, wait_object, pulse_generation );
    }
    return LF_SYNC_SUCCESS;
}

uint64_t lf_sync_wait_poll( const struct lf_sync_dispatcher *dispatcher,
                            const struct lf_sync_wait_ticket *ticket )
{
    uint64_t value;
    uint32_t word;

    if (ticket->slot >= dispatcher->wait_count) return 0;
    word = dispatcher->status_word_base + ticket->slot;
    value = load_u64( &dispatcher->arena.words[word].value );
    if (!(value & LF_DESC_BIT)) return value;
    return lf_sync_load( &dispatcher->arena, word );
}

int lf_sync_wait_park( const struct lf_sync_dispatcher *dispatcher,
                       const struct lf_sync_wait_ticket *ticket, const void *timeout )
{
    struct lf_sync_wait *wait;
    uint32_t sequence;
    int ret;

    if (ticket->slot >= dispatcher->wait_count || !dispatcher->park) return -1;
    wait = &dispatcher->waits[ticket->slot];
    if (lf_sync_wait_poll( dispatcher, ticket ) != ticket->waiting) return 0;
    __atomic_store_n( &wait->parked, 1, __ATOMIC_RELEASE );
    sequence = __atomic_load_n( &wait->park_seq, __ATOMIC_ACQUIRE );
    if (lf_sync_wait_poll( dispatcher, ticket ) != ticket->waiting)
    {
        __atomic_store_n( &wait->parked, 0, __ATOMIC_RELEASE );
        return 0;
    }
    ret = dispatcher->park( &wait->park_seq, sequence, timeout );
    __atomic_store_n( &wait->parked, 0, __ATOMIC_RELEASE );
    return ret;
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
    struct lf_sync_waiter_bucket *bucket;
    uint64_t bits;
    uint32_t bit, i, slot, summary;

    if (object >= dispatcher->object_count) return;
    if (!(bucket = get_waiter_bucket( dispatcher, object ))) return;
    summary = __atomic_load_n( &bucket->summary, __ATOMIC_ACQUIRE );
    while (summary)
    {
        i = __builtin_ctz( summary );
        bits = load_u64( &bucket->waiters[i] );
        while (bits)
        {
            bit = __builtin_ctzll( bits );
            slot = i * 64 + bit;
            if (slot < dispatcher->wait_count)
            {
                uint64_t generation = load_u64( &dispatcher->waits[slot].published );
                if (generation) retry_wait_for_object( dispatcher, slot, generation, object );
            }
            bits &= bits - 1;
        }
        summary &= summary - 1;
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
    uint32_t count = 0, pulse_index = ~0u, i;
    int auto_claimed = 0, mcas;

    if (!acquire_wait( wait, generation )) return 0;
    if (load_u64( &wait->published ) != generation) goto failed;
    waiting = lf_sync_wait_value( generation, LF_SYNC_WAITING, 0 );

    for (i = 0; i < wait->count; ++i)
        if (wait->objects[i] == pulsed_object)
        {
            pulse_index = i;
            break;
        }
    if (pulse_index == ~0u) goto failed;

    /* Preserve WaitAny input priority for objects that are independently
     * signaled before using the transient pulse. */
    retry_wait( dispatcher, slot, generation );
    if (lf_sync_load( &dispatcher->arena, dispatcher->status_word_base + slot ) != waiting)
        goto failed;

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
            ++count;
        }
        entries[count].word = dispatcher->status_word_base + slot;
        entries[count].pad = 0;
        entries[count].expected = waiting;
        entries[count].desired = lf_sync_wait_value( generation,
            final_result == LF_SYNC_ABANDONED ? LF_SYNC_WAIT_ABANDONED : LF_SYNC_WAIT_COMPLETE, 0 );
        ++count;
        sort_entries( entries, count );
        for (i = 1; i < count; ++i) if (entries[i - 1].word == entries[i].word) goto failed;
        do { mcas = lf_sync_mcas_owned( &dispatcher->arena, entries, count, wait->owner ); }
        while (mcas < 0 && load_u64( &wait->published ) == generation &&
               wait->owner_state == arena_owner_state( &dispatcher->arena, wait->owner ) &&
               !(wait->owner_state & LF_OWNER_DEAD));
    }

    if (mcas)
    {
        wake_wait( dispatcher, wait );
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
    struct lf_sync_waiter_bucket *bucket;
    struct lf_sync_object *event;
    uint64_t pulse, desired;
    uint64_t generation;
    uint32_t i, summary;

    if (object >= dispatcher->object_count) return LF_SYNC_INVALID;
    event = &dispatcher->objects[object];
    if (event->type != LF_SYNC_EVENT) return LF_SYNC_INVALID;
    if (!(bucket = get_waiter_bucket( dispatcher, object ))) return LF_SYNC_INVALID;

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

    summary = __atomic_load_n( &bucket->summary, __ATOMIC_ACQUIRE );
    while (summary)
    {
        uint64_t bits;

        i = __builtin_ctz( summary );
        bits = load_u64( &bucket->waiters[i] );

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
        summary &= summary - 1;
    }
    return LF_SYNC_SUCCESS;
}

void lf_sync_abandon_waits( const struct lf_sync_dispatcher *dispatcher, uint32_t owner )
{
    uint64_t owner_state;
    uint32_t i;

    if (!owner) return;
    owner_state = arena_owner_state( &dispatcher->arena, owner );
    for (i = 0; i < dispatcher->wait_count; ++i)
    {
        struct lf_sync_wait *wait = &dispatcher->waits[i];
        uint32_t generation = load_u64( &wait->published );
        uint64_t prepared, published, waiting, failed;

        if (!generation || !acquire_wait( wait, generation )) continue;
        if (wait->owner != owner ||
            (dispatcher->arena.owner_count && !(owner_state & LF_OWNER_DEAD) &&
             wait->owner_state == owner_state)) goto done;
        published = generation;
        if (!cas_u64( &wait->published, &published, 0 )) goto done;
        unregister_wait( dispatcher, wait, i );
        waiting = lf_sync_wait_value( generation, LF_SYNC_WAITING, 0 );
        failed = lf_sync_wait_value( generation, LF_SYNC_WAIT_INVALID, 0 );
        lf_sync_compare_exchange( &dispatcher->arena, dispatcher->status_word_base + i,
                                  waiting, failed );
        prepared = lf_sync_wait_value( generation, LF_SYNC_WAIT_PREPARED, 0 );
        lf_sync_compare_exchange( &dispatcher->arena, dispatcher->status_word_base + i,
                                  prepared, failed );
        wake_wait( dispatcher, wait );
        /* Drop the registration reference owned by the dead thread. */
        lf_sync_load( &dispatcher->arena, dispatcher->status_word_base + i );
        release_wait( wait );
done:
        release_wait( wait );
    }
}

void lf_sync_init_shared( struct lf_sync_shared *shared )
{
    /* The backing memfd has just been extended and is already zero-filled.
     * Touch only the header so initialization does not fault every arena page
     * into the wineserver's RSS. */
    __atomic_store_n( &shared->magic, 0, __ATOMIC_RELAXED );
    shared->version = LF_SYNC_SHARED_VERSION;
    shared->next_object = 0;
    shared->free_object = UINT32_MAX;
    memset( shared->header_pad, 0, sizeof(shared->header_pad) );
    __atomic_store_n( &shared->magic, LF_SYNC_SHARED_MAGIC, __ATOMIC_RELEASE );
}

static uint64_t lease_control( uint64_t generation, enum lf_sync_lease_state state )
{
    return (generation << LF_SYNC_LEASE_GENERATION_SHIFT) | state;
}

static uint64_t lease_generation( uint64_t token )
{
    return token >> LF_SYNC_LEASE_SLOT_BITS;
}

int lf_sync_activate_lease( struct lf_sync_shared *shared, uint32_t slot, uint64_t *token )
{
    uint64_t control, generation, desired;

    if (slot >= LF_SYNC_SHARED_LEASES || !token) return 0;
    control = __atomic_load_n( &shared->leases[slot].control, __ATOMIC_ACQUIRE );
    if ((control & LF_SYNC_LEASE_STATE_MASK) != LF_SYNC_LEASE_FREE) return 0;
    generation = ((control >> LF_SYNC_LEASE_GENERATION_SHIFT) + 1) & LF_SYNC_LEASE_GENERATION_MASK;
    if (!generation) generation = 1;
    desired = lease_control( generation, LF_SYNC_LEASE_ACTIVE );
    if (!__atomic_compare_exchange_n( &shared->leases[slot].control, &control, desired, 0,
                                      __ATOMIC_RELEASE, __ATOMIC_ACQUIRE )) return 0;
    *token = (generation << LF_SYNC_LEASE_SLOT_BITS) | slot;
    return 1;
}

int lf_sync_mark_lease_released( struct lf_sync_shared *shared, uint64_t token )
{
    uint32_t slot = token & LF_SYNC_LEASE_SLOT_MASK;
    uint64_t generation = lease_generation( token );
    uint64_t expected, desired;

    if (!generation) return 0;
    expected = lease_control( generation, LF_SYNC_LEASE_ACTIVE );
    desired = lease_control( generation, LF_SYNC_LEASE_RELEASED );
    return __atomic_compare_exchange_n( &shared->leases[slot].control, &expected, desired, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE );
}

void lf_sync_notify_lease_release( struct lf_sync_shared *shared, uint64_t token )
{
    uint32_t slot = token & LF_SYNC_LEASE_SLOT_MASK;
    __atomic_fetch_or( &shared->released_leases[slot / 64], UINT64_C(1) << (slot % 64), __ATOMIC_RELEASE );
}

int lf_sync_release_lease( struct lf_sync_shared *shared, uint64_t token )
{
    if (!lf_sync_mark_lease_released( shared, token )) return 0;
    lf_sync_notify_lease_release( shared, token );
    return 1;
}

int lf_sync_lease_is_released( const struct lf_sync_shared *shared, uint64_t token )
{
    uint32_t slot = token & LF_SYNC_LEASE_SLOT_MASK;
    uint64_t generation = lease_generation( token );

    return generation && __atomic_load_n( &shared->leases[slot].control, __ATOMIC_ACQUIRE ) ==
                         lease_control( generation, LF_SYNC_LEASE_RELEASED );
}

int lf_sync_free_lease( struct lf_sync_shared *shared, uint64_t token )
{
    uint32_t slot = token & LF_SYNC_LEASE_SLOT_MASK;
    uint64_t generation = lease_generation( token );
    uint64_t expected, desired;

    if (!generation) return 0;
    expected = lease_control( generation, LF_SYNC_LEASE_RELEASED );
    desired = lease_control( generation, LF_SYNC_LEASE_FREE );
    return __atomic_compare_exchange_n( &shared->leases[slot].control, &expected, desired, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE );
}

uint64_t lf_sync_take_released_leases( struct lf_sync_shared *shared, uint32_t word )
{
    if (word >= LF_SYNC_SHARED_LEASE_WORDS) return 0;
    return __atomic_exchange_n( &shared->released_leases[word], 0, __ATOMIC_ACQ_REL );
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
    dispatcher->arena.owner_word_base = LF_SYNC_SHARED_OBJECTS + LF_SYNC_SHARED_WAITS;
    dispatcher->arena.owner_count = LF_SYNC_SHARED_OWNERS;
    dispatcher->objects = shared->objects;
    dispatcher->object_count = LF_SYNC_SHARED_OBJECTS;
    dispatcher->waiter_buckets = shared->waiter_buckets;
    dispatcher->waiter_bucket_count = LF_SYNC_SHARED_WAITER_BUCKETS;
    dispatcher->waits = shared->waits;
    dispatcher->wait_count = LF_SYNC_SHARED_WAITS;
    dispatcher->status_word_base = LF_SYNC_SHARED_OBJECTS;
    dispatcher->park = park;
    dispatcher->wake = wake;
    return 1;
}

void lf_sync_set_owner_alive( const struct lf_sync_dispatcher *dispatcher, uint32_t owner, int alive )
{
    uint64_t state, desired, generation;
    uint32_t word;

    if (!arena_owner_word( &dispatcher->arena, owner, &word )) return;
    for (;;)
    {
        state = lf_sync_load( &dispatcher->arena, word );
        if (alive)
        {
            if (!(state & LF_OWNER_DEAD)) return;
            generation = ((state >> LF_OWNER_GEN_SHIFT) + 1) & LF_OWNER_GEN_MASK;
            desired = generation << LF_OWNER_GEN_SHIFT;
        }
        else
        {
            if (state & LF_OWNER_DEAD) return;
            desired = state | LF_OWNER_DEAD;
        }
        if (lf_sync_compare_exchange( &dispatcher->arena, word, state, desired )) return;
    }
}

int lf_sync_owner_alive( const struct lf_sync_dispatcher *dispatcher, uint32_t owner )
{
    return !arena_owner_dead( &dispatcher->arena, owner );
}

int lf_sync_alloc_object( struct lf_sync_dispatcher *dispatcher, enum lf_sync_object_type type,
                          uint32_t initial, uint32_t limit, uint32_t flags, uint32_t *index )
{
    uint32_t object;

    if (!dispatcher->shared) return 0;
    if (type > LF_SYNC_MUTEX || (type == LF_SYNC_SEMAPHORE && (!limit || initial > limit))) return 0;
    object = dispatcher->shared->free_object;
    if (object != UINT32_MAX)
        dispatcher->shared->free_object = dispatcher->objects[object].next_free;
    else
    {
        object = dispatcher->shared->next_object;
        if (object >= LF_SYNC_SHARED_OBJECTS) return 0;
        dispatcher->shared->next_object = object + 1;
    }

    switch (type)
    {
    case LF_SYNC_EVENT:
        lf_sync_init_event( &dispatcher->arena, &dispatcher->objects[object], object,
                            flags & LF_SYNC_EVENT_MANUAL, initial );
        break;
    case LF_SYNC_SEMAPHORE:
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

int lf_sync_free_object( struct lf_sync_dispatcher *dispatcher, uint32_t index )
{
    struct lf_sync_waiter_bucket *bucket;
    struct lf_sync_object *object;
    uint32_t i;

    if (!dispatcher->shared || index >= dispatcher->object_count) return 0;
    object = &dispatcher->objects[index];
    if (!(bucket = get_waiter_bucket( dispatcher, index ))) return 0;
    /* Do not use the summary here. unregister_object_wait() may temporarily
     * clear a summary bit while it rechecks a concurrent registration. The
     * underlying bitmap has no such false-empty window for a live slot. */
    for (i = 0; i < LF_SYNC_SHARED_WAITER_WORDS; ++i)
    {
        uint64_t bits;

        bits = load_u64( &bucket->waiters[i] );
        while (bits)
        {
            struct lf_sync_wait *wait;
            uint32_t count, generation, j, slot = i * 64 + __builtin_ctzll( bits );

            if (slot < dispatcher->wait_count)
            {
                wait = &dispatcher->waits[slot];
                generation = load_u64( &wait->published );
                if (generation && acquire_wait( wait, generation ))
                {
                    if (load_u64( &wait->published ) == generation)
                    {
                        count = wait->count;
                        if (count > LF_SYNC_MAX_WAIT_OBJECTS) count = LF_SYNC_MAX_WAIT_OBJECTS;
                        for (j = 0; j < count; ++j)
                            if (wait->objects[j] == index) break;
                        if (j != count || wait->alert_object == index)
                        {
                            release_wait( wait );
                            return 0;
                        }
                    }
                    release_wait( wait );
                }
            }
            bits &= bits - 1;
        }
    }
    if (object->type == LF_SYNC_MUTEX && mutex_count( lf_sync_load( &dispatcher->arena, object->word ) ))
        return 0;

    object->next_free = dispatcher->shared->free_object;
    dispatcher->shared->free_object = index;
    return 1;
}
