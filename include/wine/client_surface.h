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

enum client_surface_handoff_state
{
    CLIENT_SURFACE_HANDOFF_FREE,
    CLIENT_SURFACE_HANDOFF_SUBMITTED,
    CLIENT_SURFACE_HANDOFF_READY,
    CLIENT_SURFACE_HANDOFF_READING,
    CLIENT_SURFACE_HANDOFF_RELEASED,
    CLIENT_SURFACE_HANDOFF_LOST,
};

#define CLIENT_SURFACE_HANDOFF_STATE_BITS 3
#define CLIENT_SURFACE_HANDOFF_STATE_MASK ((UINT64)((1u << CLIENT_SURFACE_HANDOFF_STATE_BITS) - 1))
#define CLIENT_SURFACE_HANDOFF_GENERATION_SHIFT CLIENT_SURFACE_HANDOFF_STATE_BITS
#define CLIENT_SURFACE_HANDOFF_SLOTS 1024
#define CLIENT_SURFACE_HANDOFF_BITMAP_WORDS (CLIENT_SURFACE_HANDOFF_SLOTS / 64)
/* futex_waitv accepts 128 waiters.  Reserve one for local compositor jobs. */
#define CLIENT_SURFACE_HANDOFF_MAX_POOLS_PER_CONSUMER 127
#define CLIENT_SURFACE_HANDOFF_MAGIC ((UINT64)0x57435348414e444full)
#define CLIENT_SURFACE_HANDOFF_VERSION 3
#define CLIENT_SURFACE_HANDOFF_MAX_CLIP_RECTS 16

#define CLIENT_SURFACE_HANDOFF_NATIVE_X11 0x0001
#define CLIENT_SURFACE_HANDOFF_FULL_DAMAGE 0x0002
#define CLIENT_SURFACE_HANDOFF_CLIPPED 0x0004
#define CLIENT_SURFACE_HANDOFF_XFIXES_CLIP 0x0008
#define CLIENT_SURFACE_HANDOFF_ENDPOINT_PRODUCER 0x0001
#define CLIENT_SURFACE_HANDOFF_ENDPOINT_CONSUMER 0x0002

static inline UINT64 client_surface_handoff_control( UINT64 generation,
                                                     enum client_surface_handoff_state state )
{
    return generation << CLIENT_SURFACE_HANDOFF_GENERATION_SHIFT | state;
}

static inline UINT64 client_surface_handoff_generation( UINT64 control )
{
    return control >> CLIENT_SURFACE_HANDOFF_GENERATION_SHIFT;
}

static inline UINT64 client_surface_handoff_next_generation( UINT64 generation )
{
    generation = (generation + 1) & (~(UINT64)0 >> CLIENT_SURFACE_HANDOFF_GENERATION_SHIFT);
    if (!generation) generation = 1;
    return generation;
}

static inline enum client_surface_handoff_state client_surface_handoff_state( UINT64 control )
{
    return control & CLIENT_SURFACE_HANDOFF_STATE_MASK;
}

struct client_surface_handoff_clip_rect
{
    SHORT x;
    SHORT y;
    USHORT width;
    USHORT height;
};

C_ASSERT( sizeof(struct client_surface_handoff_clip_rect) == 8 );

/* The producer owns the payload from SUBMITTED until READY. The owner acquires
 * it with READY -> READING and returns source storage with RELEASED. */
struct DECLSPEC_ALIGN(64) client_surface_handoff_slot
{
    LONG64 control;
    UINT64 cookie;
    UINT64 identity;
    UINT64 scene_epoch;
    UINT64 scene_generation;
    UINT64 target_seq;
    UINT64 source;
    UINT64 source_visual;
    UINT producer_process;
    UINT window;
    UINT toplevel;
    UINT flags;
    RECT destination;
    UINT width;
    UINT height;
    RECT damage;
    LONG endpoints;
    UINT clip_count;
    UINT64 clip_region;
    struct client_surface_handoff_clip_rect clips[CLIENT_SURFACE_HANDOFF_MAX_CLIP_RECTS];
    UINT64 reserved[7];
};

C_ASSERT( sizeof(struct client_surface_handoff_slot) == 320 );

/* One mapping is shared by one producer/owner process pair. There is a single
 * owner-side consumer, so the lfsync-style parked claim cannot strand another
 * compositor waiter. */
struct DECLSPEC_ALIGN(64) client_surface_handoff_shared
{
    UINT64 magic;
    UINT64 mapping_id;
    UINT version;
    UINT slot_count;
    LONG ready_sequence;
    LONG ready_parked;
    LONG release_sequence;
    LONG release_parked;
    UINT reserved[6];
    LONG64 ready_bitmap[CLIENT_SURFACE_HANDOFF_BITMAP_WORDS];
    struct client_surface_handoff_slot slots[CLIENT_SURFACE_HANDOFF_SLOTS];
};

C_ASSERT( offsetof(struct client_surface_handoff_shared, slots) % 64 == 0 );

enum client_surface_completion_kind
{
    CLIENT_SURFACE_COMPLETION_NONE,
    CLIENT_SURFACE_COMPLETION_EXACT,
    CLIENT_SURFACE_COMPLETION_SHARED,
};

typedef BOOL (*client_surface_completion_wait_func)( void *context, DWORD timeout );
typedef void (*client_surface_completion_release_func)( void *context );

struct client_surface_completion
{
    enum client_surface_completion_kind kind;
    BOOL external_result; /* completion result is supplied by the caller or queued token */
    client_surface_completion_wait_func wait;
    client_surface_completion_release_func release;
    void *context;
};

static inline BOOL client_surface_completion_result_is_external(
    const struct client_surface_completion *completion )
{
    return completion->external_result;
}

#endif /* __WINE_CLIENT_SURFACE_H */
