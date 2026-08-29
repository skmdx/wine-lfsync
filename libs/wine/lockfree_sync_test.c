/* Native tests for the lock-free NT synchronization MCAS core. */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "wine/lockfree_sync.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int failures;

#define ok(condition, ...) do { if (!(condition)) { fprintf( stderr, __VA_ARGS__ ); failures++; } } while (0)

struct fixture
{
    struct lf_sync_word words[5];
    struct lf_sync_mcas descs[8];
    struct lf_sync_arena arena;
};

static void init_fixture( struct fixture *f )
{
    memset( f, 0, sizeof(*f) );
    f->arena.words = f->words;
    f->arena.word_count = ARRAY_SIZE(f->words);
    f->arena.descs = f->descs;
    f->arena.desc_count = ARRAY_SIZE(f->descs);
}

struct completion_race
{
    struct fixture *fixture;
    struct lf_sync_object *event;
    uint64_t waiting;
    int acquired;
    int timed_out;
};

static void *run_completion( void *arg )
{
    struct completion_race *race = arg;
    const struct lf_sync_object *object = race->event;

    race->acquired = lf_sync_try_wait_status( &race->fixture->arena, &object, 1, 0, 10, 4,
                                               race->waiting ) == LF_SYNC_SUCCESS;
    return NULL;
}

static void *run_timeout( void *arg )
{
    struct completion_race *race = arg;
    uint64_t generation = race->waiting >> 16;

    race->timed_out = lf_sync_compare_exchange( &race->fixture->arena, 4, race->waiting,
                                                 lf_sync_wait_value( generation, LF_SYNC_WAIT_TIMED_OUT, 0 ) );
    return NULL;
}

static void test_completion_timeout_race(void)
{
    struct lf_sync_object event;
    struct fixture f;
    uint64_t generation;

    init_fixture( &f );
    lf_sync_init_event( &f.arena, &event, 0, 0, 1 );

    for (generation = 1; generation <= 10000; ++generation)
    {
        struct completion_race race = {.fixture = &f, .event = &event,
            .waiting = lf_sync_wait_value( generation, LF_SYNC_WAITING, 0 )};
        pthread_t completion, timeout;
        uint64_t status;

        f.words[0].value = 1;
        f.words[4].value = race.waiting;
        pthread_create( &completion, NULL, run_completion, &race );
        pthread_create( &timeout, NULL, run_timeout, &race );
        pthread_join( completion, NULL );
        pthread_join( timeout, NULL );

        status = lf_sync_load( &f.arena, 4 );
        ok( race.acquired + race.timed_out == 1, "completion/timeout race had %d winners\n",
            race.acquired + race.timed_out );
        if ((status & 0xff) == LF_SYNC_WAIT_COMPLETE)
            ok( lf_sync_load( &f.arena, 0 ) == 0, "completed wait did not consume event\n" );
        else
        {
            ok( (status & 0xff) == LF_SYNC_WAIT_TIMED_OUT, "invalid terminal wait status %llu\n",
                (unsigned long long)(status & 0xff) );
            ok( lf_sync_load( &f.arena, 0 ) == 1, "timed-out wait consumed event\n" );
        }
    }
    ok( !(f.descs[0].lifetime & 0xffffffff), "descriptor reference leaked after timeout races\n" );
}

struct increment_race
{
    struct fixture *fixture;
    unsigned int iterations;
};

static void *run_mcas_increment( void *arg )
{
    struct increment_race *race = arg;
    unsigned int completed = 0;

    while (completed < race->iterations)
    {
        struct lf_sync_mcas_entry entries[2];
        uint64_t first = lf_sync_load( &race->fixture->arena, 0 );
        uint64_t second = lf_sync_load( &race->fixture->arena, 1 );

        if (first != second) continue;
        entries[0] = (struct lf_sync_mcas_entry){0, 0, first, first + 1};
        entries[1] = (struct lf_sync_mcas_entry){1, 0, second, second + 1};
        if (lf_sync_mcas( &race->fixture->arena, entries, ARRAY_SIZE(entries) ) > 0) ++completed;
    }
    return NULL;
}

static void test_descriptor_reclamation_stress(void)
{
    struct fixture f;
    struct increment_race race = {&f, 10000};
    pthread_t threads[8];
    unsigned int i;

    init_fixture( &f );
    f.arena.desc_count = 1;
    for (i = 0; i < ARRAY_SIZE(threads); ++i) pthread_create( &threads[i], NULL, run_mcas_increment, &race );
    for (i = 0; i < ARRAY_SIZE(threads); ++i) pthread_join( threads[i], NULL );

    ok( lf_sync_load( &f.arena, 0 ) == 80000 && lf_sync_load( &f.arena, 1 ) == 80000,
        "descriptor reuse stress lost an update\n" );
    ok( !(f.descs[0].lifetime & 0xffffffff), "descriptor reference leaked after reuse stress\n" );
}

static uint64_t test_mcas_control( uint32_t owner, uint32_t generation,
                                   enum lf_sync_mcas_status status, int owner_ref )
{
    return ((uint64_t)owner << LF_SYNC_MCAS_CONTROL_OWNER_SHIFT) |
           ((uint64_t)generation << LF_SYNC_MCAS_CONTROL_GEN_SHIFT) | status |
           (owner_ref ? LF_SYNC_MCAS_CONTROL_OWNER_REF : 0);
}

static uint64_t test_mcas_tag( uint32_t index, uint32_t generation )
{
    return (UINT64_C(1) << 63) | ((uint64_t)generation << 20) | index;
}

static void test_dead_descriptor_reclamation(void)
{
    struct lf_sync_mcas_entry entry = {0, 0, 0, 1};
    struct fixture f;
    uint64_t control;
    unsigned int i;

    init_fixture( &f );

    /* Model death in the only window where no helper can discover the
     * descriptor: after its atomic owner claim but before publication. */
    f.descs[0].lifetime = (UINT64_C(1) << 32) | 1;
    f.descs[0].control = test_mcas_control( 77, 1, LF_SYNC_MCAS_PREPARING, 1 );
    ok( lf_sync_abandon_descriptors( &f.arena, 77 ) == 1,
        "preparing descriptor was not reclaimed\n" );
    control = f.descs[0].control;
    ok( !(control & LF_SYNC_MCAS_CONTROL_OWNER_REF), "preparing descriptor kept its owner reference\n" );
    ok( (control & LF_SYNC_MCAS_CONTROL_STATUS_MASK) == LF_SYNC_MCAS_ABORTED,
        "preparing descriptor was not aborted\n" );
    ok( !(f.descs[0].lifetime & UINT32_MAX), "preparing descriptor leaked a lifetime reference\n" );

    /* Model death after publication. Cleanup must help the operation to a
     * decision before dropping exactly the dead owner's reference. */
    f.descs[1].entries[0] = entry;
    f.descs[1].count = 1;
    f.descs[1].lifetime = (UINT64_C(2) << 32) | 1;
    f.descs[1].control = test_mcas_control( 78, 2, LF_SYNC_MCAS_ACTIVE, 1 );
    ok( lf_sync_abandon_descriptors( &f.arena, 78 ) == 1,
        "active descriptor was not reclaimed\n" );
    control = f.descs[1].control;
    ok( !(control & LF_SYNC_MCAS_CONTROL_OWNER_REF), "active descriptor kept its owner reference\n" );
    ok( (control & LF_SYNC_MCAS_CONTROL_STATUS_MASK) == LF_SYNC_MCAS_COMMITTED,
        "active descriptor was not helped to commit\n" );
    ok( f.words[0].value == 1, "reclaimed active descriptor did not publish its result\n" );
    ok( !(f.descs[1].lifetime & UINT32_MAX), "active descriptor leaked a lifetime reference\n" );

    entry.expected = 1;
    for (i = 0; i < 10000; ++i)
    {
        entry.desired = entry.expected + 1;
        ok( lf_sync_mcas_owned( &f.arena, &entry, 1, 79 ) == 1,
            "descriptor reuse failed after death cleanup at iteration %u\n", i );
        entry.expected = entry.desired;
    }
    for (i = 0; i < ARRAY_SIZE(f.descs); ++i)
        ok( !(f.descs[i].lifetime & UINT32_MAX),
            "descriptor %u leaked after post-cleanup reuse\n", i );
}

static void test_commit_and_abort(void)
{
    struct lf_sync_mcas_entry entries[] =
    {
        {0, 0, 10, 11},
        {1, 0, 20, 21},
        {2, 0, 30, 31},
    };
    struct fixture f;

    init_fixture( &f );
    f.words[0].value = 10;
    f.words[1].value = 20;
    f.words[2].value = 30;
    ok( lf_sync_mcas( &f.arena, entries, ARRAY_SIZE(entries) ), "commit failed\n" );
    ok( lf_sync_load( &f.arena, 0 ) == 11 && lf_sync_load( &f.arena, 1 ) == 21 &&
        lf_sync_load( &f.arena, 2 ) == 31, "commit was not atomic\n" );

    entries[0].expected = 11;
    entries[0].desired = 12;
    entries[1].expected = 21;
    entries[1].desired = 22;
    entries[2].expected = 31;
    entries[2].desired = 32;
    ok( lf_sync_mcas( &f.arena, entries, ARRAY_SIZE(entries) ), "reclaimed descriptor failed\n" );
    ok( lf_sync_load( &f.arena, 0 ) == 12 && lf_sync_load( &f.arena, 1 ) == 22 &&
        lf_sync_load( &f.arena, 2 ) == 32, "reclaimed descriptor produced wrong values\n" );

    entries[0].expected = 12;
    entries[0].desired = 13;
    entries[1].expected = 999;
    entries[2].expected = 32;
    entries[2].desired = 33;
    ok( !lf_sync_mcas( &f.arena, entries, ARRAY_SIZE(entries) ), "mismatched transaction committed\n" );
    ok( lf_sync_load( &f.arena, 0 ) == 12 && lf_sync_load( &f.arena, 1 ) == 22 &&
        lf_sync_load( &f.arena, 2 ) == 32, "abort left a partial update\n" );
}

static void test_invalid_mcas_entries(void)
{
    struct lf_sync_mcas_entry entries[2] = {{0, 0, 0, 1}, {1, 0, 0, 1}};
    struct fixture f;

    init_fixture( &f );
    ok( !lf_sync_mcas( &f.arena, entries, 0 ), "empty transaction was accepted\n" );
    entries[0].word = ARRAY_SIZE(f.words);
    ok( !lf_sync_mcas( &f.arena, entries, 1 ), "out-of-range transaction word was accepted\n" );
    entries[0].word = 0;
    entries[0].expected = UINT64_C(1) << 63;
    ok( !lf_sync_mcas( &f.arena, entries, 1 ), "descriptor-tag expected value was accepted\n" );
    entries[0].expected = 0;
    entries[0].desired = UINT64_C(1) << 63;
    ok( !lf_sync_mcas( &f.arena, entries, 1 ), "descriptor-tag desired value was accepted\n" );
    entries[0] = (struct lf_sync_mcas_entry){1, 0, 0, 1};
    entries[1] = (struct lf_sync_mcas_entry){0, 0, 0, 1};
    ok( !lf_sync_mcas( &f.arena, entries, 2 ), "unordered transaction was accepted\n" );
    entries[1].word = 1;
    ok( !lf_sync_mcas( &f.arena, entries, 2 ), "duplicate transaction word was accepted\n" );
}

struct race
{
    struct fixture *fixture;
    int result;
};

static void *run_wait_all( void *arg )
{
    struct race *race = arg;
    struct lf_sync_mcas_entry entries[] =
    {
        {0, 0, 100, 101}, /* auto-reset event: signaled -> nonsignaled + version */
        {1, 0, 200, 201}, /* semaphore: count 1 -> 0 + version */
    };

    race->result = lf_sync_mcas( &race->fixture->arena, entries, ARRAY_SIZE(entries) );
    return NULL;
}

static void test_competing_wait_all(void)
{
    struct fixture f;
    struct race races[2] = {{&f, 0}, {&f, 0}};
    pthread_t threads[2];

    init_fixture( &f );
    f.words[0].value = 100;
    f.words[1].value = 200;
    pthread_create( &threads[0], NULL, run_wait_all, &races[0] );
    pthread_create( &threads[1], NULL, run_wait_all, &races[1] );
    pthread_join( threads[0], NULL );
    pthread_join( threads[1], NULL );

    ok( races[0].result + races[1].result == 1, "expected exactly one WaitAll winner, got %d\n",
        races[0].result + races[1].result );
    ok( lf_sync_load( &f.arena, 0 ) == 101 && lf_sync_load( &f.arena, 1 ) == 201,
        "winning WaitAll did not consume every object\n" );
}

static void test_large_unordered_wait_all(void)
{
    struct lf_sync_mcas desc;
    struct lf_sync_word words[LF_SYNC_MAX_WAIT_OBJECTS];
    struct lf_sync_object objects[LF_SYNC_MAX_WAIT_OBJECTS];
    const struct lf_sync_object *wait_objects[LF_SYNC_MAX_WAIT_OBJECTS];
    struct lf_sync_arena arena = {words, ARRAY_SIZE(words), &desc, 1, 0, 0};
    uint32_t i, index;

    memset( &desc, 0, sizeof(desc) );
    memset( words, 0, sizeof(words) );
    for (i = 0; i < ARRAY_SIZE(objects); ++i)
    {
        lf_sync_init_event( &arena, &objects[i], ARRAY_SIZE(objects) - i - 1, 0, 1 );
        wait_objects[i] = &objects[i];
    }

    ok( lf_sync_try_wait( &arena, wait_objects, ARRAY_SIZE(wait_objects), 1, 10, &index ) ==
        LF_SYNC_SUCCESS, "large reverse-ordered WaitAll failed\n" );
    for (i = 0; i < ARRAY_SIZE(words); ++i)
        ok( !lf_sync_load( &arena, i ), "large WaitAll did not consume object %u\n", i );

    for (i = 0; i < ARRAY_SIZE(objects); ++i)
        lf_sync_set_event( &arena, &objects[i], NULL );
    wait_objects[ARRAY_SIZE(wait_objects) - 1] = wait_objects[ARRAY_SIZE(wait_objects) - 2];
    ok( lf_sync_try_wait( &arena, wait_objects, ARRAY_SIZE(wait_objects), 1, 10, &index ) ==
        LF_SYNC_INVALID, "large WaitAll did not reject a duplicate object\n" );
}

static void test_nt_object_transitions(void)
{
    struct lf_sync_object auto_event, manual_event, semaphore, mutex;
    const struct lf_sync_object *wait_objects[3];
    struct fixture f;
    uint32_t index, previous;

    init_fixture( &f );
    lf_sync_init_event( &f.arena, &auto_event, 0, 0, 1 );
    lf_sync_init_event( &f.arena, &manual_event, 1, 1, 1 );
    lf_sync_init_semaphore( &f.arena, &semaphore, 2, 1, 2 );
    lf_sync_init_mutex( &f.arena, &mutex, 3, 0, 0 );

    wait_objects[0] = &auto_event;
    wait_objects[1] = &manual_event;
    wait_objects[2] = &semaphore;
    ok( lf_sync_try_wait( &f.arena, wait_objects, 3, 1, 10, &index ) == LF_SYNC_SUCCESS,
        "WaitAll over event/event/semaphore failed\n" );
    ok( lf_sync_load( &f.arena, 0 ) == 0, "auto-reset event was not consumed\n" );
    ok( lf_sync_load( &f.arena, 1 ) == 1, "manual-reset event was consumed\n" );
    ok( lf_sync_load( &f.arena, 2 ) == 0, "semaphore was not consumed\n" );
    ok( lf_sync_try_wait( &f.arena, wait_objects, 3, 1, 10, &index ) == LF_SYNC_UNSATISFIED,
        "unsignaled WaitAll succeeded\n" );
    ok( lf_sync_load( &f.arena, 1 ) == 1, "failed WaitAll changed the manual event\n" );

    ok( lf_sync_set_event( &f.arena, &auto_event, &previous ) == LF_SYNC_SUCCESS && !previous,
        "setting event returned wrong previous state\n" );
    ok( lf_sync_release_semaphore( &f.arena, &semaphore, 2, &previous ) == LF_SYNC_SUCCESS && !previous,
        "semaphore release failed\n" );
    ok( lf_sync_release_semaphore( &f.arena, &semaphore, 1, NULL ) == LF_SYNC_LIMIT_EXCEEDED,
        "semaphore limit was not enforced\n" );

    wait_objects[0] = &mutex;
    ok( lf_sync_try_wait( &f.arena, wait_objects, 1, 0, 10, &index ) == LF_SYNC_SUCCESS,
        "free mutex acquisition failed\n" );
    ok( lf_sync_try_wait( &f.arena, wait_objects, 1, 0, 10, &index ) == LF_SYNC_SUCCESS,
        "recursive mutex acquisition failed\n" );
    ok( lf_sync_try_wait( &f.arena, wait_objects, 1, 0, 11, &index ) == LF_SYNC_UNSATISFIED,
        "mutex was acquired by a non-owner\n" );
    ok( lf_sync_release_mutex( &f.arena, &mutex, 11, NULL ) == LF_SYNC_NOT_OWNER,
        "non-owner released mutex\n" );
    ok( lf_sync_release_mutex( &f.arena, &mutex, 10, &previous ) == LF_SYNC_SUCCESS && previous == 2,
        "recursive mutex release returned wrong count\n" );
    ok( lf_sync_abandon_mutex( &f.arena, &mutex, 10 ) == LF_SYNC_SUCCESS, "mutex abandonment failed\n" );
    ok( lf_sync_try_wait( &f.arena, wait_objects, 1, 0, 11, &index ) == LF_SYNC_ABANDONED,
        "abandoned mutex was not reported\n" );
    ok( lf_sync_try_wait( &f.arena, wait_objects, 1, 0, 11, &index ) == LF_SYNC_SUCCESS,
        "new mutex owner could not recurse\n" );

    lf_sync_init_mutex( &f.arena, &mutex, 3, 11, 0x7fffffff );
    ok( lf_sync_try_wait( &f.arena, wait_objects, 1, 0, 11, &index ) == LF_SYNC_LIMIT_EXCEEDED,
        "31-bit mutex recursion limit was not enforced\n" );
}

static void test_indexed_mutex_abandonment(void)
{
    struct lf_sync_dispatcher dispatcher;
    struct lf_sync_shared *shared;
    const struct lf_sync_object *mutex;
    uint32_t event_idx, owned_idx, other_idx, second_idx, index;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate shared arena fixture\n" );
    if (!shared) return;

    lf_sync_init_shared( shared );
    ok( lf_sync_open_shared( &dispatcher, shared, NULL, NULL ), "failed to open shared arena fixture\n" );
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_EVENT, 0, 0, 0, &event_idx ),
        "failed to allocate arena event\n" );
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_MUTEX, 1, 0, 77, &owned_idx ),
        "failed to allocate first owned arena mutex\n" );
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_MUTEX, 1, 0, 88, &other_idx ),
        "failed to allocate other owner's arena mutex\n" );
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_MUTEX, 1, 0, 77, &second_idx ),
        "failed to allocate second owned arena mutex\n" );

    ok( lf_sync_abandon_mutex( &dispatcher.arena, &dispatcher.objects[owned_idx], 77 ) ==
        LF_SYNC_SUCCESS, "failed to abandon first indexed mutex\n" );
    ok( lf_sync_abandon_mutex( &dispatcher.arena, &dispatcher.objects[second_idx], 77 ) ==
        LF_SYNC_SUCCESS, "failed to abandon second indexed mutex\n" );
    ok( lf_sync_abandon_mutex( &dispatcher.arena, &dispatcher.objects[other_idx], 77 ) ==
        LF_SYNC_NOT_OWNER, "indexed cleanup changed another owner's mutex\n" );
    mutex = &dispatcher.objects[owned_idx];
    ok( lf_sync_try_wait( &dispatcher.arena, &mutex, 1, 0, 99, &index ) == LF_SYNC_ABANDONED,
        "first arena mutex was not abandoned\n" );
    mutex = &dispatcher.objects[second_idx];
    ok( lf_sync_try_wait( &dispatcher.arena, &mutex, 1, 0, 99, &index ) == LF_SYNC_ABANDONED,
        "second arena mutex was not abandoned\n" );
    mutex = &dispatcher.objects[other_idx];
    ok( lf_sync_try_wait( &dispatcher.arena, &mutex, 1, 0, 99, &index ) == LF_SYNC_UNSATISFIED,
        "arena abandonment changed another owner's mutex\n" );

    free( shared );
}

static void test_owner_death_transaction_ordering(void)
{
    struct lf_sync_dispatcher dispatcher;
    struct lf_sync_shared *shared;
    struct lf_sync_mcas *desc;
    uint32_t count, index, owned, owner = 84;
    uint64_t tag;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate owner-death fixture\n" );
    if (!shared) return;

    lf_sync_init_shared( shared );
    ok( lf_sync_open_shared( &dispatcher, shared, NULL, NULL ),
        "failed to open owner-death fixture\n" );
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_MUTEX, 0, 0, 0, &index ),
        "failed to allocate owner-death mutex\n" );

    /* Model an ACTIVE acquire which has not tagged the owner epoch yet. Once
     * death linearizes, helping must abort instead of installing a dead owner. */
    desc = &shared->descs[0];
    desc->entries[0] = (struct lf_sync_mcas_entry){index, 0, 0, (UINT64_C(1) << 32) | owner};
    desc->entries[1] = (struct lf_sync_mcas_entry){dispatcher.arena.owner_word_base + (owner >> 2), 0, 0, 0};
    desc->count = 2;
    desc->lifetime = (UINT64_C(1) << 32) | 1;
    desc->control = test_mcas_control( owner, 1, LF_SYNC_MCAS_ACTIVE, 1 );
    lf_sync_set_owner_alive( &dispatcher, owner, 0 );
    ok( lf_sync_abandon_descriptors( &dispatcher.arena, owner ) == 1,
        "death did not settle the active acquire\n" );
    lf_sync_query_mutex( &dispatcher.arena, &dispatcher.objects[index], owner, &count, &owned, NULL );
    ok( !count && !owned, "transaction ordered after death installed the dead mutex owner\n" );

    /* A one-shot death scan can miss PREPARING publication. Allocation must
     * reclaim such a dead-owner slot instead of leaking it permanently. */
    desc->lifetime = (UINT64_C(2) << 32) | 1;
    desc->control = test_mcas_control( owner, 2, LF_SYNC_MCAS_PREPARING, 1 );
    dispatcher.arena.desc_count = 1;
    {
        struct lf_sync_mcas_entry entry = {index, 0, 0, 0};
        ok( lf_sync_mcas_owned( &dispatcher.arena, &entry, 1, 88 ) == 1,
            "allocator did not reclaim a missed dead-owner descriptor\n" );
    }
    dispatcher.arena.desc_count = LF_SYNC_SHARED_DESCS;

    /* Conversely, if the transaction tags the epoch first, the death-side
     * CAS helps it commit and indexed cleanup must abandon the result. */
    lf_sync_set_owner_alive( &dispatcher, owner, 1 );
    tag = test_mcas_tag( 0, 4 );
    desc->entries[0] = (struct lf_sync_mcas_entry){index, 0, 0, (UINT64_C(1) << 32) | owner};
    desc->entries[1] = (struct lf_sync_mcas_entry){dispatcher.arena.owner_word_base + (owner >> 2), 0, 2, 2};
    desc->count = 2;
    desc->lifetime = (UINT64_C(4) << 32) | 1;
    desc->control = test_mcas_control( owner, 4, LF_SYNC_MCAS_ACTIVE, 1 );
    shared->words[index].value = tag;
    shared->words[desc->entries[1].word].value = tag;
    lf_sync_set_owner_alive( &dispatcher, owner, 0 );
    lf_sync_abandon_descriptors( &dispatcher.arena, owner );
    ok( lf_sync_abandon_mutex( &dispatcher.arena, &dispatcher.objects[index], owner ) == LF_SYNC_SUCCESS,
        "transaction ordered before death was not abandoned by the indexed cleanup\n" );
    lf_sync_query_mutex( &dispatcher.arena, &dispatcher.objects[index], owner, &count, &owned, NULL );
    ok( !count && !owned, "owner-death cleanup left a mutex owned\n" );

    free( shared );
}

static void test_mcas_cleanup_waits_for_active_helpers(void)
{
    struct fixture f;
    struct lf_sync_mcas *desc;
    const uint32_t generation = 7, owner = 77;
    uint64_t tag;

    init_fixture( &f );
    desc = &f.descs[0];
    tag = test_mcas_tag( 0, generation );

    /* Model an owner reference plus a helper which entered while ACTIVE and
     * then stalled.  A second helper may decide the transaction, but it must
     * not remove the tags while the first helper can still install another
     * one from an expected value which has undergone an ABA transition. */
    desc->entries[0] = (struct lf_sync_mcas_entry){0, 0, 0, 1};
    desc->count = 1;
    desc->lifetime = ((uint64_t)generation << 32) | 2;
    desc->control = test_mcas_control( owner, generation, LF_SYNC_MCAS_ACTIVE, 1 );
    f.words[0].value = tag;

    ok( lf_sync_abandon_descriptors( &f.arena, owner ) == 1,
        "failed to settle descriptor with an active helper\n" );
    ok( f.words[0].value == tag,
        "descriptor tags were cleaned while an active helper could re-tag the word\n" );
    ok( (desc->lifetime & 0xffffffff) == 1,
        "active helper reference was not preserved\n" );
}

static void test_active_mcas_resolves_decided_dependency(void)
{
    struct fixture f;
    struct lf_sync_mcas *active, *decided;
    const uint32_t active_generation = 8, decided_generation = 9;

    init_fixture( &f );
    active = &f.descs[0];
    decided = &f.descs[1];

    /* Model a helper which died after another thread decided the descriptor
     * but before the helper dropped its lifetime reference and cleaned the
     * tag.  A later active transaction must be able to acquire the decided
     * generation long enough to remove that tag. */
    decided->entries[0] = (struct lf_sync_mcas_entry){1, 0, 20, 21};
    decided->count = 1;
    decided->lifetime = ((uint64_t)decided_generation << 32) | 1;
    decided->control = test_mcas_control( 77, decided_generation, LF_SYNC_MCAS_COMMITTED, 0 );
    f.words[1].value = test_mcas_tag( 1, decided_generation );

    active->entries[0] = (struct lf_sync_mcas_entry){0, 0, 10, 11};
    active->entries[1] = (struct lf_sync_mcas_entry){1, 0, 21, 21};
    active->count = 2;
    active->lifetime = ((uint64_t)active_generation << 32) | 1;
    active->control = test_mcas_control( 78, active_generation, LF_SYNC_MCAS_ACTIVE, 1 );
    f.words[0].value = test_mcas_tag( 0, active_generation );

    ok( lf_sync_load( &f.arena, 0 ) == 11,
        "active descriptor did not resolve a decided dependency\n" );
    ok( lf_sync_load( &f.arena, 1 ) == 21,
        "decided dependency tag was not cleaned\n" );
    ok( (active->control & LF_SYNC_MCAS_CONTROL_STATUS_MASK) == LF_SYNC_MCAS_COMMITTED,
        "dependent descriptor did not commit\n" );
}

static void test_object_reuse(void)
{
    struct lf_sync_mcas_entry entry;
    struct lf_sync_wait_ticket ticket;
    struct lf_sync_dispatcher dispatcher;
    struct lf_sync_shared *shared;
    uint32_t first, reused, object, i;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate object reuse fixture\n" );
    if (!shared) return;
    lf_sync_init_shared( shared );
    ok( lf_sync_open_shared( &dispatcher, shared, NULL, NULL ), "failed to open object reuse fixture\n" );

    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_EVENT, 0, 0, 0, &first ),
        "failed to allocate reusable event\n" );
    lf_sync_set_owner_alive( &dispatcher, 84, 0 );
    object = first;
    ok( !lf_sync_wait_begin( &dispatcher, &object, 1, 0, 84, &ticket ),
        "dead owner published a wait\n" );
    entry.word = first; /* Shared object words are indexed one-to-one. */
    entry.pad = 0;
    entry.expected = 0;
    entry.desired = 1;
    ok( lf_sync_mcas_owned( &dispatcher.arena, &entry, 1, 84 ) < 0,
        "dead owner published a descriptor\n" );
    lf_sync_set_owner_alive( &dispatcher, 84, 1 );
    ok( lf_sync_mcas_owned( &dispatcher.arena, &entry, 1, 84 ) == 1,
        "live reused owner could not publish a descriptor\n" );
    ok( lf_sync_reset_event( &dispatcher.arena, &dispatcher.objects[first], NULL ) == LF_SYNC_SUCCESS,
        "failed to reset owner-gating event\n" );
    object = first;
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 80, &ticket ),
        "failed to register reuse-blocking waiter\n" );
    ok( (dispatcher.waits[ticket.slot].lifetime & UINT32_MAX) == 1,
        "published wait retained a temporary lifetime reference\n" );
    ok( !lf_sync_free_object( &dispatcher, first ), "object with a waiter was prematurely freed\n" );
    lf_sync_wait_timeout( &dispatcher, &ticket );
    lf_sync_wait_end( &dispatcher, &ticket );
    ok( lf_sync_free_object( &dispatcher, first ), "quiescent event was not freed\n" );
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_EVENT, 1, 0, 0, &reused ) && reused == first,
        "freed event slot was not reused\n" );
    ok( lf_sync_free_object( &dispatcher, reused ), "reused event could not be freed again\n" );

    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_MUTEX, 1, 0, 81, &reused ) && reused == first,
        "failed to reuse slot for mutex\n" );
    ok( !lf_sync_free_object( &dispatcher, reused ), "owned mutex was prematurely freed\n" );
    ok( lf_sync_abandon_mutex( &dispatcher.arena, &dispatcher.objects[reused], 81 ) == LF_SYNC_SUCCESS,
        "failed to abandon reusable mutex\n" );
    ok( lf_sync_free_object( &dispatcher, reused ), "abandoned mutex was not freed\n" );
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_SEMAPHORE, 1, 2, 0, &object ) && object == first,
        "failed to reuse slot for semaphore\n" );
    ok( lf_sync_load( &dispatcher.arena, object ) == 1 && dispatcher.objects[object].limit == 2,
        "reused object retained stale state\n" );

    for (i = 0; i < LF_SYNC_SHARED_OBJECTS + 1024; ++i)
    {
        if (!lf_sync_free_object( &dispatcher, object ) ||
            !lf_sync_alloc_object( &dispatcher, LF_SYNC_EVENT, i & 1, 0, 0, &reused ) || reused != first)
        {
            ok( 0, "object reuse failed after %u iterations\n", i );
            break;
        }
        object = reused;
    }
    ok( i == LF_SYNC_SHARED_OBJECTS + 1024, "object reuse exhausted after %u iterations\n", i );

    free( shared );
}

static void test_shared_initialization_does_not_touch_payload(void)
{
    struct lf_sync_shared *shared;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate initialization fixture\n" );
    if (!shared) return;
    shared->words[LF_SYNC_SHARED_WORDS - 1].value = UINT64_C(0x123456789abcdef0);
    shared->waits[LF_SYNC_SHARED_WAITS - 1].published = UINT64_C(0xfedcba9876543210);
    shared->leases[LF_SYNC_SHARED_LEASES - 1].control = UINT64_C(0x13579bdf2468ace0);
    shared->released_leases[LF_SYNC_SHARED_LEASE_WORDS - 1] = UINT64_C(0x55aa55aa55aa55aa);
    shared->next_object = 123;
    shared->free_object = 456;
    memset( shared->header_pad, 0x7b, sizeof(shared->header_pad) );

    lf_sync_init_shared( shared );
    ok( shared->magic == LF_SYNC_SHARED_MAGIC && shared->version == LF_SYNC_SHARED_VERSION &&
        !shared->next_object && shared->free_object == UINT32_MAX && !shared->header_pad[0] &&
        !shared->header_pad[sizeof(shared->header_pad) - 1],
        "shared initialization did not initialize its header\n" );
    ok( shared->words[LF_SYNC_SHARED_WORDS - 1].value == UINT64_C(0x123456789abcdef0) &&
        shared->waits[LF_SYNC_SHARED_WAITS - 1].published == UINT64_C(0xfedcba9876543210) &&
        shared->leases[LF_SYNC_SHARED_LEASES - 1].control == UINT64_C(0x13579bdf2468ace0) &&
        shared->released_leases[LF_SYNC_SHARED_LEASE_WORDS - 1] == UINT64_C(0x55aa55aa55aa55aa),
        "shared initialization touched zero-filled payload pages\n" );
    free( shared );
}

static void test_shared_cacheline_layout(void)
{
    ok( sizeof(struct lf_sync_mcas) % LF_SYNC_CACHELINE_SIZE == 0,
        "MCAS descriptor stride is not cache-line aligned\n" );
    ok( sizeof(struct lf_sync_waiter_bucket) % LF_SYNC_CACHELINE_SIZE == 0,
        "waiter bucket stride is not cache-line aligned\n" );
    ok( sizeof(struct lf_sync_wait) % LF_SYNC_CACHELINE_SIZE == 0,
        "wait slot stride is not cache-line aligned\n" );
    ok( sizeof(struct lf_sync_object) == 16,
        "shared object metadata is not compact\n" );
    ok( sizeof(struct lf_sync_wait_ticket) == 8,
        "wait ticket retained derived state\n" );
    ok( LF_SYNC_SHARED_OBJECTS <= LF_SYNC_OBJECT_WORD_MASK + 1,
        "shared object indices do not fit packed object metadata\n" );
    ok( offsetof(struct lf_sync_object, limit) == offsetof(struct lf_sync_object, next_free),
        "allocated and free object metadata do not share storage\n" );
    ok( offsetof(struct lf_sync_shared, words) % LF_SYNC_CACHELINE_SIZE == 0,
        "shared words are not cache-line aligned\n" );
    ok( offsetof(struct lf_sync_shared, descs) % LF_SYNC_CACHELINE_SIZE == 0,
        "shared descriptors are not cache-line aligned\n" );
    ok( offsetof(struct lf_sync_shared, objects) % LF_SYNC_CACHELINE_SIZE == 0,
        "shared objects are not cache-line aligned\n" );
    ok( offsetof(struct lf_sync_shared, waiter_buckets) % LF_SYNC_CACHELINE_SIZE == 0,
        "shared waiter buckets are not cache-line aligned\n" );
    ok( offsetof(struct lf_sync_shared, waits) % LF_SYNC_CACHELINE_SIZE == 0,
        "shared waits are not cache-line aligned\n" );
    ok( offsetof(struct lf_sync_shared, leases) % LF_SYNC_CACHELINE_SIZE == 0,
        "shared leases are not cache-line aligned\n" );
    ok( offsetof(struct lf_sync_shared, released_leases) % LF_SYNC_CACHELINE_SIZE == 0,
        "released lease bitmap is not cache-line aligned\n" );
}

static void test_open_shared_initializes_dispatcher(void)
{
    struct lf_sync_dispatcher dispatcher;
    struct lf_sync_shared *shared;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate dispatcher fixture\n" );
    if (!shared) return;
    lf_sync_init_shared( shared );
    memset( &dispatcher, 0xa5, sizeof(dispatcher) );
    ok( lf_sync_open_shared( &dispatcher, shared, NULL, NULL ),
        "failed to open poisoned dispatcher fixture\n" );
    ok( dispatcher.shared == shared && dispatcher.arena.words == shared->words &&
        dispatcher.arena.word_count == LF_SYNC_SHARED_WORDS &&
        dispatcher.arena.descs == shared->descs &&
        dispatcher.arena.desc_count == LF_SYNC_SHARED_DESCS &&
        dispatcher.arena.owner_word_base == LF_SYNC_SHARED_OBJECTS + LF_SYNC_SHARED_WAITS &&
        dispatcher.arena.owner_count == LF_SYNC_SHARED_OWNERS &&
        dispatcher.objects == shared->objects && dispatcher.object_count == LF_SYNC_SHARED_OBJECTS &&
        dispatcher.waiter_buckets == shared->waiter_buckets &&
        dispatcher.waiter_bucket_count == LF_SYNC_SHARED_WAITER_BUCKETS &&
        dispatcher.waits == shared->waits && dispatcher.wait_count == LF_SYNC_SHARED_WAITS &&
        dispatcher.status_word_base == LF_SYNC_SHARED_OBJECTS && !dispatcher.park && !dispatcher.wake,
        "shared dispatcher retained poisoned fields\n" );
    free( shared );
}

static void test_packed_object_boundary(void)
{
    struct lf_sync_dispatcher dispatcher;
    struct lf_sync_shared *shared;
    uint32_t index, manual, signaled;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate packed object fixture\n" );
    if (!shared) return;
    lf_sync_init_shared( shared );
    ok( lf_sync_open_shared( &dispatcher, shared, NULL, NULL ),
        "failed to open packed object fixture\n" );
    shared->next_object = LF_SYNC_SHARED_OBJECTS - 1;
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_EVENT, 1, 0, LF_SYNC_EVENT_MANUAL, &index ) &&
        index == LF_SYNC_SHARED_OBJECTS - 1,
        "failed to allocate the highest packed object index\n" );
    ok( lf_sync_query_event( &dispatcher.arena, &dispatcher.objects[index], &manual, &signaled ) ==
        LF_SYNC_SUCCESS && manual && signaled,
        "highest packed object index lost type, flags, or word bits\n" );
    ok( lf_sync_reset_event( &dispatcher.arena, &dispatcher.objects[index], NULL ) == LF_SYNC_SUCCESS,
        "highest packed object index addressed the wrong state word\n" );
    ok( !lf_sync_load( &dispatcher.arena, index ),
        "highest packed object index did not update its matching state word\n" );
    free( shared );
}

static void test_lease_state_machine(void)
{
    struct lf_sync_shared *shared;
    uint64_t first, second;
    const uint32_t slot = 73;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate lease fixture\n" );
    if (!shared) return;
    lf_sync_init_shared( shared );

    ok( lf_sync_activate_lease( shared, slot, &first ), "failed to activate first lease\n" );
    ok( (first & LF_SYNC_LEASE_SLOT_MASK) == slot && first >> LF_SYNC_LEASE_SLOT_BITS,
        "first lease token has invalid slot or generation\n" );
    ok( lf_sync_mark_lease_released( shared, first ), "failed to mark first lease released\n" );
    ok( lf_sync_lease_is_released( shared, first ), "released lease state was not authoritative\n" );
    ok( !lf_sync_take_released_leases( shared, slot / 64 ),
        "mark-only release unexpectedly published a bitmap notification\n" );
    lf_sync_notify_lease_release( shared, first );
    ok( lf_sync_take_released_leases( shared, slot / 64 ) == (UINT64_C(1) << (slot % 64)),
        "release bitmap did not report the released lease\n" );
    ok( lf_sync_free_lease( shared, first ), "failed to free first lease\n" );

    ok( lf_sync_activate_lease( shared, slot, &second ), "failed to reuse lease slot\n" );
    ok( first != second, "reused lease slot did not advance its generation\n" );
    ok( !lf_sync_mark_lease_released( shared, first ), "stale lease token released a new lease\n" );
    lf_sync_notify_lease_release( shared, first );
    ok( lf_sync_take_released_leases( shared, slot / 64 ) == (UINT64_C(1) << (slot % 64)),
        "stale bitmap notification was not observable as a hint\n" );
    ok( !lf_sync_lease_is_released( shared, second ), "stale bitmap changed the active lease\n" );
    ok( lf_sync_release_lease( shared, second ), "normal final lease release failed\n" );
    ok( lf_sync_lease_is_released( shared, second ), "normal release did not publish authoritative state\n" );
    ok( lf_sync_free_lease( shared, second ), "failed to free reused lease\n" );

    free( shared );
}

#ifdef __linux__

struct shared_fixture
{
    struct lf_sync_word words[4];
    struct lf_sync_mcas descs[8];
    struct lf_sync_object objects[2];
    struct lf_sync_waiter_bucket waiter_buckets[2];
    struct lf_sync_wait waits[2];
};

static int futex_park( uint32_t *address, uint32_t expected, const void *timeout )
{
    return syscall( SYS_futex, address, FUTEX_WAIT, expected, timeout, NULL, 0 );
}

static void futex_wake( uint32_t *address )
{
    syscall( SYS_futex, address, FUTEX_WAKE, 1, NULL, NULL, 0 );
}

static unsigned int wake_calls;

static void count_wake( uint32_t *address )
{
    (void)address;
    ++wake_calls;
}

static void init_dispatcher( struct lf_sync_dispatcher *dispatcher, struct shared_fixture *shared )
{
    memset( dispatcher, 0, sizeof(*dispatcher) );
    dispatcher->arena.words = shared->words;
    dispatcher->arena.word_count = ARRAY_SIZE(shared->words);
    dispatcher->arena.descs = shared->descs;
    dispatcher->arena.desc_count = ARRAY_SIZE(shared->descs);
    dispatcher->objects = shared->objects;
    dispatcher->object_count = ARRAY_SIZE(shared->objects);
    dispatcher->waiter_buckets = shared->waiter_buckets;
    dispatcher->waiter_bucket_count = ARRAY_SIZE(shared->waiter_buckets);
    dispatcher->waits = shared->waits;
    dispatcher->wait_count = ARRAY_SIZE(shared->waits);
    dispatcher->status_word_base = 2;
    dispatcher->park = futex_park;
    dispatcher->wake = futex_wake;
}

static void test_shared_parking(void)
{
    struct lf_sync_dispatcher dispatcher;
    struct shared_fixture *shared;
    int ready[2], status;
    pid_t child;
    char byte;

    shared = mmap( NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0 );
    ok( shared != MAP_FAILED, "failed to allocate shared dispatcher\n" );
    if (shared == MAP_FAILED) return;
    memset( shared, 0, sizeof(*shared) );
    init_dispatcher( &dispatcher, shared );
    lf_sync_init_event( &dispatcher.arena, &shared->objects[0], 0, 0, 0 );
    ok( pipe( ready ) == 0, "failed to create readiness pipe\n" );

    child = fork();
    ok( child >= 0, "failed to fork parking test\n" );
    if (!child)
    {
        struct lf_sync_dispatcher child_dispatcher;
        struct lf_sync_wait_ticket ticket;
        uint32_t object = 0;
        uint64_t result;

        close( ready[0] );
        init_dispatcher( &child_dispatcher, shared );
        if (!lf_sync_wait_begin( &child_dispatcher, &object, 1, 0, 77, &ticket )) _exit( 2 );
        if (write( ready[1], "x", 1 ) != 1) _exit( 3 );
        while ((result = lf_sync_wait_poll( &child_dispatcher, &ticket )) ==
               lf_sync_wait_value( ticket.generation, LF_SYNC_WAITING, 0 ))
            if (lf_sync_wait_park( &child_dispatcher, &ticket, NULL ) < 0 && errno != EINTR && errno != EAGAIN)
                _exit( 4 );
        lf_sync_wait_end( &child_dispatcher, &ticket );
        _exit( (result & 0xff) == LF_SYNC_WAIT_COMPLETE ? 0 : 5 );
    }

    close( ready[1] );
    if (child > 0)
    {
        if (read( ready[0], &byte, 1 ) != 1) failures++;
        lf_sync_set_event( &dispatcher.arena, &shared->objects[0], NULL );
        lf_sync_wake_object( &dispatcher, 0 );
        waitpid( child, &status, 0 );
        ok( WIFEXITED(status) && !WEXITSTATUS(status), "cross-process waiter failed with status %#x\n", status );
        ok( lf_sync_load( &dispatcher.arena, 0 ) == 0, "cross-process wait did not consume event\n" );
    }
    close( ready[0] );
    munmap( shared, sizeof(*shared) );
}

static void test_registered_timeout(void)
{
    struct lf_sync_dispatcher dispatcher;
    struct shared_fixture shared = {0};
    struct lf_sync_wait_ticket ticket;
    uint32_t object = 0;
    uint64_t result;

    init_dispatcher( &dispatcher, &shared );
    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 88, &ticket ), "wait registration failed\n" );
    ok( shared.waiter_buckets[0].waiters[0] & (UINT64_C(1) << ticket.slot),
        "waiter was not registered on its object\n" );
    ok( lf_sync_wait_timeout( &dispatcher, &ticket ), "registered timeout lost without a signal\n" );
    lf_sync_set_event( &dispatcher.arena, &shared.objects[0], NULL );
    lf_sync_wake_object( &dispatcher, 0 );
    result = lf_sync_wait_poll( &dispatcher, &ticket );
    ok( (result & 0xff) == LF_SYNC_WAIT_TIMED_OUT, "signal overwrote terminal timeout status\n" );
    ok( lf_sync_load( &dispatcher.arena, 0 ) == 1, "signal after timeout was incorrectly consumed\n" );
    lf_sync_wait_end( &dispatcher, &ticket );
    ok( !(shared.waiter_buckets[0].waiters[0] & (UINT64_C(1) << ticket.slot)),
        "waiter was not removed from its object\n" );
}

static void test_prepared_regular_wait(void)
{
    struct lf_sync_wait_ticket first, second;
    struct lf_sync_dispatcher dispatcher;
    struct shared_fixture shared = {0};
    uint32_t object = 0;

    init_dispatcher( &dispatcher, &shared );
    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 1 );
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 88, &first ),
        "ready regular wait failed\n" );
    ok( (lf_sync_wait_poll( &dispatcher, &first ) & 0xff) == LF_SYNC_WAIT_COMPLETE,
        "ready regular wait did not complete from PREPARED\n" );
    ok( !(uint32_t)shared.waiter_buckets[0].word_state && !shared.waiter_buckets[0].waiters[0],
        "ready regular wait was unnecessarily registered\n" );
    lf_sync_wait_end( &dispatcher, &first );

    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 88, &first ),
        "blocking regular wait failed\n" );
    ok( lf_sync_wait_poll( &dispatcher, &first ) ==
        lf_sync_wait_value( first.generation, LF_SYNC_WAITING, 0 ) &&
        shared.waiter_buckets[0].waiters[0] & (UINT64_C(1) << first.slot),
        "blocking regular wait was not armed after PREPARED\n" );
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 92, &second ),
        "second distributed wait failed\n" );
    ok( first.slot != second.slot && first.slot == ((88 >> 2) % dispatcher.wait_count) &&
        second.slot == ((92 >> 2) % dispatcher.wait_count),
        "wait allocation did not use owner-distributed starting slots\n" );
    lf_sync_wait_timeout( &dispatcher, &first );
    lf_sync_wait_timeout( &dispatcher, &second );
    lf_sync_wait_end( &dispatcher, &first );
    lf_sync_wait_end( &dispatcher, &second );
}

static void test_missed_dead_wait_reclamation(void)
{
    struct lf_sync_wait_ticket stale, replacement;
    struct lf_sync_dispatcher dispatcher;
    struct lf_sync_shared *shared;
    uint32_t index, owner = 84;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate missed-wait fixture\n" );
    if (!shared) return;
    lf_sync_init_shared( shared );
    ok( lf_sync_open_shared( &dispatcher, shared, NULL, NULL ),
        "failed to open missed-wait fixture\n" );
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_EVENT, 0, 0, 0, &index ),
        "failed to allocate missed-wait event\n" );
    ok( lf_sync_wait_begin( &dispatcher, &index, 1, 0, owner, &stale ),
        "failed to publish stale wait\n" );

    lf_sync_set_owner_alive( &dispatcher, owner, 0 );
    lf_sync_set_event( &dispatcher.arena, &dispatcher.objects[index], NULL );
    lf_sync_wake_object( &dispatcher, index );
    ok( !shared->waits[stale.slot].published &&
        !(shared->waits[stale.slot].lifetime & UINT32_MAX),
        "signal-side retry did not reclaim a dead-owner wait\n" );
    lf_sync_set_owner_alive( &dispatcher, owner, 1 );
    lf_sync_reset_event( &dispatcher.arena, &dispatcher.objects[index], NULL );
    ok( lf_sync_wait_begin( &dispatcher, &index, 1, 0, owner, &stale ),
        "failed to publish second stale wait\n" );

    /* Model a death scan which ran just before publication, followed by TID
     * reuse. The allocator must use the recorded epoch to reclaim only the
     * stale generation. */
    lf_sync_set_owner_alive( &dispatcher, owner, 0 );
    lf_sync_set_owner_alive( &dispatcher, owner, 1 );
    ok( lf_sync_wait_begin( &dispatcher, &index, 1, 0, owner, &replacement ),
        "allocator did not reclaim a missed dead wait\n" );
    ok( replacement.slot == stale.slot && replacement.generation != stale.generation,
        "missed dead wait slot was not reused with a new generation\n" );
    lf_sync_wait_timeout( &dispatcher, &replacement );
    lf_sync_wait_end( &dispatcher, &replacement );
    free( shared );
}

static void test_preparing_wait_reclamation(void)
{
    struct lf_sync_wait_ticket ticket;
    struct lf_sync_dispatcher dispatcher;
    struct lf_sync_shared *shared;
    uint32_t index, owner = 84;
    uint64_t preparing;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate preparing-wait fixture\n" );
    if (!shared) return;
    lf_sync_init_shared( shared );
    ok( lf_sync_open_shared( &dispatcher, shared, NULL, NULL ),
        "failed to open preparing-wait fixture\n" );
    dispatcher.wait_count = 1;
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_EVENT, 0, 0, 0, &index ),
        "failed to allocate preparing-wait event\n" );

    preparing = (UINT64_C(1) << 63) | (UINT64_C(7) << 32) | owner;
    shared->waits[0].lifetime = preparing;
    lf_sync_abandon_waits( &dispatcher, owner );
    ok( shared->waits[0].lifetime == preparing,
        "death cleanup reclaimed a live owner's unpublished wait claim\n" );
    lf_sync_set_owner_alive( &dispatcher, owner, 0 );
    lf_sync_abandon_waits( &dispatcher, owner );
    ok( shared->waits[0].lifetime == (UINT64_C(8) << 32) && !shared->waits[0].published,
        "death cleanup did not reclaim an unpublished wait claim\n" );

    preparing = (UINT64_C(1) << 63) | (UINT64_C(8) << 32) | owner;
    shared->waits[0].lifetime = preparing;
    ok( lf_sync_wait_begin( &dispatcher, &index, 1, 0, 88, &ticket ) &&
        ticket.slot == 0 && ticket.generation == 10,
        "allocator did not reclaim a missed unpublished wait claim\n" );
    lf_sync_wait_timeout( &dispatcher, &ticket );
    lf_sync_wait_end( &dispatcher, &ticket );

    preparing = (UINT64_C(1) << 63) | (UINT64_C(0x7fffffff) << 32) | owner;
    shared->waits[0].lifetime = preparing;
    lf_sync_abandon_waits( &dispatcher, owner );
    ok( shared->waits[0].lifetime == (UINT64_C(1) << 32),
        "unpublished wait generation did not wrap past zero\n" );
    free( shared );
}

static void test_parked_handshake(void)
{
    struct lf_sync_wait_ticket ticket;
    struct lf_sync_dispatcher dispatcher;
    struct shared_fixture shared = {0};
    uint32_t object = 0, sequence;

    init_dispatcher( &dispatcher, &shared );
    dispatcher.wake = count_wake;
    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );

    wake_calls = 0;
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 88, &ticket ),
        "spin-handoff wait registration failed\n" );
    lf_sync_set_event( &dispatcher.arena, &shared.objects[0], NULL );
    lf_sync_wake_object( &dispatcher, 0 );
    ok( !wake_calls && !shared.waits[ticket.slot].park_seq,
        "completion woke a waiter which had not parked\n" );
    lf_sync_wait_end( &dispatcher, &ticket );

    lf_sync_reset_event( &dispatcher.arena, &shared.objects[0], NULL );
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 88, &ticket ),
        "parked-handoff wait registration failed\n" );
    sequence = shared.waits[ticket.slot].park_seq;
    shared.waits[ticket.slot].parked = 1;
    lf_sync_set_event( &dispatcher.arena, &shared.objects[0], NULL );
    lf_sync_wake_object( &dispatcher, 0 );
    ok( wake_calls == 1 && shared.waits[ticket.slot].park_seq == sequence + 1 &&
        !shared.waits[ticket.slot].parked,
        "parked completion did not perform exactly one wake\n" );
    lf_sync_wait_end( &dispatcher, &ticket );
}

static void test_waiter_summary(void)
{
    struct
    {
        struct lf_sync_word words[66];
        struct lf_sync_mcas descs[8];
        struct lf_sync_object object;
        struct lf_sync_waiter_bucket waiter_bucket;
        struct lf_sync_wait waits[65];
    } shared = {0};
    struct lf_sync_wait_ticket tickets[65];
    struct lf_sync_dispatcher dispatcher = {0};
    uint32_t object = 0, i;

    dispatcher.arena.words = shared.words;
    dispatcher.arena.word_count = ARRAY_SIZE(shared.words);
    dispatcher.arena.descs = shared.descs;
    dispatcher.arena.desc_count = ARRAY_SIZE(shared.descs);
    dispatcher.objects = &shared.object;
    dispatcher.object_count = 1;
    dispatcher.waiter_buckets = &shared.waiter_bucket;
    dispatcher.waiter_bucket_count = 1;
    dispatcher.waits = shared.waits;
    dispatcher.wait_count = ARRAY_SIZE(shared.waits);
    dispatcher.status_word_base = 1;
    dispatcher.wake = futex_wake;
    lf_sync_init_event( &dispatcher.arena, &shared.object, 0, 1, 0 );

    for (i = 0; i < ARRAY_SIZE(tickets); ++i)
        ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 200 + i, &tickets[i] ),
            "summary waiter %u registration failed\n", i );
    ok( (uint32_t)shared.waiter_bucket.word_state == 3,
        "waiter summary %#x did not cover both populated words\n",
        (uint32_t)shared.waiter_bucket.word_state );

    lf_sync_set_event( &dispatcher.arena, &shared.object, NULL );
    lf_sync_wake_object( &dispatcher, 0 );
    for (i = 0; i < ARRAY_SIZE(tickets); ++i)
    {
        ok( (lf_sync_wait_poll( &dispatcher, &tickets[i] ) & 0xff) == LF_SYNC_WAIT_COMPLETE,
            "summary waiter %u was not completed\n", i );
        lf_sync_wait_end( &dispatcher, &tickets[i] );
    }
    ok( !(uint32_t)shared.waiter_bucket.word_state, "waiter summary was not cleared\n" );
    ok( shared.waiter_bucket.word_state >> 32 == 3,
        "waiter touched bitmap did not retain both populated words\n" );
}

struct waiter_summary_race
{
    struct lf_sync_dispatcher *dispatcher;
    struct lf_sync_wait_ticket old_ticket;
    struct lf_sync_wait_ticket new_ticket;
    pthread_barrier_t barrier;
    int registered;
};

static void *waiter_summary_unregister(void *arg)
{
    struct waiter_summary_race *race = arg;

    pthread_barrier_wait( &race->barrier );
    lf_sync_wait_end( race->dispatcher, &race->old_ticket );
    return NULL;
}

static void *waiter_summary_register(void *arg)
{
    struct waiter_summary_race *race = arg;
    uint32_t object = 0;

    pthread_barrier_wait( &race->barrier );
    race->registered = lf_sync_wait_begin( race->dispatcher, &object, 1, 0, 104,
                                            &race->new_ticket );
    return NULL;
}

static void test_waiter_summary_race(void)
{
    unsigned int i;

    for (i = 0; i < 1000; ++i)
    {
        struct waiter_summary_race race;
        struct lf_sync_dispatcher dispatcher;
        struct shared_fixture shared = {0};
        pthread_t registrar, unregistrar;
        uint32_t object = 0;

        init_dispatcher( &dispatcher, &shared );
        lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 1, 0 );
        memset( &race, 0, sizeof(race) );
        race.dispatcher = &dispatcher;
        ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 103, &race.old_ticket ),
            "summary-race initial registration failed at iteration %u\n", i );
        pthread_barrier_init( &race.barrier, NULL, 2 );
        pthread_create( &unregistrar, NULL, waiter_summary_unregister, &race );
        pthread_create( &registrar, NULL, waiter_summary_register, &race );
        pthread_join( unregistrar, NULL );
        pthread_join( registrar, NULL );
        pthread_barrier_destroy( &race.barrier );

        ok( race.registered, "summary-race registration failed at iteration %u\n", i );
        if (!race.registered) continue;
        ok( (uint32_t)shared.waiter_buckets[0].word_state && shared.waiter_buckets[0].waiters[0],
            "summary-race lost the registered waiter at iteration %u\n", i );
        lf_sync_set_event( &dispatcher.arena, &shared.objects[0], NULL );
        lf_sync_wake_object( &dispatcher, 0 );
        ok( (lf_sync_wait_poll( &dispatcher, &race.new_ticket ) & 0xff) == LF_SYNC_WAIT_COMPLETE,
            "summary-race waiter was not completed at iteration %u\n", i );
        lf_sync_wait_end( &dispatcher, &race.new_ticket );
    }
}

static void test_waiter_bucket_collision(void)
{
    struct lf_sync_wait_ticket ticket;
    struct lf_sync_dispatcher dispatcher;
    struct lf_sync_shared *shared;
    uint32_t first, second, object;

    shared = calloc( 1, sizeof(*shared) );
    ok( !!shared, "failed to allocate waiter bucket collision fixture\n" );
    if (!shared) return;
    lf_sync_init_shared( shared );
    ok( lf_sync_open_shared( &dispatcher, shared, NULL, NULL ),
        "failed to open waiter bucket collision fixture\n" );
    dispatcher.waiter_bucket_count = 1;

    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_EVENT, 0, 0, 0, &first ),
        "failed to allocate first colliding event\n" );
    ok( lf_sync_alloc_object( &dispatcher, LF_SYNC_EVENT, 0, 0, 0, &second ),
        "failed to allocate second colliding event\n" );
    object = second;
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 108, &ticket ),
        "failed to register colliding waiter\n" );

    lf_sync_set_event( &dispatcher.arena, &dispatcher.objects[second], NULL );
    lf_sync_wake_object( &dispatcher, first );
    ok( lf_sync_wait_poll( &dispatcher, &ticket ) ==
        lf_sync_wait_value( ticket.generation, LF_SYNC_WAITING, 0 ),
        "hash collision retried an unrelated waiter\n" );
    ok( lf_sync_free_object( &dispatcher, first ),
        "unrelated hash collision prevented object reclamation\n" );

    lf_sync_wake_object( &dispatcher, second );
    ok( (lf_sync_wait_poll( &dispatcher, &ticket ) & 0xff) == LF_SYNC_WAIT_COMPLETE,
        "colliding waiter was not completed by its own object\n" );
    lf_sync_wait_end( &dispatcher, &ticket );
    ok( lf_sync_free_object( &dispatcher, second ),
        "completed colliding wait prevented object reclamation\n" );
    free( shared );
}

static void test_registered_mutex_limit(void)
{
    struct lf_sync_dispatcher dispatcher;
    struct shared_fixture shared = {0};
    struct lf_sync_wait_ticket ticket;
    uint32_t object = 0;
    uint64_t result;

    init_dispatcher( &dispatcher, &shared );
    lf_sync_init_mutex( &dispatcher.arena, &shared.objects[0], 0, 88, 0x7fffffff );
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 88, &ticket ),
        "mutex-limit wait registration failed\n" );
    result = lf_sync_wait_poll( &dispatcher, &ticket );
    ok( (result & 0xff) == LF_SYNC_WAIT_LIMIT_EXCEEDED,
        "registered mutex-limit wait did not terminate\n" );
    lf_sync_wait_end( &dispatcher, &ticket );
}

static void test_atomic_signal_and_wait(void)
{
    struct lf_sync_dispatcher dispatcher;
    struct shared_fixture shared = {0};
    struct lf_sync_wait_ticket ticket;
    uint64_t status;

    init_dispatcher( &dispatcher, &shared );
    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
    lf_sync_init_event( &dispatcher.arena, &shared.objects[1], 1, 1, 1 );
    ok( lf_sync_signal_and_wait_begin( &dispatcher, 0, 1, 91, ~0u, &ticket ) == LF_SYNC_SUCCESS,
        "event signal-and-wait transaction failed\n" );
    status = lf_sync_wait_poll( &dispatcher, &ticket );
    ok( (status & 0xff) == LF_SYNC_WAIT_COMPLETE, "ready signal-and-wait did not complete atomically\n" );
    ok( lf_sync_load( &dispatcher.arena, 0 ) == 1, "signal event was not set\n" );
    ok( lf_sync_load( &dispatcher.arena, 1 ) == 1, "manual wait event was consumed\n" );
    lf_sync_wait_end( &dispatcher, &ticket );

    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
    ok( lf_sync_signal_and_wait_begin( &dispatcher, 0, 0, 91, ~0u, &ticket ) == LF_SYNC_SUCCESS,
        "same-event signal-and-wait transaction failed\n" );
    status = lf_sync_wait_poll( &dispatcher, &ticket );
    ok( (status & 0xff) == LF_SYNC_WAIT_COMPLETE, "same-event signal-and-wait did not complete\n" );
    ok( lf_sync_load( &dispatcher.arena, 0 ) == 0, "same auto-event signal-and-wait left it signaled\n" );
    lf_sync_wait_end( &dispatcher, &ticket );

    lf_sync_init_semaphore( &dispatcher.arena, &shared.objects[0], 0, 0, 1 );
    lf_sync_init_semaphore( &dispatcher.arena, &shared.objects[1], 1, 1, 1 );
    ok( lf_sync_signal_and_wait_begin( &dispatcher, 0, 1, 91, ~0u, &ticket ) == LF_SYNC_SUCCESS,
        "semaphore signal-and-wait transaction failed\n" );
    ok( (lf_sync_wait_poll( &dispatcher, &ticket ) & 0xff) == LF_SYNC_WAIT_COMPLETE,
        "semaphore signal-and-wait did not complete\n" );
    ok( lf_sync_load( &dispatcher.arena, 0 ) == 1 && lf_sync_load( &dispatcher.arena, 1 ) == 0,
        "semaphore signal-and-wait published partial state\n" );
    lf_sync_wait_end( &dispatcher, &ticket );
    ok( lf_sync_signal_and_wait_begin( &dispatcher, 0, 1, 91, ~0u, &ticket ) == LF_SYNC_LIMIT_EXCEEDED,
        "full signal semaphore did not reject the whole transaction\n" );
    ok( lf_sync_load( &dispatcher.arena, 0 ) == 1 && lf_sync_load( &dispatcher.arena, 1 ) == 0,
        "failed signal-and-wait changed semaphore state\n" );

    lf_sync_init_mutex( &dispatcher.arena, &shared.objects[0], 0, 91, 1 );
    ok( lf_sync_signal_and_wait_begin( &dispatcher, 0, 0, 91, ~0u, &ticket ) == LF_SYNC_SUCCESS,
        "same-mutex signal-and-wait transaction failed\n" );
    ok( (lf_sync_wait_poll( &dispatcher, &ticket ) & 0xff) == LF_SYNC_WAIT_COMPLETE,
        "same-mutex signal-and-wait did not complete\n" );
    ok( lf_sync_load( &dispatcher.arena, 0 ) == (UINT64_C(1) << 32 | 91),
        "same-mutex signal-and-wait changed ownership\n" );
    lf_sync_wait_end( &dispatcher, &ticket );

    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
    lf_sync_init_event( &dispatcher.arena, &shared.objects[1], 1, 0, 0 );
    ok( lf_sync_signal_and_wait_begin( &dispatcher, 0, 1, 91, ~0u, &ticket ) == LF_SYNC_SUCCESS,
        "blocking signal-and-wait transaction failed\n" );
    ok( (lf_sync_wait_poll( &dispatcher, &ticket ) & 0xff) == LF_SYNC_WAITING,
        "blocking signal-and-wait was not atomically armed\n" );
    ok( lf_sync_load( &dispatcher.arena, 0 ) == 1, "blocking signal-and-wait did not signal\n" );
    lf_sync_set_event( &dispatcher.arena, &shared.objects[1], NULL );
    lf_sync_wake_object( &dispatcher, 1 );
    ok( (lf_sync_wait_poll( &dispatcher, &ticket ) & 0xff) == LF_SYNC_WAIT_COMPLETE,
        "armed signal-and-wait did not complete later\n" );
    lf_sync_wait_end( &dispatcher, &ticket );
}

static void test_pulse_snapshot(void)
{
    struct lf_sync_wait_ticket first, second;
    struct lf_sync_dispatcher dispatcher;
    struct shared_fixture shared = {0};
    uint32_t objects[2] = {0, 1}, previous;
    uint64_t first_result, second_result;

    init_dispatcher( &dispatcher, &shared );
    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
    ok( lf_sync_wait_begin( &dispatcher, objects, 1, 0, 90, &first ), "first pulse wait failed\n" );
    ok( lf_sync_wait_begin( &dispatcher, objects, 1, 0, 91, &second ), "second pulse wait failed\n" );
    ok( lf_sync_pulse_event( &dispatcher, 0, &previous ) == LF_SYNC_SUCCESS && !previous,
        "auto-event pulse failed\n" );
    first_result = lf_sync_wait_poll( &dispatcher, &first );
    second_result = lf_sync_wait_poll( &dispatcher, &second );
    ok( ((first_result & 0xff) == LF_SYNC_WAIT_COMPLETE) +
        ((second_result & 0xff) == LF_SYNC_WAIT_COMPLETE) == 1,
        "auto-event pulse did not complete exactly one snapshot waiter\n" );
    ok( !lf_sync_load( &dispatcher.arena, 0 ), "pulse left auto event signaled\n" );
    lf_sync_wait_timeout( &dispatcher,
        (first_result & 0xff) == LF_SYNC_WAITING ? &first : &second );
    lf_sync_wait_end( &dispatcher, &first );
    lf_sync_wait_end( &dispatcher, &second );

    memset( &shared, 0, sizeof(shared) );
    init_dispatcher( &dispatcher, &shared );
    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 1, 0 );
    ok( lf_sync_wait_begin( &dispatcher, objects, 1, 0, 90, &first ), "first manual pulse wait failed\n" );
    ok( lf_sync_wait_begin( &dispatcher, objects, 1, 0, 91, &second ), "second manual pulse wait failed\n" );
    lf_sync_pulse_event( &dispatcher, 0, NULL );
    ok( (lf_sync_wait_poll( &dispatcher, &first ) & 0xff) == LF_SYNC_WAIT_COMPLETE &&
        (lf_sync_wait_poll( &dispatcher, &second ) & 0xff) == LF_SYNC_WAIT_COMPLETE,
        "manual-event pulse did not complete every snapshot waiter\n" );
    lf_sync_wait_end( &dispatcher, &first );
    lf_sync_wait_end( &dispatcher, &second );

    memset( &shared, 0, sizeof(shared) );
    init_dispatcher( &dispatcher, &shared );
    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
    lf_sync_init_semaphore( &dispatcher.arena, &shared.objects[1], 1, 1, 1 );
    ok( lf_sync_wait_begin( &dispatcher, objects, 2, 1, 90, &first ), "pulse WaitAll failed\n" );
    lf_sync_pulse_event( &dispatcher, 0, NULL );
    ok( (lf_sync_wait_poll( &dispatcher, &first ) & 0xff) == LF_SYNC_WAIT_COMPLETE,
        "pulse did not atomically complete WaitAll\n" );
    ok( !lf_sync_load( &dispatcher.arena, 1 ), "pulse WaitAll did not consume semaphore\n" );
    lf_sync_wait_end( &dispatcher, &first );
}

static void test_dead_waiter_reclamation(void)
{
    struct lf_sync_wait_ticket dead, replacement;
    struct lf_sync_dispatcher dispatcher;
    struct shared_fixture shared = {0};
    uint32_t object = 0;

    init_dispatcher( &dispatcher, &shared );
    dispatcher.wait_count = 1;
    lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 99, &dead ),
        "dead waiter registration failed\n" );
    lf_sync_abandon_waits( &dispatcher, 99 );
    ok( !shared.waits[dead.slot].published && !(shared.waits[dead.slot].lifetime & 0xffffffff),
        "dead waiter registration reference leaked\n" );
    ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 100, &replacement ),
        "reclaimed waiter slot could not be reused\n" );
    ok( replacement.slot == dead.slot && replacement.generation != dead.generation,
        "reclaimed waiter slot did not advance generation\n" );
    lf_sync_wait_timeout( &dispatcher, &replacement );
    lf_sync_wait_end( &dispatcher, &replacement );
}

struct pulse_race_context
{
    struct lf_sync_dispatcher *dispatcher;
    struct lf_sync_wait_ticket ticket;
    pthread_barrier_t barrier;
    int registered;
};

static void *pulse_race_register(void *arg)
{
    struct pulse_race_context *context = arg;
    uint32_t object = 0;

    pthread_barrier_wait( &context->barrier );
    context->registered = lf_sync_wait_begin( context->dispatcher, &object, 1, 0, 101,
                                               &context->ticket );
    return NULL;
}

static void *pulse_race_pulse(void *arg)
{
    struct pulse_race_context *context = arg;

    pthread_barrier_wait( &context->barrier );
    lf_sync_pulse_event( context->dispatcher, 0, NULL );
    return NULL;
}

static void test_pulse_registration_race(void)
{
    unsigned int i;

    for (i = 0; i < 1000; ++i)
    {
        struct pulse_race_context context;
        struct lf_sync_dispatcher dispatcher;
        struct shared_fixture shared = {0};
        pthread_t registrar, pulser;
        uint64_t result;

        init_dispatcher( &dispatcher, &shared );
        lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
        memset( &context, 0, sizeof(context) );
        context.dispatcher = &dispatcher;
        pthread_barrier_init( &context.barrier, NULL, 2 );
        pthread_create( &registrar, NULL, pulse_race_register, &context );
        pthread_create( &pulser, NULL, pulse_race_pulse, &context );
        pthread_join( registrar, NULL );
        pthread_join( pulser, NULL );
        pthread_barrier_destroy( &context.barrier );

        ok( context.registered, "pulse-race waiter registration failed at iteration %u\n", i );
        if (!context.registered) continue;
        result = lf_sync_wait_poll( &dispatcher, &context.ticket );
        ok( (result & 0xff) == LF_SYNC_WAITING || (result & 0xff) == LF_SYNC_WAIT_COMPLETE,
            "pulse-registration race produced invalid status %#llx at iteration %u\n",
            (unsigned long long)result, i );
        if ((result & 0xff) == LF_SYNC_WAITING) lf_sync_wait_timeout( &dispatcher, &context.ticket );
        lf_sync_wait_end( &dispatcher, &context.ticket );
        ok( !(shared.waits[context.ticket.slot].lifetime & 0xffffffff),
            "pulse-registration race leaked waiter at iteration %u\n", i );
    }
}

struct abandon_race_context
{
    struct lf_sync_dispatcher *dispatcher;
    struct lf_sync_wait_ticket *ticket;
    pthread_barrier_t barrier;
};

static void *abandon_race_end(void *arg)
{
    struct abandon_race_context *context = arg;

    pthread_barrier_wait( &context->barrier );
    lf_sync_wait_end( context->dispatcher, context->ticket );
    return NULL;
}

static void *abandon_race_cleanup(void *arg)
{
    struct abandon_race_context *context = arg;

    pthread_barrier_wait( &context->barrier );
    lf_sync_abandon_waits( context->dispatcher, 102 );
    return NULL;
}

static void test_wait_end_abandon_race(void)
{
    unsigned int i;

    for (i = 0; i < 1000; ++i)
    {
        struct abandon_race_context context;
        struct lf_sync_wait_ticket ticket;
        struct lf_sync_dispatcher dispatcher;
        struct shared_fixture shared = {0};
        pthread_t ender, cleaner;
        uint32_t object = 0;

        init_dispatcher( &dispatcher, &shared );
        lf_sync_init_event( &dispatcher.arena, &shared.objects[0], 0, 0, 0 );
        ok( lf_sync_wait_begin( &dispatcher, &object, 1, 0, 102, &ticket ),
            "abandon-race registration failed at iteration %u\n", i );
        context.dispatcher = &dispatcher;
        context.ticket = &ticket;
        pthread_barrier_init( &context.barrier, NULL, 2 );
        pthread_create( &ender, NULL, abandon_race_end, &context );
        pthread_create( &cleaner, NULL, abandon_race_cleanup, &context );
        pthread_join( ender, NULL );
        pthread_join( cleaner, NULL );
        pthread_barrier_destroy( &context.barrier );
        ok( !(shared.waits[ticket.slot].lifetime & 0xffffffff),
            "wait-end/abandon race leaked or underflowed at iteration %u\n", i );
    }
}

#endif

int main(void)
{
    ok( lf_sync_is_lock_free(), "64-bit atomics are not lock-free on this platform\n" );
    test_commit_and_abort();
    test_invalid_mcas_entries();
    test_competing_wait_all();
    test_large_unordered_wait_all();
    test_descriptor_reclamation_stress();
    test_dead_descriptor_reclamation();
    test_nt_object_transitions();
    test_indexed_mutex_abandonment();
    test_owner_death_transaction_ordering();
    test_mcas_cleanup_waits_for_active_helpers();
    test_active_mcas_resolves_decided_dependency();
    test_object_reuse();
    test_shared_initialization_does_not_touch_payload();
    test_shared_cacheline_layout();
    test_open_shared_initializes_dispatcher();
    test_packed_object_boundary();
    test_lease_state_machine();
    test_completion_timeout_race();
#ifdef __linux__
    test_shared_parking();
    test_registered_timeout();
    test_prepared_regular_wait();
    test_missed_dead_wait_reclamation();
    test_preparing_wait_reclamation();
    test_parked_handshake();
    test_waiter_summary();
    test_waiter_summary_race();
    test_waiter_bucket_collision();
    test_registered_mutex_limit();
    test_atomic_signal_and_wait();
    test_pulse_snapshot();
    test_dead_waiter_reclamation();
    test_pulse_registration_race();
    test_wait_end_abandon_race();
#endif
    if (failures) fprintf( stderr, "%d lock-free synchronization tests failed\n", failures );
    return failures != 0;
}
