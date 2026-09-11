/*
 * Client surface image memory admission
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include <assert.h>
#include "ntstatus.h"
#include "client_surface.h"
#include "ntuser_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);
WINE_DECLARE_DEBUG_CHANNEL(csperf);

/* Explicitly allocated client-surface images share a process budget. Native
 * WSI allocations are owned by the host driver and are not estimated here.
 * Retired storage stays charged until its real completion permits release. */
#define CLIENT_SURFACE_IMAGE_MEMORY_LIMIT ((UINT64)1024 * 1024 * 1024)
static pthread_mutex_t image_memory_lock = PTHREAD_MUTEX_INITIALIZER;
static UINT64 image_memory[CLIENT_SURFACE_MEMORY_CLASS_COUNT], image_memory_total;
static UINT64 image_metadata_memory;

enum memory_account_kind { MEMORY_OWNER, MEMORY_NATIVE_DOMAIN };

struct client_surface_memory_account
{
    struct list entry;
    enum memory_account_kind kind;
    UINT64 key, id, limit;
    UINT64 used[CLIENT_SURFACE_MEMORY_CLASS_COUNT + 1];
    unsigned int refs;
    HANDLE process;
};

static struct list memory_accounts = LIST_INIT( memory_accounts );
static UINT64 memory_account_id;

/* Unused capacity for another purpose is not available for image growth.
 * These are protected portions of the same process limit, not extra budgets
 * or per-purpose maxima. Usage above a floor competes for the shared remainder.
 * Metadata remains part of STAGING accounting, but image readback/upload must
 * not consume the capacity needed to prepare transfers and their retirement. */
static const UINT64 image_memory_floor[] =
{
    [CLIENT_SURFACE_MEMORY_SOURCE] = CLIENT_SURFACE_IMAGE_MEMORY_LIMIT / 8,
    [CLIENT_SURFACE_MEMORY_STAGING] = CLIENT_SURFACE_IMAGE_MEMORY_LIMIT / 16,
    [CLIENT_SURFACE_MEMORY_OUTPUT] = CLIENT_SURFACE_IMAGE_MEMORY_LIMIT / 8,
    [CLIENT_SURFACE_MEMORY_CLASS_COUNT] = CLIENT_SURFACE_IMAGE_MEMORY_LIMIT / 1024,
};

static UINT64 memory_available( const UINT64 *used, UINT64 limit, unsigned int purpose )
{
    UINT64 committed = 0, floor;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(image_memory_floor); ++i)
        committed += max( used[i], image_memory_floor[i] * limit / CLIENT_SURFACE_IMAGE_MEMORY_LIMIT );
    assert( committed <= limit );
    floor = image_memory_floor[purpose] * limit / CLIENT_SURFACE_IMAGE_MEMORY_LIMIT;
    return limit - committed + max( used[purpose], floor ) - used[purpose];
}

static UINT64 image_memory_available( enum client_surface_memory_class type, BOOL metadata )
{
    UINT64 used[] = {image_memory[CLIENT_SURFACE_MEMORY_SOURCE],
                     image_memory[CLIENT_SURFACE_MEMORY_STAGING] - image_metadata_memory,
                     image_memory[CLIENT_SURFACE_MEMORY_OUTPUT], image_metadata_memory};

    return memory_available( used, CLIENT_SURFACE_IMAGE_MEMORY_LIMIT,
                              metadata ? CLIENT_SURFACE_MEMORY_CLASS_COUNT : type );
}

static UINT64 account_memory_available( struct client_surface_memory_account *account, unsigned int purpose )
{
    return account ? memory_available( account->used, account->limit, purpose ) : CLIENT_SURFACE_IMAGE_MEMORY_LIMIT;
}

static void trace_memory_account( const char *event, const struct client_surface_memory_account *account )
{
    LARGE_INTEGER counter;

    if (!TRACE_ON(csperf) || !account) return;
    NtQueryPerformanceCounter( &counter, NULL );
    TRACE_(csperf)( "ticks=%llu event=%s account=%llu kind=%u key=%llu limit=%llu references=%u "
                   "source=%llu staging=%llu output=%llu metadata=%llu\n",
                   (unsigned long long)counter.QuadPart, event, (unsigned long long)account->id, account->kind,
                   (unsigned long long)account->key, (unsigned long long)account->limit, account->refs,
                   (unsigned long long)account->used[CLIENT_SURFACE_MEMORY_SOURCE],
                   (unsigned long long)account->used[CLIENT_SURFACE_MEMORY_STAGING],
                   (unsigned long long)account->used[CLIENT_SURFACE_MEMORY_OUTPUT],
                   (unsigned long long)account->used[CLIENT_SURFACE_MEMORY_CLASS_COUNT] );
}

static void trace_client_surface_memory( const char *event, enum client_surface_memory_class type,
                                         UINT64 bytes, BOOL accepted, BOOL metadata,
                                         const struct client_surface_memory_scope *scope )
{
    struct client_surface_memory_account *owner = scope ? scope->owner : NULL, *domain = scope ? scope->domain : NULL;
    LARGE_INTEGER counter;

    if (!TRACE_ON(csperf)) return;
    NtQueryPerformanceCounter( &counter, NULL );
    TRACE_(csperf)( "ticks=%llu event=%s class=%u bytes=%llu accepted=%u source=%llu staging=%llu output=%llu total=%llu "
                   "metadata=%llu purpose=%u source_available=%llu staging_available=%llu output_available=%llu metadata_available=%llu "
                   "owner=%llu domain=%llu owner_source_available=%llu domain_source_available=%llu\n",
                   (unsigned long long)counter.QuadPart, event, type, (unsigned long long)bytes, accepted,
                   (unsigned long long)image_memory[CLIENT_SURFACE_MEMORY_SOURCE],
                   (unsigned long long)image_memory[CLIENT_SURFACE_MEMORY_STAGING],
                   (unsigned long long)image_memory[CLIENT_SURFACE_MEMORY_OUTPUT],
                   (unsigned long long)image_memory_total, (unsigned long long)image_metadata_memory,
                   metadata ? CLIENT_SURFACE_MEMORY_CLASS_COUNT : type,
                   (unsigned long long)image_memory_available( CLIENT_SURFACE_MEMORY_SOURCE, FALSE ),
                   (unsigned long long)image_memory_available( CLIENT_SURFACE_MEMORY_STAGING, FALSE ),
                   (unsigned long long)image_memory_available( CLIENT_SURFACE_MEMORY_OUTPUT, FALSE ),
                   (unsigned long long)image_memory_available( CLIENT_SURFACE_MEMORY_STAGING, TRUE ),
                   (unsigned long long)(owner ? owner->id : 0), (unsigned long long)(domain ? domain->id : 0),
                   (unsigned long long)account_memory_available( owner, CLIENT_SURFACE_MEMORY_SOURCE ),
                   (unsigned long long)account_memory_available( domain, CLIENT_SURFACE_MEMORY_SOURCE ) );
}

static BOOL reserve_image_memory( struct client_surface_memory_scope *scope,
                                  enum client_surface_memory_class type, UINT64 bytes, BOOL metadata )
{
    unsigned int purpose = metadata ? CLIENT_SURFACE_MEMORY_CLASS_COUNT : type;
    BOOL ret;

    assert( type < CLIENT_SURFACE_MEMORY_CLASS_COUNT );
    pthread_mutex_lock( &image_memory_lock );
    ret = bytes <= image_memory_available( type, metadata );
    if (scope)
        ret = ret && bytes <= account_memory_available( scope->owner, purpose ) &&
                     bytes <= account_memory_available( scope->domain, purpose );
    if (ret)
    {
        image_memory[type] += bytes;
        image_memory_total += bytes;
        if (metadata) image_metadata_memory += bytes;
        if (scope)
        {
            scope->used[purpose] += bytes;
            if (scope->owner) scope->owner->used[purpose] += bytes;
            if (scope->domain) scope->domain->used[purpose] += bytes;
        }
    }
    TRACE( "image reservation class %u bytes %s accepted %u source %s staging %s output %s\n",
           type, wine_dbgstr_longlong( bytes ), ret,
           wine_dbgstr_longlong( image_memory[CLIENT_SURFACE_MEMORY_SOURCE] ),
           wine_dbgstr_longlong( image_memory[CLIENT_SURFACE_MEMORY_STAGING] ),
           wine_dbgstr_longlong( image_memory[CLIENT_SURFACE_MEMORY_OUTPUT] ) );
    trace_client_surface_memory( "image_reserve", type, bytes, ret, metadata, scope );
    pthread_mutex_unlock( &image_memory_lock );
    return ret;
}

static void release_image_memory( struct client_surface_memory_scope *scope,
                                  enum client_surface_memory_class type, UINT64 bytes, BOOL metadata )
{
    unsigned int purpose = metadata ? CLIENT_SURFACE_MEMORY_CLASS_COUNT : type;
    assert( type < CLIENT_SURFACE_MEMORY_CLASS_COUNT );
    pthread_mutex_lock( &image_memory_lock );
    if (scope)
    {
        assert( scope->used[purpose] >= bytes );
        scope->used[purpose] -= bytes;
        if (scope->owner)
        {
            assert( scope->owner->used[purpose] >= bytes );
            scope->owner->used[purpose] -= bytes;
        }
        if (scope->domain)
        {
            assert( scope->domain->used[purpose] >= bytes );
            scope->domain->used[purpose] -= bytes;
        }
    }
    assert( image_memory[type] >= bytes && image_memory_total >= bytes );
    if (metadata)
    {
        assert( image_metadata_memory >= bytes );
        image_metadata_memory -= bytes;
    }
    else if (type == CLIENT_SURFACE_MEMORY_STAGING)
        assert( image_memory[type] - image_metadata_memory >= bytes );
    image_memory[type] -= bytes;
    image_memory_total -= bytes;
    trace_client_surface_memory( "image_release", type, bytes, TRUE, metadata, scope );
    pthread_mutex_unlock( &image_memory_lock );
}

BOOL client_surface_reserve_memory( enum client_surface_memory_class type, UINT64 bytes )
{
    return reserve_image_memory( NULL, type, bytes, FALSE );
}

void client_surface_release_memory( enum client_surface_memory_class type, UINT64 bytes )
{
    release_image_memory( NULL, type, bytes, FALSE );
}

BOOL client_surface_reserve_metadata_memory( UINT64 bytes )
{
    return reserve_image_memory( NULL, CLIENT_SURFACE_MEMORY_STAGING, bytes, TRUE );
}

void client_surface_release_metadata_memory( UINT64 bytes )
{
    release_image_memory( NULL, CLIENT_SURFACE_MEMORY_STAGING, bytes, TRUE );
}

BOOL client_surface_reserve_scoped_memory( struct client_surface_memory_scope *scope,
                                           enum client_surface_memory_class type, UINT64 bytes )
{
    assert( scope->domain );
    return reserve_image_memory( scope, type, bytes, FALSE );
}

void client_surface_release_scoped_memory( struct client_surface_memory_scope *scope,
                                           enum client_surface_memory_class type, UINT64 bytes )
{
    assert( !bytes || scope->domain );
    release_image_memory( scope, type, bytes, FALSE );
}

BOOL client_surface_reserve_scoped_metadata( struct client_surface_memory_scope *scope, UINT64 bytes )
{
    assert( !scope || scope->domain );
    return reserve_image_memory( scope, CLIENT_SURFACE_MEMORY_STAGING, bytes, TRUE );
}

void client_surface_release_scoped_metadata( struct client_surface_memory_scope *scope, UINT64 bytes )
{
    assert( !scope || !bytes || scope->domain );
    release_image_memory( scope, CLIENT_SURFACE_MEMORY_STAGING, bytes, TRUE );
}

static struct client_surface_memory_account *find_memory_account( enum memory_account_kind kind, UINT64 key )
{
    struct client_surface_memory_account *account;

    LIST_FOR_EACH_ENTRY( account, &memory_accounts, struct client_surface_memory_account, entry )
        if (account->kind == kind && account->key == key)
        {
            ++account->refs;
            return account;
        }
    return NULL;
}

static struct client_surface_memory_account *acquire_memory_account( enum memory_account_kind kind, UINT64 key )
{
    struct client_surface_memory_account *account, *next;
    OBJECT_ATTRIBUTES attr = {sizeof(attr)};
    CLIENT_ID client_id = {.UniqueProcess = ULongToHandle( key )};
    HANDLE process = NULL;
    NTSTATUS status;

    pthread_mutex_lock( &image_memory_lock );
    account = find_memory_account( kind, key );
    pthread_mutex_unlock( &image_memory_lock );
    if (account) return account;

    /* The server does not recycle a process id until its object is destroyed.
     * Pin that object even after exit, for as long as its images are retained. */
    if (kind == MEMORY_OWNER && (status = NtOpenProcess( &process, SYNCHRONIZE, &attr, &client_id )))
    {
        WARN( "failed to retain image owner %s, status %#x\n", wine_dbgstr_longlong( key ), (unsigned int)status );
        return NULL;
    }
    if (!client_surface_reserve_metadata_memory( sizeof(*next) )) goto failed;
    if (!(next = calloc( 1, sizeof(*next) )))
    {
        client_surface_release_metadata_memory( sizeof(*next) );
        goto failed;
    }
    next->kind = kind;
    next->key = key;
    next->process = process;
    next->refs = 1;
    /* One owner cannot fill its native domain; one native domain cannot fill
     * the process. Every level retains the same proportional purpose floors. */
    next->limit = CLIENT_SURFACE_IMAGE_MEMORY_LIMIT * (kind == MEMORY_OWNER ? 2 : 3) / 4;
    pthread_mutex_lock( &image_memory_lock );
    if (!(account = find_memory_account( kind, key )) && memory_account_id != ~(UINT64)0)
    {
        account = next;
        account->id = ++memory_account_id;
        list_add_tail( &memory_accounts, &account->entry );
        trace_memory_account( "memory_account_create", account );
    }
    pthread_mutex_unlock( &image_memory_lock );
    if (account == next) return account;
    free( next );
    client_surface_release_metadata_memory( sizeof(*next) );
    if (process) NtClose( process );
    return account;

failed:
    if (process) NtClose( process );
    return NULL;
}

static void release_memory_account( struct client_surface_memory_account *account )
{
    unsigned int i;

    if (!account) return;
    pthread_mutex_lock( &image_memory_lock );
    if (--account->refs)
    {
        pthread_mutex_unlock( &image_memory_lock );
        return;
    }
    for (i = 0; i < ARRAY_SIZE(account->used); ++i) assert( !account->used[i] );
    list_remove( &account->entry );
    trace_memory_account( "memory_account_destroy", account );
    pthread_mutex_unlock( &image_memory_lock );
    if (account->process) NtClose( account->process );
    free( account );
    client_surface_release_metadata_memory( sizeof(*account) );
}

BOOL client_surface_memory_scope_init( struct client_surface_memory_scope *scope, HWND hwnd, UINT64 domain )
{
    DWORD process;
    HWND root;

    assert( !scope->owner && !scope->domain );
    if (hwnd)
    {
        if (!(root = NtUserGetAncestor( hwnd, GA_ROOT )) || !get_window_thread( root, &process ))
        {
            WARN( "failed to find image owner for hwnd %p root %p\n", hwnd, root );
            return FALSE;
        }
        if (!(scope->owner = acquire_memory_account( MEMORY_OWNER, process ))) return FALSE;
    }
    if ((scope->domain = acquire_memory_account( MEMORY_NATIVE_DOMAIN, domain ))) return TRUE;
    release_memory_account( scope->owner );
    scope->owner = NULL;
    return FALSE;
}

void client_surface_memory_scope_copy( struct client_surface_memory_scope *dst,
                                      const struct client_surface_memory_scope *src, BOOL include_owner )
{
    unsigned int i;

    assert( !dst->owner && !dst->domain );
    for (i = 0; i < ARRAY_SIZE(dst->used); ++i) assert( !dst->used[i] );
    pthread_mutex_lock( &image_memory_lock );
    if (include_owner && (dst->owner = src->owner)) ++dst->owner->refs;
    if ((dst->domain = src->domain)) ++dst->domain->refs;
    pthread_mutex_unlock( &image_memory_lock );
}

void client_surface_memory_scope_destroy( struct client_surface_memory_scope *scope )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(scope->used); ++i) assert( !scope->used[i] );
    release_memory_account( scope->owner );
    release_memory_account( scope->domain );
    scope->owner = scope->domain = NULL;
}

/* Capacity and accounting belong to the allocation, independently of the
 * current array consumer and its logical element count. */
struct client_surface_memory_array
{
    struct client_surface_memory_scope memory;
    SIZE_T capacity, bytes;
};

void *client_surface_alloc_owned_array( const struct client_surface_memory_scope *owner,
                                       SIZE_T count, SIZE_T size )
{
    struct client_surface_memory_scope memory = {0};
    struct client_surface_memory_array *array;
    SIZE_T bytes;

    if (!count || !size || count > (~(SIZE_T)0 - sizeof(*array)) / size) return NULL;
    bytes = sizeof(*array) + count * size;
    client_surface_memory_scope_copy( &memory, owner, TRUE );
    if (!(array = client_surface_alloc_scoped_metadata( &memory, 1, bytes )))
    {
        client_surface_memory_scope_destroy( &memory );
        return NULL;
    }
    array->memory = memory;
    array->capacity = count;
    array->bytes = bytes;
    return array + 1;
}

void client_surface_free_owned_array( void *data )
{
    struct client_surface_memory_array *array;

    if (!data) return;
    array = (struct client_surface_memory_array *)data - 1;
    client_surface_free_owned_metadata( &array->memory, array, array->bytes );
}
