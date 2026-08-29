/*
 * Lock-free NT synchronization support.
 *
 * Copyright 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_LOCKFREE_SYNC_H
#define __WINE_LOCKFREE_SYNC_H

#include <stdint.h>

#define LF_SYNC_MCAS_MAX_WORDS 66
#define LF_SYNC_SHARED_MAGIC UINT64_C(0x57494e454c465359) /* WINELFSY */
#define LF_SYNC_SHARED_VERSION 9
#define LF_SYNC_SHARED_OBJECTS 262144
#define LF_SYNC_SHARED_WAITS 2048
#define LF_SYNC_SHARED_DESCS 512
#define LF_SYNC_SHARED_WAITER_BUCKETS 4096
#define LF_SYNC_SHARED_LEASES (1u << 20)
#define LF_SYNC_SHARED_LEASE_WORDS (LF_SYNC_SHARED_LEASES / 64)
#define LF_SYNC_LEASE_SLOT_BITS 20
#define LF_SYNC_LEASE_SLOT_MASK (LF_SYNC_SHARED_LEASES - 1)
#define LF_SYNC_LEASE_STATE_MASK UINT64_C(0x3)
#define LF_SYNC_LEASE_GENERATION_SHIFT 2
#define LF_SYNC_LEASE_GENERATION_MASK ((UINT64_C(1) << (64 - LF_SYNC_LEASE_SLOT_BITS)) - 1)
#define LF_SYNC_SHARED_OWNERS LF_SYNC_SHARED_OBJECTS
#define LF_SYNC_SHARED_WORDS (LF_SYNC_SHARED_OBJECTS + LF_SYNC_SHARED_WAITS + LF_SYNC_SHARED_OWNERS)
#define LF_SYNC_SHARED_WAITER_WORDS (LF_SYNC_SHARED_WAITS / 64)

enum lf_sync_mcas_status
{
    LF_SYNC_MCAS_FREE,
    LF_SYNC_MCAS_ACTIVE,
    LF_SYNC_MCAS_COMMITTED,
    LF_SYNC_MCAS_ABORTED,
    LF_SYNC_MCAS_PREPARING,
};

#define LF_SYNC_MCAS_CONTROL_STATUS_MASK UINT64_C(0x7)
#define LF_SYNC_MCAS_CONTROL_OWNER_REF   UINT64_C(0x8)
#define LF_SYNC_MCAS_CONTROL_GEN_SHIFT   4
#define LF_SYNC_MCAS_CONTROL_OWNER_SHIFT 32
#define LF_SYNC_MCAS_CONTROL_OWNER(control) ((uint32_t)((control) >> LF_SYNC_MCAS_CONTROL_OWNER_SHIFT))

struct lf_sync_word
{
    uint64_t value;
};

struct lf_sync_mcas_entry
{
    uint32_t word;
    uint32_t pad;
    uint64_t expected;
    uint64_t desired;
};

struct lf_sync_mcas
{
    uint64_t lifetime;
    uint64_t control;
    uint32_t count;
    uint32_t pad;
    struct lf_sync_mcas_entry entries[LF_SYNC_MCAS_MAX_WORDS];
};

struct lf_sync_arena
{
    struct lf_sync_word *words;
    uint32_t word_count;
    struct lf_sync_mcas *descs;
    uint32_t desc_count;
    uint32_t owner_word_base;
    uint32_t owner_count;
};

enum lf_sync_object_type
{
    LF_SYNC_EVENT,
    LF_SYNC_SEMAPHORE,
    LF_SYNC_MUTEX,
};

struct lf_sync_object
{
    uint32_t word;
    uint32_t type;
    uint32_t limit;
    uint32_t flags;
    uint32_t next_free;
    uint32_t pad;
    uint64_t pulse;
};

struct lf_sync_waiter_bucket
{
    uint32_t summary;
    uint32_t pad;
    uint64_t waiters[LF_SYNC_SHARED_WAITER_WORDS];
};

#define LF_SYNC_MAX_WAIT_OBJECTS 64

struct lf_sync_wait
{
    uint64_t lifetime;
    uint64_t published;
    uint64_t owner_state;
    uint32_t park_seq;
    uint32_t parked;
    uint32_t count;
    uint32_t owner;
    uint32_t wait_all;
    uint32_t alert_object;
    uint32_t objects[LF_SYNC_MAX_WAIT_OBJECTS];
    uint64_t object_generations[LF_SYNC_MAX_WAIT_OBJECTS];
};

typedef int (*lf_sync_park_func)( uint32_t *address, uint32_t expected, const void *timeout );
typedef void (*lf_sync_wake_func)( uint32_t *address );

struct lf_sync_shared;

struct lf_sync_dispatcher
{
    struct lf_sync_shared *shared;
    struct lf_sync_arena arena;
    struct lf_sync_object *objects;
    uint32_t object_count;
    struct lf_sync_waiter_bucket *waiter_buckets;
    uint32_t waiter_bucket_count;
    struct lf_sync_wait *waits;
    uint32_t wait_count;
    uint32_t status_word_base;
    lf_sync_park_func park;
    lf_sync_wake_func wake;
};

struct lf_sync_wait_ticket
{
    uint32_t slot;
    uint32_t generation;
    uint64_t waiting;
};

enum lf_sync_lease_state
{
    LF_SYNC_LEASE_FREE,
    LF_SYNC_LEASE_ACTIVE,
    LF_SYNC_LEASE_RELEASED,
};

struct lf_sync_lease_slot
{
    uint64_t control;
};

struct lf_sync_shared
{
    uint64_t magic;
    uint32_t version;
    uint32_t next_object;
    uint32_t free_object;
    uint32_t pad;
    struct lf_sync_word words[LF_SYNC_SHARED_WORDS];
    struct lf_sync_mcas descs[LF_SYNC_SHARED_DESCS];
    struct lf_sync_object objects[LF_SYNC_SHARED_OBJECTS];
    struct lf_sync_waiter_bucket waiter_buckets[LF_SYNC_SHARED_WAITER_BUCKETS];
    struct lf_sync_wait waits[LF_SYNC_SHARED_WAITS];
    struct lf_sync_lease_slot leases[LF_SYNC_SHARED_LEASES];
    uint64_t released_leases[LF_SYNC_SHARED_LEASE_WORDS];
};

#define LF_SYNC_EVENT_MANUAL 0x1

enum lf_sync_result
{
    LF_SYNC_SUCCESS,
    LF_SYNC_UNSATISFIED,
    LF_SYNC_ABANDONED,
    LF_SYNC_INVALID,
    LF_SYNC_LIMIT_EXCEEDED,
    LF_SYNC_NOT_OWNER,
    LF_SYNC_RETRY,
};

#define LF_SYNC_WAIT_STATUS_BITS 16

enum lf_sync_wait_status
{
    LF_SYNC_WAITING,
    LF_SYNC_WAIT_COMPLETE,
    LF_SYNC_WAIT_ABANDONED,
    LF_SYNC_WAIT_TIMED_OUT,
    LF_SYNC_WAIT_ALERTED,
    LF_SYNC_WAIT_LIMIT_EXCEEDED,
    LF_SYNC_WAIT_INVALID,
    LF_SYNC_WAIT_PREPARED,
};

/* Normal values must keep bit 63 clear. The implementation reserves it for
 * descriptor references. */
int lf_sync_value_is_valid( uint64_t value );
int lf_sync_is_lock_free(void);

uint64_t lf_sync_load( const struct lf_sync_arena *arena, uint32_t word );
int lf_sync_compare_exchange( const struct lf_sync_arena *arena, uint32_t word,
                              uint64_t expected, uint64_t desired );

/* Returns 1 on commit, 0 on comparison/validation failure, and -1 if all
 * descriptors are temporarily referenced. Entries must be ordered by
 * increasing word index and may not contain the same word twice. Descriptor
 * allocation, helping references, and generation changes are managed
 * internally. */
int lf_sync_mcas( const struct lf_sync_arena *arena,
                  const struct lf_sync_mcas_entry *entries, uint32_t count );
int lf_sync_mcas_owned( const struct lf_sync_arena *arena,
                        const struct lf_sync_mcas_entry *entries, uint32_t count, uint32_t owner );
uint32_t lf_sync_abandon_descriptors( const struct lf_sync_arena *arena, uint32_t owner );

void lf_sync_init_event( const struct lf_sync_arena *arena, struct lf_sync_object *object,
                         uint32_t word, int manual, int signaled );
void lf_sync_init_semaphore( const struct lf_sync_arena *arena, struct lf_sync_object *object,
                             uint32_t word, uint32_t initial, uint32_t maximum );
void lf_sync_init_mutex( const struct lf_sync_arena *arena, struct lf_sync_object *object,
                         uint32_t word, uint32_t owner, uint32_t count );

enum lf_sync_result lf_sync_set_event( const struct lf_sync_arena *arena,
                                       const struct lf_sync_object *object, uint32_t *previous );
enum lf_sync_result lf_sync_reset_event( const struct lf_sync_arena *arena,
                                         const struct lf_sync_object *object, uint32_t *previous );
enum lf_sync_result lf_sync_release_semaphore( const struct lf_sync_arena *arena,
                                               const struct lf_sync_object *object, uint32_t count,
                                               uint32_t *previous );
enum lf_sync_result lf_sync_release_mutex( const struct lf_sync_arena *arena,
                                           const struct lf_sync_object *object, uint32_t owner,
                                           uint32_t *previous );
enum lf_sync_result lf_sync_abandon_mutex( const struct lf_sync_arena *arena,
                                           const struct lf_sync_object *object, uint32_t owner );
enum lf_sync_result lf_sync_query_event( const struct lf_sync_arena *arena,
                                         const struct lf_sync_object *object,
                                         uint32_t *manual, uint32_t *signaled );
enum lf_sync_result lf_sync_query_semaphore( const struct lf_sync_arena *arena,
                                             const struct lf_sync_object *object,
                                             uint32_t *count, uint32_t *maximum );
enum lf_sync_result lf_sync_query_mutex( const struct lf_sync_arena *arena,
                                         const struct lf_sync_object *object, uint32_t owner,
                                         uint32_t *count, uint32_t *owned, uint32_t *abandoned );

/* Try to acquire one object or all objects. On a successful wait-any, index
 * receives the acquired object's input index. */
enum lf_sync_result lf_sync_try_wait( const struct lf_sync_arena *arena,
                                      const struct lf_sync_object *const *objects,
                                      uint32_t count, int wait_all, uint32_t owner, uint32_t *index );

uint64_t lf_sync_wait_value( uint64_t generation, enum lf_sync_wait_status status, uint32_t index );

/* As above, but change the wait status in the same MCAS transaction as the
 * object acquisition. This makes completion atomic with timeout/APC CAS. */
enum lf_sync_result lf_sync_try_wait_status( const struct lf_sync_arena *arena,
                                             const struct lf_sync_object *const *objects,
                                             uint32_t count, int wait_all, uint32_t owner,
                                             uint32_t status_word, uint64_t waiting );

int lf_sync_wait_begin( const struct lf_sync_dispatcher *dispatcher, const uint32_t *objects,
                        uint32_t count, int wait_all, uint32_t owner,
                        struct lf_sync_wait_ticket *ticket );
int lf_sync_wait_begin_alert( const struct lf_sync_dispatcher *dispatcher, const uint32_t *objects,
                              uint32_t count, int wait_all, uint32_t owner, uint32_t alert_object,
                              struct lf_sync_wait_ticket *ticket );
enum lf_sync_result lf_sync_signal_and_wait_begin( const struct lf_sync_dispatcher *dispatcher,
                                                   uint32_t signal_object, uint32_t wait_object,
                                                   uint32_t owner, uint32_t alert_object,
                                                   struct lf_sync_wait_ticket *ticket );
uint64_t lf_sync_wait_poll( const struct lf_sync_dispatcher *dispatcher,
                            const struct lf_sync_wait_ticket *ticket );
int lf_sync_wait_park( const struct lf_sync_dispatcher *dispatcher,
                       const struct lf_sync_wait_ticket *ticket, const void *timeout );
int lf_sync_wait_timeout( const struct lf_sync_dispatcher *dispatcher,
                          const struct lf_sync_wait_ticket *ticket );
void lf_sync_wait_end( const struct lf_sync_dispatcher *dispatcher,
                       const struct lf_sync_wait_ticket *ticket );
void lf_sync_wake_object( const struct lf_sync_dispatcher *dispatcher, uint32_t object );
enum lf_sync_result lf_sync_pulse_event( const struct lf_sync_dispatcher *dispatcher,
                                         uint32_t object, uint32_t *previous );
void lf_sync_abandon_waits( const struct lf_sync_dispatcher *dispatcher, uint32_t owner );

void lf_sync_init_shared( struct lf_sync_shared *shared );
int lf_sync_open_shared( struct lf_sync_dispatcher *dispatcher, struct lf_sync_shared *shared,
                         lf_sync_park_func park, lf_sync_wake_func wake );
int lf_sync_activate_lease( struct lf_sync_shared *shared, uint32_t slot, uint64_t *token );
int lf_sync_mark_lease_released( struct lf_sync_shared *shared, uint64_t token );
void lf_sync_notify_lease_release( struct lf_sync_shared *shared, uint64_t token );
int lf_sync_release_lease( struct lf_sync_shared *shared, uint64_t token );
int lf_sync_lease_is_released( const struct lf_sync_shared *shared, uint64_t token );
int lf_sync_free_lease( struct lf_sync_shared *shared, uint64_t token );
uint64_t lf_sync_take_released_leases( struct lf_sync_shared *shared, uint32_t word );
void lf_sync_set_owner_alive( const struct lf_sync_dispatcher *dispatcher, uint32_t owner, int alive );
int lf_sync_owner_alive( const struct lf_sync_dispatcher *dispatcher, uint32_t owner );
int lf_sync_alloc_object( struct lf_sync_dispatcher *dispatcher, enum lf_sync_object_type type,
                          uint32_t initial, uint32_t limit, uint32_t flags, uint32_t *index );
int lf_sync_free_object( struct lf_sync_dispatcher *dispatcher, uint32_t index );

#endif /* __WINE_LOCKFREE_SYNC_H */
