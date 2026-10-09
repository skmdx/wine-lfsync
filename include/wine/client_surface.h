/*
 * Client surface shared completion definitions
 *
 * Copyright 2026 Wine contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_CLIENT_SURFACE_H
#define __WINE_CLIENT_SURFACE_H

#include "windef.h"

enum client_surface_presentation_mode
{
    CLIENT_SURFACE_PRESENTATION_INVALID,
    CLIENT_SURFACE_PRESENTATION_DIRECT,
    CLIENT_SURFACE_PRESENTATION_STAGED,
    CLIENT_SURFACE_PRESENTATION_COMPOSITED,
};

/* Private native storage and the completed-frame ring have separate capacities.
 * A power-of-two descriptor ring keeps indexing valid across UINT64 wrap. */
#define CLIENT_SURFACE_SOURCE_FRAME_COUNT 3
#define CLIENT_SURFACE_HANDOFF_RING_SIZE 4
#define CLIENT_SURFACE_HANDOFF_CHANNELS 512
#define CLIENT_SURFACE_HANDOFF_BITMAP_WORDS (CLIENT_SURFACE_HANDOFF_CHANNELS / 64)
#define CLIENT_SURFACE_HANDOFF_MAX_POOLS_PER_CONSUMER 512
#define CLIENT_SURFACE_HANDOFF_MAGIC ((UINT64)0x57435348414e444full)
#define CLIENT_SURFACE_HANDOFF_VERSION 14

/* Each source can publish once before its read receipt permits reuse. */
C_ASSERT( CLIENT_SURFACE_HANDOFF_RING_SIZE >= CLIENT_SURFACE_SOURCE_FRAME_COUNT );

#define CLIENT_SURFACE_HANDOFF_NATIVE_X11 0x0001
#define CLIENT_SURFACE_HANDOFF_FULL_DAMAGE 0x0002
#define CLIENT_SURFACE_HANDOFF_CLIPPED 0x0004
#define CLIENT_SURFACE_HANDOFF_XFIXES_CLIP 0x0008
#define CLIENT_SURFACE_HANDOFF_PIXMAP_CLIP 0x0010
/* Producer-owned snapshot: finish all reads before returning its source slot. */
#define CLIENT_SURFACE_HANDOFF_COPY_SOURCE 0x0020
#define CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER 0x0001
#define CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER 0x0002

/* Only completed immutable image metadata enters the ring. The producer owns
 * a slot until producer_sequence publishes it; consumer_sequence returns it
 * once the owner has accepted its metadata. Source storage has an independent
 * receipt, returned only after all reads of that publication finish.
 * Placement and publication context
 * belong to the owner's scene, never to the completed image descriptor. */
struct DECLSPEC_ALIGN(64) client_surface_handoff_slot
{
    UINT64 cookie;
    UINT64 identity;
    UINT64 target_epoch;
    UINT64 source;
    UINT64 source_visual;
    UINT producer_process;
    UINT window;
    UINT toplevel;
    UINT flags;
    UINT width;
    UINT height;
    RECT damage;
    UINT64 source_sequence;
    UINT64 damage_base_sequence;
    UINT64 storage_id; /* nonzero for immutable GPU storage; unique within the producer process */
    UINT source_index;
};

C_ASSERT( sizeof(struct client_surface_handoff_slot) == 128 );

/* A channel has one serialized completion publisher and one compositor
 * consumer. Server/endpoint retirement closes the channel without changing
 * either sequence or acknowledging unfinished native reads. */
struct DECLSPEC_ALIGN(64) client_surface_handoff_channel
{
    LONG64 producer_sequence;
    LONG64 source_publications[CLIENT_SURFACE_SOURCE_FRAME_COUNT];
    UINT64 producer_padding[7 - CLIENT_SURFACE_SOURCE_FRAME_COUNT];
    LONG64 consumer_sequence;
    LONG64 source_releases[CLIENT_SURFACE_SOURCE_FRAME_COUNT];
    UINT64 consumer_padding[7 - CLIENT_SURFACE_SOURCE_FRAME_COUNT];
    UINT64 cookie;
    UINT64 identity;
    UINT producer_process;
    UINT window;
    UINT toplevel;
    LONG endpoints;
    LONG closed;
    UINT reserved[7];
    struct client_surface_handoff_slot slots[CLIENT_SURFACE_HANDOFF_RING_SIZE];
};

C_ASSERT( offsetof(struct client_surface_handoff_channel, slots) == 192 );

/* A process pair can share many surface channels. The bitmap and parked
 * notification are hints; each channel's sequence is the authoritative queue. */
struct DECLSPEC_ALIGN(64) client_surface_handoff_shared
{
    UINT64 magic;
    UINT64 mapping_id;
    UINT version;
    UINT channel_count;
    LONG ready_sequence;
    LONG ready_parked;
    LONG release_sequence;
    LONG release_parked;
    UINT reserved[6];
    LONG64 ready_bitmap[CLIENT_SURFACE_HANDOFF_BITMAP_WORDS];
    struct client_surface_handoff_channel channels[CLIENT_SURFACE_HANDOFF_CHANNELS];
};

C_ASSERT( offsetof(struct client_surface_handoff_shared, channels) % 64 == 0 );

static inline BOOL client_surface_handoff_consumed( const struct client_surface_handoff_channel *channel,
                                                     UINT64 publication )
{
    UINT64 consumed = __atomic_load_n( &channel->consumer_sequence, __ATOMIC_ACQUIRE );

    /* Outstanding publications are bounded by RING_SIZE, including at wrap. */
    return consumed - publication < ((UINT64)1 << 63);
}

static inline BOOL client_surface_handoff_source_released( const struct client_surface_handoff_channel *channel,
                                                           unsigned int index, UINT64 publication )
{
    UINT64 released = __atomic_load_n( &channel->source_releases[index], __ATOMIC_ACQUIRE );

    return released == publication;
}

enum client_surface_completion_kind
{
    CLIENT_SURFACE_COMPLETION_NONE,
    CLIENT_SURFACE_COMPLETION_EXACT,
    CLIENT_SURFACE_COMPLETION_SHARED,
};

enum client_surface_completion_status
{
    CLIENT_SURFACE_COMPLETION_PENDING,
    CLIENT_SURFACE_COMPLETION_SIGNALED,
    CLIENT_SURFACE_COMPLETION_FAILED,
};

enum client_surface_admission_reason
{
    CLIENT_SURFACE_ACCEPTED,
    CLIENT_SURFACE_CAPACITY_LIMIT,
    CLIENT_SURFACE_EXECUTOR_UNAVAILABLE,
    CLIENT_SURFACE_OUT_OF_MEMORY,
    CLIENT_SURFACE_STALE_OR_CLOSED,
};

enum client_surface_capacity_scope
{
    CLIENT_SURFACE_CAPACITY_NONE,
    CLIENT_SURFACE_CAPACITY_GLOBAL,
    CLIENT_SURFACE_CAPACITY_SURFACE,
};

struct client_surface_admission
{
    enum client_surface_admission_reason reason;
    enum client_surface_capacity_scope scope;
};

enum client_surface_present_owner
{
    CLIENT_SURFACE_PRESENT_CALLER,
    CLIENT_SURFACE_PRESENT_EXECUTOR,
};

/* Producer handoff is not owner publication. The owner validates and commits
 * its scene separately; none of these results acknowledges that commit. */
enum client_surface_handoff_result
{
    CLIENT_SURFACE_HANDOFF_NOT_QUEUED,
    CLIENT_SURFACE_HANDOFF_QUEUED,
};

struct client_surface_present_result
{
    enum client_surface_present_owner owner;
    enum client_surface_completion_status completion;
    BOOL image_complete;
    enum client_surface_handoff_result handoff;
};

enum client_surface_completion_worker_disposition
{
    CLIENT_SURFACE_COMPLETION_WORKER_REUSE,
    CLIENT_SURFACE_COMPLETION_WORKER_RETIRE,
};

struct client_surface_completion_result
{
    enum client_surface_completion_status status;
    enum client_surface_completion_worker_disposition worker;
};

static inline struct client_surface_completion_result client_surface_completion_result(
    enum client_surface_completion_status status )
{
    return (struct client_surface_completion_result){status, CLIENT_SURFACE_COMPLETION_WORKER_REUSE};
}

/* A poll consumes neither the token nor its image on PENDING. A zero timeout
 * tests the current boundary once. Native queries may still take longer than
 * the supplied wait budget. RETIRE is terminal: the caller must not execute
 * another completion callback on this worker. Success is revalidated against
 * the original submission deadline after return and capture. Expiry rejects
 * adoption, not native ownership: releases and worker-slot reuse wait for
 * real native return and, for RETIRE, actual thread exit. */
typedef struct client_surface_completion_result (*client_surface_completion_wait_func)( void *context, DWORD timeout );
typedef void (*client_surface_completion_release_func)( void *context );
struct client_surface_completion
{
    enum client_surface_completion_kind kind;
    BOOL external_result; /* completion result is supplied by the caller or queued token */
    client_surface_completion_wait_func wait;
    client_surface_completion_release_func release;
    void *context;
};

struct client_surface;
struct client_surface_frame;

struct client_surface_capture
{
    /* Extent of prepared private storage. Neither its existence nor its size
     * proves completion; the core must consume the host result and freeze it. */
    SIZE size;
    /* Make completed private storage readable without any surface lock. The
     * context owns that storage until release, including during target changes.
     * This native operation may stall; it has no access to mutable surface state. */
    BOOL (*read)( void *context );
    /* Apply readable storage under the surface submission lock. This must not
     * wait for GPU work. The core revalidates target and source ownership after
     * read returns, before allowing apply to modify the backend image. */
    BOOL (*apply)( void *context, struct client_surface *surface, struct client_surface_frame *frame );
    /* Own the storage independently of the completion fence, including when
     * a failed or stale completion prevents capture from being called. */
    void (*release)( void *context );
    void *context;
};

static inline BOOL client_surface_completion_result_is_external(
    const struct client_surface_completion *completion )
{
    return completion->external_result;
}

#endif /* __WINE_CLIENT_SURFACE_H */
