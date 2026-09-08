/*
 * Android pseudo-device handling
 *
 * Copyright 2014-2017 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>
#include <linux/sync_file.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "android.h"
#include "wine/server.h"
#include "wine/debug.h"

#include <dlfcn.h>

#define ANDROID_LOG_ERR ANDROID_LOG_ERROR
#define ANDROID_LOG_TRACE ANDROID_LOG_VERBOSE
#define ANDROID_LOG_FIXME 16

static int log_flags;

#define LOG(level, fmt, ...) \
    do { \
        if (!(log_flags & (1 << __WINE_DBCL_INIT)) || (log_flags & (1 << __WINE_DBCL_ ## level))) \
        { \
            int prio = (ANDROID_LOG_ ## level == ANDROID_LOG_FIXME) ? \
                       ANDROID_LOG_WARN : ANDROID_LOG_ ## level; \
            p__android_log_print(prio, "wineandroid.drv", "%s: %s" fmt, \
                                 __func__, \
                                 (ANDROID_LOG_ ## level == ANDROID_LOG_FIXME) ? "FIXME: " : "", \
                                 ##__VA_ARGS__); \
        } \
    } while (0)

#define DBGSTR_RECT_FMT "(%d,%d)-(%d,%d)"
#define DBGSTR_RECT(rect) (int)(rect)->left, (int)(rect)->top, (int)(rect)->right, (int)(rect)->bottom


WINE_DEFAULT_DEBUG_CHANNEL(android);

static int desktop_client_fd = -1;
static jobject java_object;

static HWND desktop_window;

int event_sink = -1;
static pthread_mutex_t dispatch_ioctl_lock = PTHREAD_MUTEX_INITIALIZER;

#define ANDROIDCONTROLTYPE  ((ULONG)'A')
#define ANDROID_IOCTL(n) CTL_CODE(ANDROIDCONTROLTYPE, n, METHOD_BUFFERED, FILE_READ_ACCESS)

enum android_ioctl
{
    IOCTL_CREATE_DESKTOP_VIEW,
    IOCTL_CREATE_WINDOW,
    IOCTL_DESTROY_WINDOW,
    IOCTL_WINDOW_POS_CHANGED,
    IOCTL_SET_WINDOW_PARENT,
    IOCTL_DEQUEUE_BUFFER,
    IOCTL_QUEUE_BUFFER,
    IOCTL_CANCEL_BUFFER,
    IOCTL_QUERY,
    IOCTL_PERFORM,
    IOCTL_SET_SWAP_INT,
    IOCTL_SET_CAPTURE,
    IOCTL_SET_CURSOR,
    NB_IOCTLS
};

#define NB_CACHED_BUFFERS 4

/* data about the native window in the context of the Java process */
struct native_win_data
{
    struct ANativeWindow       *parent;
    struct AHardwareBuffer *buffers[NB_CACHED_BUFFERS];
    HWND                        hwnd;
    BOOL                        opengl;
    int                         generation;
    int                         api;
    int                         buffer_format;
    int                         swap_interval;
    int                         buffer_lru[NB_CACHED_BUFFERS];
};

/* wrapper for a native window in the context of the client (non-Java) process */
struct native_win_wrapper
{
    struct ANativeWindow          win;
    struct
    {
        struct AHardwareBuffer   *self;
        int                       buffer_id;
        int                       generation;
    } buffers[NB_CACHED_BUFFERS];
    struct AHardwareBuffer       *locked_buffer;
    HWND                          hwnd;
    BOOL                          opengl;
    LONG                          ref;
};

#define IPC_SOCKET_NAME "\0\\Device\\WineAndroid"
#define IPC_SOCKET_ADDR_LEN ((socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof(IPC_SOCKET_NAME) - 1))

static const struct sockaddr_un ipc_addr = {
    .sun_family = AF_UNIX,
    .sun_path = IPC_SOCKET_NAME,
};

struct ioctl_header
{
    int  hwnd;
    BOOL opengl;
};

/* The two optional reply descriptors have independent ownership and roles.
 * Requests may carry only a fence, for queue/cancel. */
struct ioctl_fds
{
    int buffer;
    int fence;
};

struct android_ioctl_reply
{
    int status;
    unsigned int fd_mask;
};

#define IOCTL_BUFFER_FD 1
#define IOCTL_FENCE_FD  2

/* Java callbacks have no Wine TEB. Use their existing channel flags, and
 * perform no diagnostic syscall unless the android trace channel is enabled.
 * SCM_RIGHTS ordering and the socket peer identify the transfer; an anonymous
 * inode or a fence name alone is not a unique buffer lifetime. */
#define TRACE_FENCE(server, ...) \
    do { if ((server) ? (log_flags & (1 << __WINE_DBCL_TRACE)) : TRACE_ON(android)) \
        trace_fence( server, __VA_ARGS__ ); } while (0)

static void trace_fence( BOOL server, const char *stage, int socket, int code, const void *data, size_t size,
                         int fd, int result, int events )
{
    struct sync_file_info info = {{0}};
    struct stat st = {0};
    struct ucred peer = {0};
    socklen_t len = sizeof(peer);
    int identity[4] = {0, 0, -1, 0};
    int saved_errno = errno, peer_ret = -1, stat_ret = -1, info_ret = -1, info_errno = 0;
    char text[512];

    if (data && size >= sizeof(identity)) memcpy( identity, data, sizeof(identity) );
    if (socket != -1) peer_ret = getsockopt( socket, SOL_SOCKET, SO_PEERCRED, &peer, &len );
    if (fd != -1)
    {
        stat_ret = fstat( fd, &st );
        info_ret = ioctl( fd, SYNC_IOC_FILE_INFO, &info );
        if (info_ret < 0) info_errno = errno;
    }
    snprintf( text, sizeof(text), "fence %s pid %u tid %u socket %d peer %d peer_ret %d code %d "
              "hwnd %08x opengl %u id %d gen %d fd %d result %d events %x errno %d "
              "stat %d dev %llu ino %llu info %d info_errno %d status %d count %u name %.32s\n",
              stage, getpid(), gettid(), socket, peer.pid, peer_ret, code, identity[0], identity[1],
              identity[2], identity[3], fd, result, events, saved_errno, stat_ret,
              (unsigned long long)st.st_dev, (unsigned long long)st.st_ino, info_ret, info_errno,
              info.status, info.num_fences, info.name );
    if (server) LOG( TRACE, "%s", text );
    else TRACE( "%s", text );
    errno = saved_errno;
}

static void close_fence( BOOL server, int fd )
{
    int ret = close( fd ), saved_errno = errno;

    if (server)
    {
        if (log_flags & (1 << __WINE_DBCL_TRACE))
            LOG( TRACE, "fence close pid %u tid %u fd %d result %d errno %d\n",
                 getpid(), gettid(), fd, ret, saved_errno );
    }
    else if (TRACE_ON(android))
        TRACE( "fence close pid %u tid %u fd %d result %d errno %d\n",
               getpid(), gettid(), fd, ret, saved_errno );
    errno = saved_errno;
}

static void close_ioctl_fds( struct ioctl_fds *fds )
{
    if (fds->buffer != -1) close( fds->buffer );
    if (fds->fence != -1) close_fence( TRUE, fds->fence );
    fds->buffer = fds->fence = -1;
}

static unsigned int get_message_fds( struct msghdr *msg, int *fds, unsigned int capacity )
{
    struct cmsghdr *cmsg;
    unsigned int count = 0, i;

    for (cmsg = CMSG_FIRSTHDR( msg ); cmsg; cmsg = CMSG_NXTHDR( msg, cmsg ))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int)))
        {
            unsigned int size = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *received = (int *)CMSG_DATA(cmsg);

            for (i = 0; i < size; i++)
            {
                if (count < capacity) fds[count] = received[i];
                else close( received[i] );
                count++;
            }
        }
    return count;
}

struct ioctl_android_create_desktop_view
{
    struct ioctl_header hdr;
    int                 log_flags;
};

struct ioctl_android_create_window
{
    struct ioctl_header hdr;
    int                 parent;
    BOOL                is_desktop;
};

struct ioctl_android_destroy_window
{
    struct ioctl_header hdr;
};

struct ioctl_android_window_pos_changed
{
    struct ioctl_header hdr;
    RECT                window_rect;
    RECT                client_rect;
    RECT                visible_rect;
    int                 style;
    int                 flags;
    int                 after;
    int                 owner;
};

struct ioctl_android_dequeueBuffer
{
    struct ioctl_header hdr;
    int buffer_id;
    int generation;
};

struct ioctl_android_queueBuffer
{
    struct ioctl_header hdr;
    int                 buffer_id;
    int                 generation;
};

struct ioctl_android_cancelBuffer
{
    struct ioctl_header hdr;
    int                 buffer_id;
    int                 generation;
    BOOL                discard;
};

struct ioctl_android_query
{
    struct ioctl_header hdr;
    int                 what;
    int                 value;
};

struct ioctl_android_perform
{
    struct ioctl_header hdr;
    int                 operation;
    int                 args[4];
};

struct ioctl_android_set_swap_interval
{
    struct ioctl_header hdr;
    int                 interval;
};

struct ioctl_android_set_window_parent
{
    struct ioctl_header hdr;
    int                 parent;
};

struct ioctl_android_set_capture
{
    struct ioctl_header hdr;
};

struct ioctl_android_set_cursor
{
    struct ioctl_header hdr;
    int                 id;
    int                 width;
    int                 height;
    int                 hotspotx;
    int                 hotspoty;
    int                 bits[1];
};

static struct native_win_data *data_map[65536];

static unsigned int data_map_idx( HWND hwnd, BOOL opengl )
{
    /* window handles are always even, so use low-order bit for opengl flag */
    return LOWORD(hwnd) + !!opengl;
}

static struct native_win_data *get_native_win_data( HWND hwnd, BOOL opengl )
{
    struct native_win_data *data = data_map[data_map_idx( hwnd, opengl )];

    if (data && data->hwnd == hwnd && !data->opengl == !opengl) return data;
    LOG( WARN, "unknown win %p opengl %u\n", hwnd, opengl );
    return NULL;
}

static struct native_win_data *get_ioctl_native_win_data( const struct ioctl_header *hdr )
{
    return get_native_win_data( LongToHandle(hdr->hwnd), hdr->opengl );
}

static int get_ioctl_win_parent( HWND parent )
{
    if (parent != NtUserGetDesktopWindow() && !NtUserGetAncestor( parent, GA_PARENT ))
        return HandleToLong( HWND_MESSAGE );
    return HandleToLong( parent );
}

/* Only synchronous client CPU access and the deprecated dequeue wait here.
 * Modern native-window entry points transfer fence ownership without waiting. */
static int wait_fence( int fence )
{
    struct pollfd pollfd = { fence, POLLIN, 0 };
    int ret;

    if (fence == -1) return 0;
    TRACE_FENCE( FALSE, "poll_begin", -1, -1, NULL, 0, fence, 0, pollfd.events );
    do ret = poll( &pollfd, 1, -1 ); while (ret == -1 && errno == EINTR);
    TRACE_FENCE( FALSE, "poll_end", -1, -1, NULL, 0, fence, ret, pollfd.revents );
    if (ret == -1) return -errno;
    if (pollfd.revents & (POLLERR | POLLNVAL)) return -EINVAL;
    return (pollfd.revents & POLLIN) ? 0 : -EIO;
}

static inline struct ANativeWindowBuffer *anwb_from_ahb(AHardwareBuffer *ahb)
{
    static ptrdiff_t off = (ptrdiff_t)-1;

    if (!ahb) return NULL;

    if (off == (ptrdiff_t)-1)
    {
        struct ANativeWindowBuffer *fake =
        (struct ANativeWindowBuffer *)(uintptr_t)0x10000u;

        AHardwareBuffer *h = pANativeWindowBuffer_getHardwareBuffer(fake);
        off = (const char *)h - (const char *)fake;
    }

    return (struct ANativeWindowBuffer *)((char *)ahb - off);
}

static AHardwareBuffer *ahb_from_anwb( struct native_win_wrapper *win, struct ANativeWindowBuffer *buffer, int *buffer_id, int *generation )
{
    AHardwareBuffer *ahb;
    unsigned int i;

    if (!buffer) return NULL;

    ahb = pANativeWindowBuffer_getHardwareBuffer(buffer);
    if (!ahb) return NULL;

    if (win)
    {
        for (i = 0; i < NB_CACHED_BUFFERS; ++i)
        {
            if (win->buffers[i].self != ahb) continue;

            if (buffer_id) *buffer_id = win->buffers[i].buffer_id;
            if (generation) *generation = win->buffers[i].generation;
            break;
        }
        if (i == NB_CACHED_BUFFERS) return NULL;
    }

    return ahb;
}

/* insert a buffer index at the head of the LRU list */
static void insert_buffer_lru( struct native_win_data *win, int index )
{
    unsigned int i;

    for (i = 0; i < NB_CACHED_BUFFERS; i++)
    {
        if (win->buffer_lru[i] == index) break;
        if (win->buffer_lru[i] == -1) break;
    }

    assert( i < NB_CACHED_BUFFERS );
    memmove( win->buffer_lru + 1, win->buffer_lru, i * sizeof(win->buffer_lru[0]) );
    win->buffer_lru[0] = index;
}

static int register_buffer( struct native_win_data *win, struct AHardwareBuffer *buffer, int *is_new )
{
    unsigned int i, empty = NB_CACHED_BUFFERS;

    assert( buffer );
    *is_new = 0;
    for (i = 0; i < NB_CACHED_BUFFERS; i++)
    {
        if (win->buffers[i] == buffer) goto done;
        if (!win->buffers[i] && empty == NB_CACHED_BUFFERS) empty = i;
    }

    i = empty;
    if (i == NB_CACHED_BUFFERS)
    {
        /* reuse the least recently used buffer */
        i = win->buffer_lru[NB_CACHED_BUFFERS - 1];
        assert( i < NB_CACHED_BUFFERS );

        LOG( TRACE, "%p %p evicting buffer %p id %d from cache\n",
               win->hwnd, win->parent, win->buffers[i], i );
        pAHardwareBuffer_release(win->buffers[i]);
    }

    win->buffers[i] = buffer;

    pAHardwareBuffer_acquire(buffer);
    *is_new = 1;
    LOG( TRACE, "%p %p %p -> %d\n", win->hwnd, win->parent, buffer, i );

done:
    insert_buffer_lru( win, i );
    return i;
}

static void unregister_buffer( struct native_win_data *win, unsigned int id )
{
    unsigned int i;

    assert( id < NB_CACHED_BUFFERS && win->buffers[id] );
    LOG( TRACE, "%p %p discarding buffer %p id %u generation %d\n",
         win->hwnd, win->parent, win->buffers[id], id, win->generation );
    pAHardwareBuffer_release( win->buffers[id] );
    win->buffers[id] = NULL;
    for (i = 0; i < NB_CACHED_BUFFERS; i++)
        if (win->buffer_lru[i] == id) break;
    assert( i < NB_CACHED_BUFFERS );
    memmove( win->buffer_lru + i, win->buffer_lru + i + 1,
             (NB_CACHED_BUFFERS - i - 1) * sizeof(win->buffer_lru[0]) );
    win->buffer_lru[NB_CACHED_BUFFERS - 1] = -1;
    if (log_flags & (1 << __WINE_DBCL_TRACE))
    {
        int saved_errno = errno;

        LOG( TRACE, "%p %p released and cleared id %u generation %d cache %p lru %d,%d,%d,%d\n",
             win->hwnd, win->parent, id, win->generation, win->buffers[id],
             win->buffer_lru[0], win->buffer_lru[1], win->buffer_lru[2], win->buffer_lru[3] );
        errno = saved_errno;
    }
}

static struct ANativeWindowBuffer *get_registered_buffer( struct native_win_data *win, int id )
{
    if (id < 0 || id >= NB_CACHED_BUFFERS || !win->buffers[id])
    {
        LOG( ERR, "unknown buffer %d for %p %p\n", id, win->hwnd, win->parent );
        return NULL;
    }
    return anwb_from_ahb(win->buffers[id]);
}

static void release_native_window( struct native_win_data *data )
{
    unsigned int i;

    if (data->parent) pANativeWindow_release( data->parent );
    for (i = 0; i < NB_CACHED_BUFFERS; i++)
    {
        if (data->buffers[i]) pAHardwareBuffer_release(data->buffers[i]);
        data->buffer_lru[i] = -1;
    }
    memset( data->buffers, 0, sizeof(data->buffers) );
}

static void free_native_win_data( struct native_win_data *data )
{
    unsigned int idx = data_map_idx( data->hwnd, data->opengl );

    release_native_window( data );
    free( data );
    data_map[idx] = NULL;
}

static struct native_win_data *create_native_win_data( HWND hwnd, BOOL opengl )
{
    unsigned int i, idx = data_map_idx( hwnd, opengl );
    struct native_win_data *data = data_map[idx];

    if (data)
    {
        LOG( WARN, "data for %p not freed correctly\n", data->hwnd );
        free_native_win_data( data );
    }
    if (!(data = calloc( 1, sizeof(*data) ))) return NULL;
    data->hwnd = hwnd;
    data->opengl = opengl;
    if (!opengl) data->api = NATIVE_WINDOW_API_CPU;
    data->buffer_format = PF_BGRA_8888;
    data_map[idx] = data;
    for (i = 0; i < NB_CACHED_BUFFERS; i++) data->buffer_lru[i] = -1;
    return data;
}

/* register a native window received from the UI thread for use in ioctls */
void register_native_window( HWND hwnd, struct ANativeWindow *win, BOOL opengl )
{
    struct native_win_data *data = NULL;

    pthread_mutex_lock(&dispatch_ioctl_lock);
    data = get_native_win_data( hwnd, opengl );

    if (!win) goto end;  /* do nothing and hold on to the window until we get a new surface */

    if (!data || data->parent == win)
    {
        pANativeWindow_release( win );
        LOG( TRACE, "%p -> %p win %p (unchanged)\n", hwnd, data, win );
        goto end;
    }

    release_native_window( data );
    data->parent = win;
    data->generation++;
    if (data->api) win->perform( win, NATIVE_WINDOW_API_CONNECT, data->api );
    win->perform( win, NATIVE_WINDOW_SET_BUFFERS_FORMAT, data->buffer_format );
    win->setSwapInterval( win, data->swap_interval );
    LOG( TRACE, "%p -> %p win %p\n", hwnd, data, win );
end:
    pthread_mutex_unlock(&dispatch_ioctl_lock);
}

static jobject load_java_method( JNIEnv* env, jmethodID *method, const char *name, const char *args )
{
    if (!*method)
    {
        jclass class;

        class = (*env)->GetObjectClass( env, java_object );
        *method = (*env)->GetMethodID( env, class, name, args );
        if (!*method)
        {
            LOG( FIXME, "method %s not found\n", name );
            return NULL;
        }
    }
    return java_object;
}

static int createDesktopView_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    static int event_pipe[2];
    static jmethodID method;
    jobject object;
    struct ioctl_android_create_desktop_view *res = data;

    if (in_size < sizeof(*res)) return -EINVAL;

    if (event_sink != -1) close(event_sink);

    if (pipe2( event_pipe, O_CLOEXEC | O_NONBLOCK ) == -1)
    {
        LOG( ERR, "could not create data event pipe\n" );
        return -1;
    }

    event_sink = event_pipe[1];
    fds->buffer = event_pipe[0];

    log_flags = res->log_flags; /* Copy logging levels from client */

    if (!(object = load_java_method( env, &method, "createDesktopView", "()V" ))) return -ENOSYS;

    (*env)->CallVoidMethod( env, object, method );
    return 0;
}

static int createWindow_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_create_window *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return -EINVAL;

    if (!(win_data = create_native_win_data( LongToHandle(res->hdr.hwnd), res->hdr.opengl )))
        return -ENOMEM;

    LOG( TRACE, "hwnd %08x opengl %u parent %08x\n", res->hdr.hwnd, res->hdr.opengl, res->parent );

    if (!(object = load_java_method( env, &method, "createWindow", "(IZZI)V" ))) return -ENOSYS;

    (*env)->CallVoidMethod( env, object, method, res->hdr.hwnd, res->is_desktop, res->hdr.opengl, res->parent );
    return 0;
}

static int destroyWindow_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_destroy_window *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return -EINVAL;

    win_data = get_ioctl_native_win_data( &res->hdr );

    LOG( TRACE, "hwnd %08x opengl %u\n", res->hdr.hwnd, res->hdr.opengl );

    if (!(object = load_java_method( env, &method, "destroyWindow", "(I)V" ))) return -ENOSYS;

    (*env)->CallVoidMethod( env, object, method, res->hdr.hwnd );
    if (win_data) free_native_win_data( win_data );
    return 0;
}

static int windowPosChanged_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_window_pos_changed *res = data;

    if (in_size < sizeof(*res)) return -EINVAL;

    LOG( TRACE, "hwnd %08x win " DBGSTR_RECT_FMT " client " DBGSTR_RECT_FMT " visible " DBGSTR_RECT_FMT " style %08x flags %08x after %08x owner %08x\n",
                res->hdr.hwnd, DBGSTR_RECT(&res->window_rect), DBGSTR_RECT(&res->client_rect),
                DBGSTR_RECT(&res->visible_rect), res->style, res->flags, res->after, res->owner );

    if (!(object = load_java_method( env, &method, "windowPosChanged", "(IIIIIIIIIIIIIIIII)V" )))
        return -ENOSYS;

    (*env)->CallVoidMethod( env, object, method, res->hdr.hwnd, res->flags, res->after, res->owner, res->style,
                            res->window_rect.left, res->window_rect.top, res->window_rect.right, res->window_rect.bottom,
                            res->client_rect.left, res->client_rect.top, res->client_rect.right, res->client_rect.bottom,
                            res->visible_rect.left, res->visible_rect.top, res->visible_rect.right, res->visible_rect.bottom );
    return 0;
}

static int dequeueBuffer_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    struct ANativeWindow *parent;
    struct ioctl_android_dequeueBuffer *res = data;
    struct native_win_data *win_data;
    struct ANativeWindowBuffer *buffer = NULL;
    AHardwareBuffer *ahb = NULL;
    int fence = -1, ret, is_new = 0, cancel_ret;

    if (out_size < sizeof( *res ) || in_size < sizeof(res->hdr)) return -EINVAL;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return -ENOENT;
    if (!(parent = win_data->parent)) return -EWOULDBLOCK;

    res->buffer_id = -1;
    res->generation = 0;
    *ret_size = sizeof(*res);

    /* The CPU lock is performed in the client process, so Surface::lock()
     * cannot set the allocation usage for us before dequeuing the buffer. */
    if (!win_data->opengl && (ret = parent->perform( parent, NATIVE_WINDOW_SET_USAGE,
            (unsigned int)(AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN) )))
        return ret;

    ret = parent->dequeueBuffer( parent, &buffer, &fence );
    if (ret)
    {
        LOG( ERR, "%08x failed %d\n", res->hdr.hwnd, ret );
        return ret;
    }

    if (!buffer)
    {
        LOG( ERR, "got invalid buffer\n" );
        if (fence != -1) close( fence );
        return -EINVAL;
    }

    LOG( TRACE, "%08x got buffer %p fence %d\n", res->hdr.hwnd, buffer, fence );
    ahb = pANativeWindowBuffer_getHardwareBuffer( buffer );
    if (!ahb)
    {
        ret = -EINVAL;
        goto failed;
    }

    res->buffer_id = register_buffer( win_data, ahb, &is_new );
    res->generation = win_data->generation;
    TRACE_FENCE( TRUE, "native_dequeue", -1, IOCTL_DEQUEUE_BUFFER, res, sizeof(*res), fence, ret, 0 );

    if (is_new)
    {
        int sv[2] = { -1, -1 };

        if (socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv ) < 0)
        {
            ret = -errno;
            goto failed;
        }

        ret = pAHardwareBuffer_sendHandleToUnixSocket( ahb, sv[0] );
        if (log_flags & (1 << __WINE_DBCL_TRACE))
        {
            int saved_errno = errno;

            LOG( TRACE, "%08x opengl %u id %d generation %d ahb %p send fd %d receive fd %d handle returned %d\n",
                 res->hdr.hwnd, res->hdr.opengl, res->buffer_id, res->generation, ahb, sv[0], sv[1], ret );
            errno = saved_errno;
        }
        close( sv[0] );
        if (ret)
        {
            close( sv[1] );
            goto failed;
        }

        fds->buffer = sv[1];
    }

    fds->fence = fence;
    return 0;

failed:
    /* No client owns this dequeue.  Return its acquire fence to the native
     * window without waiting on, closing or reusing the transferred fd. */
    cancel_ret = parent->cancelBuffer( parent, buffer, fence );
    LOG( TRACE, "%08x transfer failed %d, cancel buffer %p fence %d returned %d\n",
         res->hdr.hwnd, ret, buffer, fence, cancel_ret );
    if (cancel_ret) LOG( ERR, "%08x failed to cancel undelivered buffer: %d\n", res->hdr.hwnd, cancel_ret );
    if (is_new) unregister_buffer( win_data, res->buffer_id );
    res->buffer_id = -1;
    return ret;
}

static int cancelBuffer_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    struct ioctl_android_cancelBuffer *res = data;
    struct ANativeWindow *parent;
    struct ANativeWindowBuffer *buffer;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return -EINVAL;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return -ENOENT;
    if (!(parent = win_data->parent)) return -EWOULDBLOCK;
    if (res->generation != win_data->generation) return 0;  /* obsolete buffer, ignore */

    if (!(buffer = get_registered_buffer( win_data, res->buffer_id ))) return -ENOENT;

    LOG( TRACE, "%08x buffer %p\n", res->hdr.hwnd, buffer );
    ret = parent->cancelBuffer( parent, buffer, fds->fence );
    fds->fence = -1;
    if (res->discard) unregister_buffer( win_data, res->buffer_id );
    LOG( TRACE, "%08x id %d generation %d discard %d cancel returned %d\n",
         res->hdr.hwnd, res->buffer_id, res->generation, res->discard, ret );
    return ret;
}

static int queueBuffer_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    struct ioctl_android_queueBuffer *res = data;
    struct ANativeWindow *parent;
    struct ANativeWindowBuffer *buffer;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return -EINVAL;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return -ENOENT;
    if (!(parent = win_data->parent)) return -EWOULDBLOCK;
    if (res->generation != win_data->generation) return 0;  /* obsolete buffer, ignore */

    if (!(buffer = get_registered_buffer( win_data, res->buffer_id ))) return -ENOENT;

    LOG( TRACE, "%08x buffer %p\n", res->hdr.hwnd, buffer );
    ret = parent->queueBuffer( parent, buffer, fds->fence );
    fds->fence = -1;
    return ret;
}

static int query_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    struct ioctl_android_query *res = data;
    struct ANativeWindow *parent;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res) || out_size < sizeof(*res)) return -EINVAL;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return -ENOENT;
    if (!(parent = win_data->parent)) return -EWOULDBLOCK;

    *ret_size = sizeof( *res );
    ret = parent->query( parent, res->what, &res->value );
    return ret;
}

static int perform_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    struct ioctl_android_perform *res = data;
    struct ANativeWindow *parent;
    struct native_win_data *win_data;
    int ret = -ENOENT;

    if (in_size < sizeof(*res)) return -EINVAL;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return -ENOENT;
    if (!(parent = win_data->parent)) return -EWOULDBLOCK;

    switch (res->operation)
    {
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
        ret = parent->perform( parent, res->operation, res->args[0] );
        if (!ret) win_data->buffer_format = res->args[0];
        break;
    case NATIVE_WINDOW_API_CONNECT:
        ret = parent->perform( parent, res->operation, res->args[0] );
        if (!ret) win_data->api = res->args[0];
        break;
    case NATIVE_WINDOW_API_DISCONNECT:
        ret = parent->perform( parent, res->operation, res->args[0] );
        if (!ret) win_data->api = 0;
        break;
    case NATIVE_WINDOW_SET_USAGE:
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
    case NATIVE_WINDOW_SET_SCALING_MODE:
        ret = parent->perform( parent, res->operation, res->args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        ret = parent->perform( parent, res->operation, (size_t)res->args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        ret = parent->perform( parent, res->operation, res->args[0], res->args[1] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        ret = parent->perform( parent, res->operation, res->args[0], res->args[1], res->args[2] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
        ret = parent->perform( parent, res->operation, res->args[0] | ((int64_t)res->args[1] << 32) );
        break;
    case NATIVE_WINDOW_CONNECT:
    case NATIVE_WINDOW_DISCONNECT:
    case NATIVE_WINDOW_UNLOCK_AND_POST:
        ret = parent->perform( parent, res->operation );
        break;
    case NATIVE_WINDOW_SET_CROP:
    {
        android_native_rect_t rect;
        rect.left   = res->args[0];
        rect.top    = res->args[1];
        rect.right  = res->args[2];
        rect.bottom = res->args[3];
        ret = parent->perform( parent, res->operation, &rect );
        break;
    }
    case NATIVE_WINDOW_LOCK:
    default:
        LOG( FIXME, "unsupported perform op %d\n", res->operation );
        break;
    }
    return ret;
}

static int setSwapInterval_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    struct ioctl_android_set_swap_interval *res = data;
    struct ANativeWindow *parent;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return -EINVAL;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return -ENOENT;
    win_data->swap_interval = res->interval;

    if (!(parent = win_data->parent)) return 0;
    ret = parent->setSwapInterval( parent, res->interval );
    return ret;
}

static int setWindowParent_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_set_window_parent *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return -EINVAL;

    if (!(win_data = get_ioctl_native_win_data( &res->hdr ))) return -ENOENT;

    LOG( TRACE, "hwnd %08x parent %08x\n", res->hdr.hwnd, res->parent );

    if (!(object = load_java_method( env, &method, "setParent", "(II)V" ))) return -ENOSYS;

    (*env)->CallVoidMethod( env, object, method, res->hdr.hwnd, res->parent );
    return 0;
}

static int setCapture_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    struct ioctl_android_set_capture *res = data;

    if (in_size < sizeof(*res)) return -EINVAL;

    if (res->hdr.hwnd && !get_ioctl_native_win_data( &res->hdr )) return -ENOENT;

    LOG( TRACE, "hwnd %08x\n", res->hdr.hwnd );

    return 0;
}

static int setCursor_ioctl( JNIEnv* env, void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds )
{
    static jmethodID method;
    jobject object;
    int size;
    struct ioctl_android_set_cursor *res = data;

    if (in_size < offsetof( struct ioctl_android_set_cursor, bits )) return -EINVAL;

    if (res->width < 0 || res->height < 0 || res->width > 256 || res->height > 256)
        return -EINVAL;

    size = res->width * res->height;
    if (in_size != offsetof( struct ioctl_android_set_cursor, bits[size] ))
        return -EINVAL;

    LOG( TRACE, "hwnd %08x size %d\n", res->hdr.hwnd, size );

    if (!(object = load_java_method( env, &method, "setCursor", "(IIIII[I)V" )))
        return -ENOSYS;

    if (size)
    {
        jintArray array = (*env)->NewIntArray( env, size );
        (*env)->SetIntArrayRegion( env, array, 0, size, (jint *)res->bits );
        (*env)->CallVoidMethod( env, object, method, 0, res->width, res->height,
                                    res->hotspotx, res->hotspoty, array );
        (*env)->DeleteLocalRef( env, array );
    }
    else (*env)->CallVoidMethod( env, object, method, res->id, 0, 0, 0, 0, NULL );

    return 0;
}

typedef int (*ioctl_func)( JNIEnv* env, void *in, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size, struct ioctl_fds *fds );
static const ioctl_func ioctl_funcs[] =
{
    createDesktopView_ioctl,    /* IOCTL_CREATE_DESKTOP_VIEW */
    createWindow_ioctl,         /* IOCTL_CREATE_WINDOW */
    destroyWindow_ioctl,        /* IOCTL_DESTROY_WINDOW */
    windowPosChanged_ioctl,     /* IOCTL_WINDOW_POS_CHANGED */
    setWindowParent_ioctl,      /* IOCTL_SET_WINDOW_PARENT */
    dequeueBuffer_ioctl,        /* IOCTL_DEQUEUE_BUFFER */
    queueBuffer_ioctl,          /* IOCTL_QUEUE_BUFFER */
    cancelBuffer_ioctl,         /* IOCTL_CANCEL_BUFFER */
    query_ioctl,                /* IOCTL_QUERY */
    perform_ioctl,              /* IOCTL_PERFORM */
    setSwapInterval_ioctl,      /* IOCTL_SET_SWAP_INT */
    setCapture_ioctl,           /* IOCTL_SET_CAPTURE */
    setCursor_ioctl,            /* IOCTL_SET_CURSOR */
};

static ALooper *looper;
static JNIEnv *looper_env; /* JNIEnv for the main thread looper. Must only be used from that thread. */

/* Handle a single ioctl request from a client socket.
 * Returns 0 if a request was handled successfully and the caller may
 * continue draining the socket, -1 if there is nothing more to read
 * for now, and 1 if the client fd should be closed.
 */
static int handle_ioctl_message( JNIEnv *env, int fd )
{
    struct ANativeWindow *dequeued_parent = NULL;
    struct ANativeWindowBuffer *dequeued_buffer = NULL;
    AHardwareBuffer *dequeued_ahb = NULL;
    char buffer[1024], control[CMSG_SPACE(2 * sizeof(int))];
    struct ioctl_fds fds = { -1, -1 };
    struct android_ioctl_reply result = { -EINVAL, 0 };
    int code = 0, received[2] = { -1, -1 }, sent[2];
    unsigned int count;
    ULONG_PTR reply_size = 0;
    ssize_t ret;
    struct iovec iov[2] = { { &code, sizeof(code) }, { buffer, sizeof(buffer) } };
    struct iovec reply_iov[2] = { { &result, sizeof(result) }, { buffer, 0 } };
    struct msghdr msg = { NULL, 0, iov, 2, control, sizeof(control), 0 };
    struct msghdr reply = { NULL, 0, reply_iov, 2, NULL, 0, 0 };
    struct cmsghdr *cmsg;

    ret = recvmsg( fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC );
    if (ret < 0)
    {
        if (errno == EINTR) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return 1;
    }

    count = get_message_fds( &msg, received, ARRAY_SIZE(received) );
    if (!ret || ret < sizeof(code) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        count > 1 || (count && code != IOCTL_QUEUE_BUFFER && code != IOCTL_CANCEL_BUFFER))
    {
        if (received[0] != -1) close( received[0] );
        if (received[1] != -1) close( received[1] );
        return 1;
    }
    fds.fence = received[0];
    ret -= sizeof(code);
    if (code == IOCTL_QUEUE_BUFFER || code == IOCTL_CANCEL_BUFFER)
        TRACE_FENCE( TRUE, "request_received", fd, code, buffer, ret, fds.fence, ret, msg.msg_flags );

    if ((unsigned int)code < NB_IOCTLS)
    {
        if (ret >= sizeof(struct ioctl_header))
        {
            pthread_mutex_lock( &dispatch_ioctl_lock );
            result.status = ioctl_funcs[code]( env, buffer, ret, sizeof(buffer), &reply_size, &fds );
            if (code == IOCTL_DEQUEUE_BUFFER && !result.status)
            {
                struct ioctl_android_dequeueBuffer *dequeue = (void *)buffer;
                struct native_win_data *win = get_ioctl_native_win_data( &dequeue->hdr );

                /* Keep the actual dequeue alive across an unlocked reply and
                 * a concurrent UI replacement of this native window. */
                dequeued_parent = win->parent;
                dequeued_ahb = win->buffers[dequeue->buffer_id];
                dequeued_buffer = anwb_from_ahb( dequeued_ahb );
                dequeued_parent->common.incRef( &dequeued_parent->common );
                pAHardwareBuffer_acquire( dequeued_ahb );
            }
            if (IOCTL_CREATE_DESKTOP_VIEW == code) /* special case: desktop client */
                desktop_client_fd = fd;
            pthread_mutex_unlock( &dispatch_ioctl_lock );
        }
    }
    else
    {
        LOG( FIXME, "ioctl %x not supported\n", code );
        result.status = -ENOTSUP;
    }

    /* A stale or rejected queue/cancel still consumes the caller's fence. */
    if ((code == IOCTL_QUEUE_BUFFER || code == IOCTL_CANCEL_BUFFER) && fds.fence != -1)
    {
        close_fence( TRUE, fds.fence );
        fds.fence = -1;
    }
    count = 0;
    if (fds.buffer != -1)
    {
        result.fd_mask |= IOCTL_BUFFER_FD;
        sent[count++] = fds.buffer;
    }
    if (fds.fence != -1)
    {
        result.fd_mask |= IOCTL_FENCE_FD;
        sent[count++] = fds.fence;
    }
    reply_iov[1].iov_len = reply_size;
    if (count)
    {
        reply.msg_control = control;
        reply.msg_controllen = CMSG_SPACE(count * sizeof(int));
        cmsg = CMSG_FIRSTHDR( &reply );
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
        memcpy( CMSG_DATA(cmsg), sent, count * sizeof(int) );
    }

    if (code == IOCTL_DEQUEUE_BUFFER)
        TRACE_FENCE( TRUE, "reply_send", fd, code, buffer, reply_size, fds.fence, result.status, result.fd_mask );
    ret = sendmsg( fd, &reply, MSG_NOSIGNAL );
    if (code == IOCTL_DEQUEUE_BUFFER)
        TRACE_FENCE( TRUE, "reply_sent", fd, code, buffer, reply_size, fds.fence, ret, result.fd_mask );
    if (ret != sizeof(result) + reply_size && dequeued_parent)
    {
        struct ioctl_android_dequeueBuffer *dequeue = (void *)buffer;
        struct native_win_data *win;
        int cancel_ret;

        pthread_mutex_lock( &dispatch_ioctl_lock );
        cancel_ret = dequeued_parent->cancelBuffer( dequeued_parent, dequeued_buffer, fds.fence );
        fds.fence = -1; /* native cancellation owns even an unsignaled fence */
        win = get_ioctl_native_win_data( &dequeue->hdr );
        if (win && win->parent == dequeued_parent && win->generation == dequeue->generation &&
            anwb_from_ahb(win->buffers[dequeue->buffer_id]) == dequeued_buffer)
            unregister_buffer( win, dequeue->buffer_id );
        pthread_mutex_unlock( &dispatch_ioctl_lock );
        if (cancel_ret) LOG( ERR, "failed reply buffer cancellation: %d\n", cancel_ret );
    }
    /* Successful SCM_RIGHTS transfer duplicated the descriptors for the client.
     * Failed sends retain ownership here, except a fence passed to cancellation. */
    close_ioctl_fds( &fds );
    if (dequeued_parent)
    {
        pAHardwareBuffer_release( dequeued_ahb );
        dequeued_parent->common.decRef( &dequeued_parent->common );
    }
    return ret == sizeof(result) + reply_size ? 0 : 1;
}

static int looper_handle_client( int fd, int events, void *data )
{
    for (;;)
    {
        int ret = (events & (ALOOPER_EVENT_HANGUP | ALOOPER_EVENT_ERROR)) ? 1 : handle_ioctl_message( looper_env, fd );

        if (!ret) continue;

        if (ret > 0)
        {
            pALooper_removeFd( looper, fd );
            close( fd );
            if (fd == desktop_client_fd) /* our explorer process died */
                _exit(0);
        }
        break;
    }

    return 1;
}

static int looper_handle_listen( int fd, int events, void *data )
{
    for (;;)
    {
        int client = accept4( fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK );

        if (client < 0)
        {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG( ERR,  "accept4 failed: %s\n", strerror( errno ) );
            break;
        }

        if (pALooper_addFd( looper, client, client, ALOOPER_EVENT_INPUT | ALOOPER_EVENT_HANGUP | ALOOPER_EVENT_ERROR, looper_handle_client, NULL ) != 1) {
            LOG( ERR, "Failed to add client to ALooper\n" );
            close( client );
        }
    }

    return 1;
}

/* main Wine initialisation */
void wine_init_jni( JNIEnv *env, jobject obj )
{
    int sockfd;

    java_object = (*env)->NewGlobalRef( env, obj );
    looper_env = env;
    if (!(looper = pALooper_forThread()))
    {
        LOG( ERR, "No looper for current thread\n");
        abort();
    }
    pALooper_acquire( looper );

    sockfd = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0 );
    if (sockfd < 0)
    {
        LOG( ERR,  "Failed to open server socket: %s\n", strerror( errno ) );
        abort();
    }

    if (bind( sockfd, (const struct sockaddr *)&ipc_addr, IPC_SOCKET_ADDR_LEN ) < 0 ||
        listen( sockfd, 32 ) < 0)
    {
        LOG( ERR, "Failed to bind server socket: %s\n", strerror( errno ) );
        close(sockfd);
        abort();
    }

    if (pALooper_addFd( looper, sockfd, sockfd, ALOOPER_EVENT_INPUT, looper_handle_listen, NULL ) != 1) {
        LOG( ERR, "Failed to add listening socket to main looper\n" );
        close(sockfd);
        abort();
    }
}


/* Client-side ioctl support */


/* Takes ownership of send_fence, including on connection or send failure. */
static int android_ioctl_fds( enum android_ioctl code, void *in, DWORD in_size, void *out, DWORD *out_size,
                              int send_fence, int *recv_buffer, int *recv_fence )
{
    static int device_fd = -1;
    static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;
    struct android_ioctl_reply result;
    int err = -ENOENT, received[2] = { -1, -1 };
    unsigned int count, expected, i = 0;
    ssize_t ret;
    char control[CMSG_SPACE(2 * sizeof(int))];
    struct iovec input[2] = { { &code, sizeof(code) }, { in, in_size } };
    struct iovec output[2] = { { &result, sizeof(result) }, { out, out_size ? *out_size : 0 } };
    struct msghdr request = { NULL, 0, input, 2, NULL, 0, 0 };
    struct msghdr reply = { NULL, 0, output, (out && out_size) ? 2 : 1, control, sizeof(control), 0 };
    struct cmsghdr *cmsg;

    pthread_mutex_lock( &device_mutex );
    if (recv_buffer) *recv_buffer = -1;
    if (recv_fence) *recv_fence = -1;

    if (device_fd == -1)
    {
        device_fd = socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0 );
        if (device_fd < 0) goto done;
        if (connect( device_fd, (const struct sockaddr *)&ipc_addr, IPC_SOCKET_ADDR_LEN ) < 0)
        {
            close( device_fd );
            device_fd = -1;
            goto done;
        }
    }

    if (send_fence != -1)
    {
        request.msg_control = control;
        request.msg_controllen = CMSG_SPACE(sizeof(send_fence));
        cmsg = CMSG_FIRSTHDR( &request );
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(send_fence));
        memcpy( CMSG_DATA(cmsg), &send_fence, sizeof(send_fence) );
    }
    if (code == IOCTL_QUEUE_BUFFER || code == IOCTL_CANCEL_BUFFER)
        TRACE_FENCE( FALSE, "request_send", device_fd, code, in, in_size, send_fence, 0, 0 );
    do ret = sendmsg( device_fd, &request, MSG_NOSIGNAL ); while (ret == -1 && errno == EINTR);
    if (code == IOCTL_QUEUE_BUFFER || code == IOCTL_CANCEL_BUFFER)
        TRACE_FENCE( FALSE, "request_sent", device_fd, code, in, in_size, send_fence, ret, 0 );
    if (send_fence != -1) close_fence( FALSE, send_fence );
    send_fence = -1;
    if (ret != sizeof(code) + in_size) goto disconnected;

    do ret = recvmsg( device_fd, &reply, MSG_CMSG_CLOEXEC ); while (ret == -1 && errno == EINTR);
    if (ret < 0) goto disconnected;
    count = get_message_fds( &reply, received, ARRAY_SIZE(received) );
    if (ret < sizeof(result)) goto disconnected;
    expected = !!(result.fd_mask & IOCTL_BUFFER_FD) + !!(result.fd_mask & IOCTL_FENCE_FD);
    if ((reply.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || count != expected ||
        (result.fd_mask & ~(IOCTL_BUFFER_FD | IOCTL_FENCE_FD)) ||
        ((result.fd_mask & IOCTL_BUFFER_FD) && !recv_buffer) ||
        ((result.fd_mask & IOCTL_FENCE_FD) && !recv_fence))
    {
        if (out_size) *out_size = 0; /* no complete native identity may be inferred */
        err = -EINVAL;
        goto done;
    }
    if (result.fd_mask & IOCTL_BUFFER_FD)
    {
        *recv_buffer = received[i];
        received[i++] = -1;
    }
    if (result.fd_mask & IOCTL_FENCE_FD)
    {
        *recv_fence = received[i];
        received[i++] = -1;
    }
    if (out && out_size) *out_size = ret - sizeof(result);
    if (code == IOCTL_DEQUEUE_BUFFER)
        TRACE_FENCE( FALSE, "reply_received", device_fd, code, out, out && out_size ? *out_size : 0,
                     recv_fence ? *recv_fence : -1, result.status, result.fd_mask );
    err = result.status;
    goto done;

disconnected:
    close( device_fd );
    device_fd = -1;
    WARN( "parent process is gone\n" );
    /* Release acquired descriptors before the existing process termination. */
    for (i = 0; i < ARRAY_SIZE(received); i++)
    {
        if (received[i] != -1) close( received[i] );
        received[i] = -1;
    }
    NtTerminateProcess( 0, 1 );
    err = -ENOENT;

done:
    if (send_fence != -1) close_fence( FALSE, send_fence );
    for (i = 0; i < ARRAY_SIZE(received); i++)
        if (received[i] != -1) close( received[i] );
    pthread_mutex_unlock( &device_mutex );
    return err;
}

static int android_ioctl( enum android_ioctl code, void *in, DWORD in_size, void *out, DWORD *out_size, int *recv_fd )
{
    return android_ioctl_fds( code, in, in_size, out, out_size, -1, recv_fd, NULL );
}

static void win_incRef( struct android_native_base_t *base )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)base;
    InterlockedIncrement( &win->ref );
}

static void win_decRef( struct android_native_base_t *base )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)base;
    InterlockedDecrement( &win->ref );
}

void createDesktopView( int *event_source )
{
    struct ioctl_android_create_desktop_view res = { 0 };
    res.log_flags = __wine_dbg_get_channel_flags(&__wine_dbch_android);
    android_ioctl( IOCTL_CREATE_DESKTOP_VIEW, &res, sizeof(res), NULL, NULL, event_source );
}

static int cancel_buffer( struct native_win_wrapper *win, int id, int generation, BOOL discard, int fence )
{
    struct ioctl_android_cancelBuffer cancel = { { HandleToLong(win->hwnd), win->opengl }, id, generation, discard };

    return android_ioctl_fds( IOCTL_CANCEL_BUFFER, &cancel, sizeof(cancel), NULL, NULL, fence, NULL, NULL );
}

static int dequeue_buffer( struct native_win_wrapper *win, struct ANativeWindowBuffer **buffer, int *fence,
                           struct ioctl_android_dequeueBuffer *identity )
{
    struct ioctl_android_dequeueBuffer res = {0};
    DWORD size = sizeof(res);
    int ret, buffer_fd = -1, acquire_fence = -1;

    res.hdr.hwnd = HandleToLong( win->hwnd );
    res.hdr.opengl = win->opengl;
    res.buffer_id = -1;
    res.generation = 0;

    ret = android_ioctl_fds( IOCTL_DEQUEUE_BUFFER, &res, size, &res, &size, -1, &buffer_fd, &acquire_fence );
    if (ret) goto failed;
    if (size != sizeof(res) || res.buffer_id < 0 || res.buffer_id >= NB_CACHED_BUFFERS ||
        res.hdr.hwnd != HandleToLong(win->hwnd) || res.hdr.opengl != win->opengl)
    {
        ret = -EINVAL;
        goto failed;
    }

    if (buffer_fd != -1)
    {
        AHardwareBuffer *ahb = NULL;

        ret = pAHardwareBuffer_recvHandleFromUnixSocket( buffer_fd, &ahb );
        if (TRACE_ON(android))
        {
            int saved_errno = errno;

            TRACE( "%08x opengl %u id %d generation %d ahb %p handle fd %d received %d\n",
                   res.hdr.hwnd, res.hdr.opengl, res.buffer_id, res.generation, ahb, buffer_fd, ret );
            errno = saved_errno;
        }
        close( buffer_fd );
        buffer_fd = -1;
        if (ret) goto failed;
        if (!ahb)
        {
            ret = -EINVAL;
            goto failed;
        }

        if (win->buffers[res.buffer_id].self)
            pAHardwareBuffer_release( win->buffers[res.buffer_id].self );

        win->buffers[res.buffer_id].self = ahb;
        win->buffers[res.buffer_id].buffer_id = res.buffer_id;
        win->buffers[res.buffer_id].generation = res.generation;
    }

    if (!win->buffers[res.buffer_id].self || win->buffers[res.buffer_id].generation != res.generation)
    {
        ret = -EINVAL;
        goto failed;
    }

    *buffer = anwb_from_ahb(win->buffers[res.buffer_id].self);
    *fence = acquire_fence;
    if (identity) *identity = res;

    TRACE( "hwnd %p, buffer %p id %d gen %d fence %d\n",
           win->hwnd, *buffer, res.buffer_id, res.generation, *fence );
    return 0;

failed:
    if (buffer_fd != -1) close( buffer_fd );
    if (size == sizeof(res) && res.buffer_id >= 0 && res.buffer_id < NB_CACHED_BUFFERS &&
        res.hdr.hwnd == HandleToLong(win->hwnd) && res.hdr.opengl == win->opengl)
    {
        int cancel_ret;

        /* The native dequeue succeeded, but no usable client buffer exists.
         * Forget its server cache entry so the next dequeue transfers it again. */
        cancel_ret = cancel_buffer( win, res.buffer_id, res.generation, TRUE, acquire_fence );
        acquire_fence = -1;
        if (cancel_ret) WARN( "hwnd %p failed to cancel undelivered buffer: %d\n", win->hwnd, cancel_ret );
    }
    if (acquire_fence != -1) close_fence( FALSE, acquire_fence );
    return ret;
}

static int dequeueBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer **buffer, int *fence )
{
    return dequeue_buffer( (struct native_win_wrapper *)window, buffer, fence, NULL );
}

static int cancelBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_cancelBuffer cancel = {0};

    TRACE( "hwnd %p buffer %p fence %d\n", win->hwnd, buffer, fence );

    if (!ahb_from_anwb( win, buffer, &cancel.buffer_id, &cancel.generation ))
    {
        if (fence != -1) close( fence );
        return -EINVAL;
    }

    cancel.hdr.hwnd = HandleToLong( win->hwnd );
    cancel.hdr.opengl = win->opengl;
    return android_ioctl_fds( IOCTL_CANCEL_BUFFER, &cancel, sizeof(cancel), NULL, NULL, fence, NULL, NULL );
}

static int queueBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_queueBuffer queue;

    TRACE( "hwnd %p buffer %p fence %d\n", win->hwnd, buffer, fence );

    if (!ahb_from_anwb( win, buffer, &queue.buffer_id, &queue.generation ))
    {
        if (fence != -1) close( fence );
        return -EINVAL;
    }

    queue.hdr.hwnd = HandleToLong( win->hwnd );
    queue.hdr.opengl = win->opengl;
    return android_ioctl_fds( IOCTL_QUEUE_BUFFER, &queue, sizeof(queue), NULL, NULL, fence, NULL, NULL );
}

static int dequeueBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer **buffer )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_dequeueBuffer identity;
    int fence, ret = dequeue_buffer( win, buffer, &fence, &identity );

    if (ret) return ret;
    if ((ret = wait_fence( fence )))
    {
        cancel_buffer( win, identity.buffer_id, identity.generation, FALSE, fence );
        *buffer = NULL;
    }
    else if (fence != -1) close_fence( FALSE, fence );
    return ret;
}

static int cancelBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return cancelBuffer( window, buffer, -1 );
}

static int lockBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return 0;  /* nothing to do */
}

static int queueBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return queueBuffer( window, buffer, -1 );
}

static int setSwapInterval( struct ANativeWindow *window, int interval )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_set_swap_interval swap;

    TRACE( "hwnd %p interval %d\n", win->hwnd, interval );
    swap.hdr.hwnd = HandleToLong( win->hwnd );
    swap.hdr.opengl = win->opengl;
    swap.interval = interval;
    return android_ioctl( IOCTL_SET_SWAP_INT, &swap, sizeof(swap), NULL, NULL, NULL );
}

static int query( const ANativeWindow *window, int what, int *value )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_query query;
    DWORD size = sizeof( query );
    int ret;

    query.hdr.hwnd = HandleToLong( win->hwnd );
    query.hdr.opengl = win->opengl;
    query.what = what;
    ret = android_ioctl( IOCTL_QUERY, &query, sizeof(query), &query, &size, NULL );
    TRACE( "hwnd %p what %d got %d -> %p\n", win->hwnd, what, query.value, value );
    if (!ret) *value = query.value;
    return ret;
}

static int perform( ANativeWindow *window, int operation, ... )
{
    static const char * const names[] =
    {
        "SET_USAGE", "CONNECT", "DISCONNECT", "SET_CROP", "SET_BUFFER_COUNT", "SET_BUFFERS_GEOMETRY",
        "SET_BUFFERS_TRANSFORM", "SET_BUFFERS_TIMESTAMP", "SET_BUFFERS_DIMENSIONS", "SET_BUFFERS_FORMAT",
        "SET_SCALING_MODE", "LOCK", "UNLOCK_AND_POST", "API_CONNECT", "API_DISCONNECT",
        "SET_BUFFERS_USER_DIMENSIONS", "SET_POST_TRANSFORM_CROP"
    };

    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_perform perf;
    va_list args;

    perf.hdr.hwnd  = HandleToLong( win->hwnd );
    perf.hdr.opengl = win->opengl;
    perf.operation = operation;
    memset( perf.args, 0, sizeof(perf.args) );

    va_start( args, operation );
    switch (operation)
    {
    case NATIVE_WINDOW_SET_USAGE:
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
    case NATIVE_WINDOW_SET_SCALING_MODE:
    case NATIVE_WINDOW_API_CONNECT:
    case NATIVE_WINDOW_API_DISCONNECT:
        perf.args[0] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %d\n", win->hwnd, names[operation], perf.args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        perf.args[0] = va_arg( args, size_t );
        TRACE( "hwnd %p %s count %d\n", win->hwnd, names[operation], perf.args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        perf.args[0] = va_arg( args, int );
        perf.args[1] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %dx%d\n", win->hwnd, names[operation], perf.args[0], perf.args[1] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        perf.args[0] = va_arg( args, int );
        perf.args[1] = va_arg( args, int );
        perf.args[2] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %dx%d %d\n", win->hwnd, names[operation],
               perf.args[0], perf.args[1], perf.args[2] );
        break;
    case NATIVE_WINDOW_SET_CROP:
    {
        android_native_rect_t *rect = va_arg( args, android_native_rect_t * );
        perf.args[0] = rect->left;
        perf.args[1] = rect->top;
        perf.args[2] = rect->right;
        perf.args[3] = rect->bottom;
        TRACE( "hwnd %p %s rect %d,%d-%d,%d\n", win->hwnd, names[operation],
               perf.args[0], perf.args[1], perf.args[2], perf.args[3] );
        break;
    }
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
    {
        int64_t timestamp = va_arg( args, int64_t );
        perf.args[0] = timestamp;
        perf.args[1] = timestamp >> 32;
        TRACE( "hwnd %p %s arg %08x%08x\n", win->hwnd, names[operation], perf.args[1], perf.args[0] );
        break;
    }
    case NATIVE_WINDOW_LOCK:
    {
        struct ANativeWindowBuffer *buffer = NULL;
        struct ioctl_android_dequeueBuffer identity;
        struct ANativeWindow_Buffer *buffer_ret = va_arg( args, ANativeWindow_Buffer * );
        struct AHardwareBuffer *b = NULL;
        ARect *bounds = va_arg( args, ARect * );
        int fence = -1, ret, cancel_ret;

        buffer_ret->bits = NULL;
        ret = dequeue_buffer( win, &buffer, &fence, &identity );
        if (!ret)
        {
            b = ahb_from_anwb( win, buffer, NULL, NULL );
            if (!b) ret = -EINVAL;
            else if (!(ret = wait_fence( fence )))
            {
                ret = pAHardwareBuffer_lock( b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                             AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL,
                                             &buffer_ret->bits );
                TRACE_FENCE( FALSE, "cpu_locked", -1, IOCTL_DEQUEUE_BUFFER, &identity, sizeof(identity),
                             fence, ret, 0 );
            }
            if (ret)
            {
                /* This is the identity of the successful native dequeue,
                 * independent of a failed AHB conversion/lock.  Cancellation
                 * owns the original fence, including when its wait failed. */
                cancel_ret = cancel_buffer( win, identity.buffer_id, identity.generation, FALSE, fence );
                fence = -1;
                if (cancel_ret) WARN( "hwnd %p failed lock cancellation: %d\n", win->hwnd, cancel_ret );
            }
        }
        if (fence != -1) close_fence( FALSE, fence );
        if (!ret)
        {
            AHardwareBuffer_Desc d = {0};
            pAHardwareBuffer_describe(b, &d);
            buffer_ret->width  = d.width;
            buffer_ret->height = d.height;
            buffer_ret->stride = d.stride;
            buffer_ret->format = d.format;
            win->locked_buffer = b;
            if (bounds)
            {
                bounds->left   = 0;
                bounds->top    = 0;
                bounds->right  = d.width;
                bounds->bottom = d.height;
            }
        }
        va_end( args );
        TRACE( "hwnd %p %s bits %p ret %d %s\n", win->hwnd, names[operation], buffer_ret->bits, ret, strerror(-ret) );
        return ret;
    }
    case NATIVE_WINDOW_UNLOCK_AND_POST:
    {
        int ret = -EINVAL;
        if (win->locked_buffer)
        {
            pAHardwareBuffer_unlock(win->locked_buffer, NULL);
            ret = window->queueBuffer( window, anwb_from_ahb(win->locked_buffer), -1 );
            win->locked_buffer = NULL;
        }
        va_end( args );
        TRACE( "hwnd %p %s ret %d\n", win->hwnd, names[operation], ret );
        return ret;
    }
    case NATIVE_WINDOW_CONNECT:
    case NATIVE_WINDOW_DISCONNECT:
        TRACE( "hwnd %p %s\n", win->hwnd, names[operation] );
        break;
    case NATIVE_WINDOW_SET_POST_TRANSFORM_CROP:
    default:
        FIXME( "unsupported perform hwnd %p op %d %s\n", win->hwnd, operation,
               operation < ARRAY_SIZE( names ) ? names[operation] : "???" );
        break;
    }
    va_end( args );
    return android_ioctl( IOCTL_PERFORM, &perf, sizeof(perf), NULL, NULL, NULL );
}

struct ANativeWindow *create_ioctl_window( HWND hwnd, BOOL opengl )
{
    struct ioctl_android_create_window req;
    struct native_win_wrapper *win = calloc( 1, sizeof(*win) );

    if (!win) return NULL;

    win->win.common.magic             = ANDROID_NATIVE_WINDOW_MAGIC;
    win->win.common.version           = sizeof(ANativeWindow);
    win->win.common.incRef            = win_incRef;
    win->win.common.decRef            = win_decRef;
    win->win.setSwapInterval          = setSwapInterval;
    win->win.dequeueBuffer_DEPRECATED = dequeueBuffer_DEPRECATED;
    win->win.lockBuffer_DEPRECATED    = lockBuffer_DEPRECATED;
    win->win.queueBuffer_DEPRECATED   = queueBuffer_DEPRECATED;
    win->win.query                    = query;
    win->win.perform                  = perform;
    win->win.cancelBuffer_DEPRECATED  = cancelBuffer_DEPRECATED;
    win->win.dequeueBuffer            = dequeueBuffer;
    win->win.queueBuffer              = queueBuffer;
    win->win.cancelBuffer             = cancelBuffer;
    win->ref  = 1;
    win->hwnd = hwnd;
    win->opengl = opengl;
    TRACE( "-> %p %p opengl=%u\n", win, win->hwnd, opengl );

    req.hdr.hwnd = HandleToLong( win->hwnd );
    req.hdr.opengl = win->opengl;
    req.parent = get_ioctl_win_parent( NtUserGetAncestor( hwnd, GA_PARENT ));
    req.is_desktop = hwnd == desktop_window;
    android_ioctl( IOCTL_CREATE_WINDOW, &req, sizeof(req), NULL, NULL, NULL );

    return &win->win;
}

struct ANativeWindow *grab_ioctl_window( struct ANativeWindow *window )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    InterlockedIncrement( &win->ref );
    return window;
}

void release_ioctl_window( struct ANativeWindow *window )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    unsigned int i;

    if (InterlockedDecrement( &win->ref ) > 0) return;

    TRACE( "%p %p\n", win, win->hwnd );
    for (i = 0; i < ARRAY_SIZE( win->buffers ); i++)
        if (win->buffers[i].self) pAHardwareBuffer_release(win->buffers[i].self);

    destroy_ioctl_window( win->hwnd, win->opengl );
    free( win );
}

void destroy_ioctl_window( HWND hwnd, BOOL opengl )
{
    struct ioctl_android_destroy_window req;

    req.hdr.hwnd = HandleToLong( hwnd );
    req.hdr.opengl = opengl;
    android_ioctl( IOCTL_DESTROY_WINDOW, &req, sizeof(req), NULL, NULL, NULL );
}

int ioctl_window_pos_changed( HWND hwnd, const struct window_rects *rects,
                              UINT style, UINT flags, HWND after, HWND owner )
{
    struct ioctl_android_window_pos_changed req;

    req.hdr.hwnd     = HandleToLong( hwnd );
    req.hdr.opengl   = FALSE;
    req.window_rect  = rects->window;
    req.client_rect  = rects->client;
    req.visible_rect = rects->visible;
    req.style        = style;
    req.flags        = flags;
    req.after        = HandleToLong( after );
    req.owner        = HandleToLong( owner );
    return android_ioctl( IOCTL_WINDOW_POS_CHANGED, &req, sizeof(req), NULL, NULL, NULL );
}

int ioctl_set_window_parent( HWND hwnd, HWND parent )
{
    struct ioctl_android_set_window_parent req;

    req.hdr.hwnd = HandleToLong( hwnd );
    req.hdr.opengl = FALSE;
    req.parent = get_ioctl_win_parent( parent );
    return android_ioctl( IOCTL_SET_WINDOW_PARENT, &req, sizeof(req), NULL, NULL, NULL );
}

int ioctl_set_capture( HWND hwnd )
{
    struct ioctl_android_set_capture req;

    req.hdr.hwnd  = HandleToLong( hwnd );
    req.hdr.opengl = FALSE;
    return android_ioctl( IOCTL_SET_CAPTURE, &req, sizeof(req), NULL, NULL, NULL );
}

int ioctl_set_cursor( int id, int width, int height,
                      int hotspotx, int hotspoty, const unsigned int *bits )
{
    struct ioctl_android_set_cursor *req;
    unsigned int size = offsetof( struct ioctl_android_set_cursor, bits[width * height] );
    int ret;

    if (!(req = malloc( size ))) return -ENOMEM;
    req->hdr.hwnd   = 0;  /* unused */
    req->hdr.opengl = FALSE;
    req->id       = id;
    req->width    = width;
    req->height   = height;
    req->hotspotx = hotspotx;
    req->hotspoty = hotspoty;
    memcpy( req->bits, bits, width * height * sizeof(req->bits[0]) );
    ret = android_ioctl( IOCTL_SET_CURSOR, req, size, NULL, NULL, NULL );
    free( req );
    return ret;
}

/**********************************************************************
 *           ANDROID_SetDesktopWindow
 */
void ANDROID_SetDesktopWindow( HWND hwnd )
{
    desktop_window = hwnd;
}
