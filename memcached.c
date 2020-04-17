/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *  memcached - memory caching daemon
 *
 *       https://www.memcached.org/
 *
 *  Copyright 2003 Danga Interactive, Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Anatoly Vorobey <mellon@pobox.com>
 *      Brad Fitzpatrick <brad@danga.com>
 */
#include "memcached.h"
#ifdef EXTSTORE
#include "storage.h"
#endif
#include "authfile.h"
#include "restart.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <ctype.h>
#include <stdarg.h>

/* some POSIX systems need the following definition
 * to get mlockall flags out of sys/mman.h.  */
#ifndef _P1003_1B_VISIBLE
#define _P1003_1B_VISIBLE
#endif
#include <pwd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <sysexits.h>
#include <stddef.h>

#ifdef HAVE_GETOPT_LONG
#include <getopt.h>
#endif

#ifdef TLS
#include "tls.h"
#endif

#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

#ifdef SFD_VALUE_IS_RANDOM
#include "conn_list.h"
#endif /* #ifdef SFD_VALUE_IS_RANDOM */

/*
 * forward declarations
 */
static void drive_machine(conn *c);
static int new_socket(struct addrinfo *ai);
static ssize_t tcp_read(conn *arg, void *buf, size_t count);
static ssize_t tcp_sendmsg(conn *arg, struct msghdr *msg, int flags);
static ssize_t tcp_write(conn *arg, void *buf, size_t count);

enum try_read_result {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,            /** an error occurred (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR      /** failed to allocate more memory */
};

static int try_read_command_negotiate(conn *c);
static int try_read_command_udp(conn *c);
static int try_read_command_binary(conn *c);
static int try_read_command_ascii(conn *c);
static int try_read_command_asciiauth(conn *c);

static enum try_read_result try_read_network(conn *c);
static enum try_read_result try_read_udp(conn *c);

static void conn_set_state(conn *c, enum conn_states state);
static int start_conn_timeout_thread();

static mc_resp* resp_finish(conn *c, mc_resp *resp);

/* stats */
static void stats_init(void);
static void server_stats(ADD_STAT add_stats, conn *c);
static void process_stat_settings(ADD_STAT add_stats, void *c);
static void conn_to_str(const conn *c, char *addr, char *svr_addr);

/** Return a datum for stats in binary protocol */
static bool get_stats(const char *stat_type, int nkey, ADD_STAT add_stats, void *c);

/* defaults */
static void settings_init(void);

/* event handling, network IO */
static void event_handler(const evutil_socket_t fd, const short which, void *arg);
static void conn_close(conn *c);
static void conn_init(void);
static bool update_event(conn *c, const int new_flags);
static void complete_nread(conn *c);
static void process_command(conn *c, char *command);
static void write_and_free(conn *c, char *buf, int bytes);
static void write_bin_error(conn *c, protocol_binary_response_status err,
                            const char *errstr, int swallow);
static void write_bin_miss_response(conn *c, char *key, size_t nkey);

#ifdef EXTSTORE
static void _get_extstore_cb(void *e, obj_io *io, int ret);
static inline int _get_extstore(conn *c, item *it, mc_resp *resp);
#endif
static void conn_free(conn *c);

/** binprot handlers **/
static void process_bin_flush(conn *c, char *extbuf);
static void process_bin_append_prepend(conn *c);
static void process_bin_update(conn *c, char *extbuf);
static void process_bin_get_or_touch(conn *c, char *extbuf);
static void process_bin_delete(conn *c);
static void complete_incr_bin(conn *c, char *extbuf);
static void process_bin_stat(conn *c);
static void process_bin_sasl_auth(conn *c);

/** exported globals **/
struct stats stats;
struct stats_state stats_state;
struct settings settings;
time_t process_started;     /* when the process was started */
conn **conns;

struct slab_rebalance slab_rebal;
volatile int slab_rebalance_signal;
#ifdef EXTSTORE
/* hoping this is temporary; I'd prefer to cut globals, but will complete this
 * battle another day.
 */
void *ext_storage = NULL;
#endif
/** file scope variables **/
static conn *listen_conn = NULL;
static int max_fds;
static struct event_base *main_base;

enum transmit_result {
    TRANSMIT_COMPLETE,   /** All done writing. */
    TRANSMIT_INCOMPLETE, /** More data remaining to write. */
    TRANSMIT_SOFT_ERROR, /** Can't write any more right now. */
    TRANSMIT_HARD_ERROR  /** Can't write (c->state is set to conn_closing) */
};

/* Default methods to read from/ write to a socket */
ssize_t tcp_read(conn *c, void *buf, size_t count) {
    assert (c != NULL);
    return sock_read(c->sfd, buf, count);
}

ssize_t tcp_sendmsg(conn *c, struct msghdr *msg, int flags) {
    assert (c != NULL);
    return sendmsg(c->sfd, msg, flags);
}

ssize_t tcp_write(conn *c, void *buf, size_t count) {
    assert (c != NULL);
    return sock_write(c->sfd, buf, count);
}

static enum transmit_result transmit(conn *c);

/* This reduces the latency without adding lots of extra wiring to be able to
 * notify the listener thread of when to listen again.
 * Also, the clock timer could be broken out into its own thread and we
 * can block the listener via a condition.
 */
static volatile bool allow_new_conns = true;
static bool stop_main_loop = false;
static struct event maxconnsevent;
static void maxconns_handler(const evutil_socket_t fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 0, .tv_usec = 10000};

    if (fd == -42 || allow_new_conns == false) {
        /* reschedule in 10ms if we need to keep polling */
        evtimer_set(&maxconnsevent, maxconns_handler, 0);
        event_base_set(main_base, &maxconnsevent);
        evtimer_add(&maxconnsevent, &t);
    } else {
        evtimer_del(&maxconnsevent);
        accept_new_conns(true);
    }
}

#define REALTIME_MAXDELTA 60*60*24*30

/* Negative exptimes can underflow and end up immortal. realtime() will
   immediately expire values that are greater than REALTIME_MAXDELTA, but less
   than process_started, so lets aim for that. */
#define EXPTIME_TO_POSITIVE_TIME(exptime) (exptime < 0) ? \
        REALTIME_MAXDELTA + 1 : exptime


/*
 * given time value that's either unix time or delta from current unix time, return
 * unix time. Use the fact that delta can't exceed one month (and real time value can't
 * be that low).
 */
static rel_time_t realtime(const time_t exptime) {
    /* no. of seconds in 30 days - largest possible delta exptime */

    if (exptime == 0) return 0; /* 0 means never expire */

    if (exptime > REALTIME_MAXDELTA) {
        /* if item expiration is at/before the server started, give it an
           expiration time of 1 second after the server started.
           (because 0 means don't expire).  without this, we'd
           underflow and wrap around to some large value way in the
           future, effectively making items expiring in the past
           really expiring never */
        if (exptime <= process_started)
            return (rel_time_t)1;
        return (rel_time_t)(exptime - process_started);
    } else {
        return (rel_time_t)(exptime + current_time);
    }
}

static void stats_init(void) {
    memset(&stats, 0, sizeof(struct stats));
    memset(&stats_state, 0, sizeof(struct stats_state));
    stats_state.accepting_conns = true; /* assuming we start in this state. */

    /* make the time we started always be 2 seconds before we really
       did, so time(0) - time.started is never zero.  if so, things
       like 'settings.oldest_live' which act as booleans as well as
       values are now false in boolean context... */
    process_started = time(0) - ITEM_UPDATE_INTERVAL - 2;
    stats_prefix_init(settings.prefix_delimiter);
}

static void stats_reset(void) {
    STATS_LOCK();
    memset(&stats, 0, sizeof(struct stats));
    stats_prefix_clear();
    STATS_UNLOCK();
    threadlocal_stats_reset();
    item_stats_reset();
}

static void settings_init(void) {
    settings.use_cas = true;
    settings.access = 0700;
    settings.port = 11211;
    settings.udpport = 0;
#ifdef TLS
    settings.ssl_enabled = false;
    settings.ssl_ctx = NULL;
    settings.ssl_chain_cert = NULL;
    settings.ssl_key = NULL;
    settings.ssl_verify_mode = SSL_VERIFY_NONE;
    settings.ssl_keyformat = SSL_FILETYPE_PEM;
    settings.ssl_ciphers = NULL;
    settings.ssl_ca_cert = NULL;
    settings.ssl_last_cert_refresh_time = current_time;
    settings.ssl_wbuf_size = 16 * 1024; // default is 16KB (SSL max frame size is 17KB)
    settings.ssl_session_cache = false;
#endif
    /* By default this string should be NULL for getaddrinfo() */
    settings.inter = NULL;
    settings.maxbytes = 64 * 1024 * 1024; /* default is 64MB */
    settings.maxconns = 1024;         /* to limit connections-related memory to about 5MB */
    settings.verbose = 0;
    settings.oldest_live = 0;
    settings.oldest_cas = 0;          /* supplements accuracy of oldest_live */
    settings.evict_to_free = 1;       /* push old items out of cache when memory runs out */
    settings.socketpath = NULL;       /* by default, not using a unix socket */
    settings.auth_file = NULL;        /* by default, not using ASCII authentication tokens */
    settings.factor = 1.25;
    settings.chunk_size = 48;         /* space for a modest key and value */
    settings.num_threads = 4;         /* N workers */
    settings.num_threads_per_udp = 0;
    settings.prefix_delimiter = ':';
    settings.detail_enabled = 0;
    settings.reqs_per_event = 20;
    settings.backlog = 1024;
    settings.binding_protocol = negotiating_prot;
    settings.item_size_max = 1024 * 1024; /* The famous 1MB upper limit. */
    settings.slab_page_size = 1024 * 1024; /* chunks are split from 1MB pages. */
    settings.slab_chunk_size_max = settings.slab_page_size / 2;
    settings.sasl = false;
    settings.maxconns_fast = true;
    settings.lru_crawler = false;
    settings.lru_crawler_sleep = 100;
    settings.lru_crawler_tocrawl = 0;
    settings.lru_maintainer_thread = false;
    settings.lru_segmented = true;
    settings.hot_lru_pct = 20;
    settings.warm_lru_pct = 40;
    settings.hot_max_factor = 0.2;
    settings.warm_max_factor = 2.0;
    settings.temp_lru = false;
    settings.temporary_ttl = 61;
    settings.idle_timeout = 0; /* disabled */
    settings.hashpower_init = 0;
    settings.slab_reassign = true;
    settings.slab_automove = 1;
    settings.slab_automove_ratio = 0.8;
    settings.slab_automove_window = 30;
    settings.shutdown_command = false;
    settings.tail_repair_time = TAIL_REPAIR_TIME_DEFAULT;
    settings.flush_enabled = true;
    settings.dump_enabled = true;
    settings.crawls_persleep = 1000;
    settings.logger_watcher_buf_size = LOGGER_WATCHER_BUF_SIZE;
    settings.logger_buf_size = LOGGER_BUF_SIZE;
    settings.drop_privileges = false;
    settings.watch_enabled = true;
    settings.resp_obj_mem_limit = 0;
    settings.read_buf_mem_limit = 0;
#ifdef MEMCACHED_DEBUG
    settings.relaxed_privileges = false;
#endif
}

extern pthread_mutex_t conn_lock;

/* Connection timeout thread bits */
static pthread_t conn_timeout_tid;
static int do_run_conn_timeout_thread;

#define CONNS_PER_SLICE 100
#define TIMEOUT_MSG_SIZE (1 + sizeof(int))
static void *conn_timeout_thread(void *arg) {
    int i;
    conn *c;
    char buf[TIMEOUT_MSG_SIZE];
    rel_time_t oldest_last_cmd;
    int sleep_time;
    int sleep_slice = max_fds / CONNS_PER_SLICE;
    if (sleep_slice == 0)
        sleep_slice = CONNS_PER_SLICE;

    useconds_t timeslice = 1000000 / sleep_slice;

    while(do_run_conn_timeout_thread) {
        if (settings.verbose > 2)
            fprintf(stderr, "idle timeout thread at top of connection list\n");

        oldest_last_cmd = current_time;

        for (i = 0; i < max_fds; i++) {
            if ((i % CONNS_PER_SLICE) == 0) {
                if (settings.verbose > 2)
                    fprintf(stderr, "idle timeout thread sleeping for %ulus\n",
                        (unsigned int)timeslice);
                usleep(timeslice);
            }

            if (!conns[i])
                continue;

            c = conns[i];

            if (!IS_TCP(c->transport))
                continue;

            if (c->state != conn_new_cmd && c->state != conn_read)
                continue;

            if ((current_time - c->last_cmd_time) > settings.idle_timeout) {
                buf[0] = 't';
                memcpy(&buf[1], &i, sizeof(int));
                if (pipe_write(c->thread->notify_send_fd, buf, TIMEOUT_MSG_SIZE)
                    != TIMEOUT_MSG_SIZE)
                    perror("Failed to write timeout to notify pipe");
            } else {
                if (c->last_cmd_time < oldest_last_cmd)
                    oldest_last_cmd = c->last_cmd_time;
            }
        }

        /* This is the soonest we could have another connection time out */
        sleep_time = settings.idle_timeout - (current_time - oldest_last_cmd) + 1;
        if (sleep_time <= 0)
            sleep_time = 1;

        if (settings.verbose > 2)
            fprintf(stderr,
                    "idle timeout thread finished pass, sleeping for %ds\n",
                    sleep_time);
        usleep((useconds_t) sleep_time * 1000000);
    }

    return NULL;
}

static int start_conn_timeout_thread() {
    int ret;

    if (settings.idle_timeout == 0)
        return -1;

    do_run_conn_timeout_thread = 1;
    if ((ret = pthread_create(&conn_timeout_tid, NULL,
        conn_timeout_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create idle connection timeout thread: %s\n",
            strerror(ret));
        return -1;
    }

    return 0;
}

int stop_conn_timeout_thread(void) {
    if (!do_run_conn_timeout_thread)
        return -1;
    do_run_conn_timeout_thread = 0;
    pthread_join(conn_timeout_tid, NULL);
    return 0;
}

/*
 * read buffer cache helper functions
 */
static void rbuf_release(conn *c) {
    if (c->rbuf != NULL && c->rbytes == 0 && !IS_UDP(c->transport)) {
        if (c->rbuf_malloced) {
            free(c->rbuf);
            c->rbuf_malloced = false;
        } else {
            do_cache_free(c->thread->rbuf_cache, c->rbuf);
        }
        c->rsize = 0;
        c->rbuf = NULL;
        c->rcurr = NULL;
    }
}

static bool rbuf_alloc(conn *c) {
    if (c->rbuf == NULL) {
        c->rbuf = do_cache_alloc(c->thread->rbuf_cache);
        if (!c->rbuf) {
            THR_STATS_LOCK(c);
            c->thread->stats.read_buf_oom++;
            THR_STATS_UNLOCK(c);
            return false;
        }
        c->rsize = READ_BUFFER_SIZE;
        c->rcurr = c->rbuf;
    }
    return true;
}

// Just for handling huge ASCII multigets.
// The previous system was essentially the same; realloc'ing until big enough,
// then realloc'ing back down after the request finished.
static bool rbuf_switch_to_malloc(conn *c) {
    // Might as well start with x2 and work from there.
    size_t size = c->rsize * 2;
    char *tmp = malloc(size);
    if (!tmp)
        return false;

    do_cache_free(c->thread->rbuf_cache, c->rbuf);
    memcpy(tmp, c->rcurr, c->rbytes);

    c->rcurr = c->rbuf = tmp;
    c->rsize = size;
    c->rbuf_malloced = true;
    return true;
}

/*
 * Initializes the connections array. We don't actually allocate connection
 * structures until they're needed, so as to avoid wasting memory when the
 * maximum connection count is much higher than the actual number of
 * connections.
 *
 * This does end up wasting a few pointers' worth of memory for FDs that are
 * used for things other than connections, but that's worth it in exchange for
 * being able to directly index the conns array by FD.
 */
static void conn_init(void) {
#ifndef SFD_VALUE_IS_RANDOM
    /* We're unlikely to see an FD much higher than maxconns. */
    int next_fd = dup(1);
    if (next_fd < 0) {
        perror("Failed to duplicate file descriptor\n");
        exit(1);
    }
    int headroom = 10;      /* account for extra unexpected open FDs */

    max_fds = settings.maxconns + headroom + next_fd;

    close(next_fd);
#else
    max_fds = settings.maxconns;
#endif /* #ifndef SFD_VALUE_IS_RANDOM */

#ifndef DISABLE_RLIMIT_NOFILE
    struct rlimit rl;

    /* But if possible, get the actual highest FD we can possibly ever see. */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        max_fds = rl.rlim_max;
    } else {
        fprintf(stderr, "Failed to query maximum file descriptor; "
                        "falling back to maxconns\n");
    }
#endif /* #ifndef DISABLE_RLIMIT_NOFILE */

    if ((conns = calloc(max_fds, sizeof(conn *))) == NULL) {
        fprintf(stderr, "Failed to allocate connection structures\n");
        /* This is unrecoverable so bail out early. */
        exit(1);
    }
#ifdef SFD_VALUE_IS_RANDOM
    conn_list_init(max_fds);
#endif /* #ifdef SFD_VALUE_IS_RANDOM */
}

static const char *prot_text(enum protocol prot) {
    char *rv = "unknown";
    switch(prot) {
        case ascii_prot:
            rv = "ascii";
            break;
        case binary_prot:
            rv = "binary";
            break;
        case negotiating_prot:
            rv = "auto-negotiate";
            break;
    }
    return rv;
}

void conn_close_idle(conn *c) {
    if (settings.idle_timeout > 0 &&
        (current_time - c->last_cmd_time) > settings.idle_timeout) {
        if (c->state != conn_new_cmd && c->state != conn_read) {
            if (settings.verbose > 1)
                fprintf(stderr,
                    "fd %d wants to timeout, but isn't in read state", c->sfd);
            return;
        }

        if (settings.verbose > 1)
            fprintf(stderr, "Closing idle fd %d\n", c->sfd);

        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.idle_kicks++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        conn_set_state(c, conn_closing);
        drive_machine(c);
    }
}

/* bring conn back from a sidethread. could have had its event base moved. */
void conn_worker_readd(conn *c) {
    c->ev_flags = EV_READ | EV_PERSIST;
    event_set(&c->event, c->sfd, c->ev_flags, event_handler, (void *)c);
    event_base_set(c->thread->base, &c->event);

    // TODO: call conn_cleanup/fail/etc
    if (event_add(&c->event, 0) == -1) {
        perror("event_add");
    }

    // side thread wanted us to close immediately.
    if (c->state == conn_closing) {
        drive_machine(c);
        return;
    }
    c->state = conn_new_cmd;

#ifdef EXTSTORE
    // If we had IO objects, process
    if (c->io_wraplist) {
        //assert(c->io_wrapleft == 0); // assert no more to process
        conn_set_state(c, conn_mwrite);
        drive_machine(c);
    }
#endif
}

conn *conn_new(const int sfd, enum conn_states init_state,
                const int event_flags,
                const int read_buffer_size, enum network_transport transport,
                struct event_base *base, void *ssl) {
    conn *c = NULL;

#ifndef SFD_VALUE_IS_RANDOM
    assert(sfd >= 0 && sfd < max_fds);
    c = conns[sfd];
#endif /* #ifndef SFD_VALUE_IS_RANDOM */

    if (NULL == c) {
#ifndef SFD_VALUE_IS_RANDOM
        c = (conn *)calloc(1, sizeof(conn));
#else
        c = conn_list_open(sfd);
#endif /* #ifndef SFD_VALUE_IS_RANDOM */
        if (!c) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            fprintf(stderr, "Failed to allocate connection object\n");
            return NULL;
        }
        MEMCACHED_CONN_CREATE(c);
        c->read = NULL;
        c->sendmsg = NULL;
        c->write = NULL;
        c->rbuf = NULL;

        c->rsize = read_buffer_size;

        // UDP connections use a persistent static buffer.
        if (c->rsize) {
            c->rbuf = (char *)malloc((size_t)c->rsize);
        }

        if (c->rsize && c->rbuf == NULL) {
            conn_free(c);
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            fprintf(stderr, "Failed to allocate buffers for connection\n");
            return NULL;
        }

#ifndef SFD_VALUE_IS_RANDOM
        STATS_LOCK();
        stats_state.conn_structs++;
        STATS_UNLOCK();

        c->sfd = sfd;
        conns[sfd] = c;
#endif /* #ifdef SFD_VALUE_IS_RANDOM */
    }

    c->transport = transport;
    c->protocol = settings.binding_protocol;

    /* unix socket mode doesn't need this, so zeroed out.  but why
     * is this done for every command?  presumably for UDP
     * mode.  */
    if (!settings.socketpath) {
        c->request_addr_size = sizeof(c->request_addr);
    } else {
        c->request_addr_size = 0;
    }

    if (transport == tcp_transport && init_state == conn_new_cmd) {
        if (getpeername(sfd, (struct sockaddr *) &c->request_addr,
                        &c->request_addr_size)) {
            perror("getpeername");
            memset(&c->request_addr, 0, sizeof(c->request_addr));
        }
    }

    if (settings.verbose > 1) {
        if (init_state == conn_listening) {
            fprintf(stderr, "<%d server listening (%s)\n", sfd,
                prot_text(c->protocol));
        } else if (IS_UDP(transport)) {
            fprintf(stderr, "<%d server listening (udp)\n", sfd);
        } else if (c->protocol == negotiating_prot) {
            fprintf(stderr, "<%d new auto-negotiating client connection\n",
                    sfd);
        } else if (c->protocol == ascii_prot) {
            fprintf(stderr, "<%d new ascii client connection.\n", sfd);
        } else if (c->protocol == binary_prot) {
            fprintf(stderr, "<%d new binary client connection.\n", sfd);
        } else {
            fprintf(stderr, "<%d new unknown (%d) client connection\n",
                sfd, c->protocol);
            assert(false);
        }
    }

#ifdef TLS
    c->ssl = NULL;
    c->ssl_wbuf = NULL;
    c->ssl_enabled = false;
#endif
    c->state = init_state;
    c->rlbytes = 0;
    c->cmd = -1;
    c->rbytes = 0;
    c->rcurr = c->rbuf;
    c->ritem = 0;
    c->rbuf_malloced = false;
    c->sasl_started = false;
    c->set_stale = false;
    c->mset_res = false;
    c->close_after_write = false;
    c->last_cmd_time = current_time; /* initialize for idle kicker */
#ifdef EXTSTORE
    c->io_wraplist = NULL;
    c->io_wrapleft = 0;
#endif

    c->item = 0;

    c->noreply = false;

#ifdef TLS
    if (ssl) {
        c->ssl = (SSL*)ssl;
        c->read = ssl_read;
        c->sendmsg = ssl_sendmsg;
        c->write = ssl_write;
        c->ssl_enabled = true;
#ifndef OPENSSL_IS_BORINGSSL
        SSL_set_info_callback(c->ssl, ssl_callback);
#endif /* #ifndef OPENSSL_IS_BORINGSSL */
    } else
#else
    // This must be NULL if TLS is not enabled.
    assert(ssl == NULL);
#endif
    {
        c->read = tcp_read;
        c->sendmsg = tcp_sendmsg;
        c->write = tcp_write;
    }

    if (IS_UDP(transport)) {
        c->try_read_command = try_read_command_udp;
    } else {
        switch (c->protocol) {
            case ascii_prot:
                if (settings.auth_file == NULL) {
                    c->authenticated = true;
                    c->try_read_command = try_read_command_ascii;
                } else {
                    c->authenticated = false;
                    c->try_read_command = try_read_command_asciiauth;
                }
                break;
            case binary_prot:
                // binprot handles its own authentication via SASL parsing.
                c->authenticated = false;
                c->try_read_command = try_read_command_binary;
                break;
            case negotiating_prot:
                c->try_read_command = try_read_command_negotiate;
                break;
        }
    }

    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = event_flags;

    if (event_add(&c->event, 0) == -1) {
        perror("event_add");
        return NULL;
    }

    STATS_LOCK();
    stats_state.curr_conns++;
    stats.total_conns++;
    STATS_UNLOCK();

    MEMCACHED_CONN_ALLOCATE(c->sfd);

    return c;
}
#ifdef EXTSTORE
static void recache_or_free(conn *c, io_wrap *wrap) {
    item *it;
    it = (item *)wrap->io.buf;
    bool do_free = true;
    if (wrap->active) {
        // If request never dispatched, free the read buffer but leave the
        // item header alone.
        do_free = false;
        size_t ntotal = ITEM_ntotal(wrap->hdr_it);
        slabs_free(it, ntotal, slabs_clsid(ntotal));
        c->io_wrapleft--;
        assert(c->io_wrapleft >= 0);
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.get_aborted_extstore++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    } else if (wrap->miss) {
        // If request was ultimately a miss, unlink the header.
        do_free = false;
        size_t ntotal = ITEM_ntotal(wrap->hdr_it);
        item_unlink(wrap->hdr_it);
        slabs_free(it, ntotal, slabs_clsid(ntotal));
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.miss_from_extstore++;
        if (wrap->badcrc)
            c->thread->stats.badcrc_from_extstore++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    } else if (settings.ext_recache_rate) {
        // hashvalue is cuddled during store
        uint32_t hv = (uint32_t)it->time;
        // opt to throw away rather than wait on a lock.
        void *hold_lock = item_trylock(hv);
        if (hold_lock != NULL) {
            item *h_it = wrap->hdr_it;
            uint8_t flags = ITEM_LINKED|ITEM_FETCHED|ITEM_ACTIVE;
            // Item must be recently hit at least twice to recache.
            if (((h_it->it_flags & flags) == flags) &&
                    h_it->time > current_time - ITEM_UPDATE_INTERVAL &&
                    c->recache_counter++ % settings.ext_recache_rate == 0) {
                do_free = false;
                // In case it's been updated.
                it->exptime = h_it->exptime;
                it->it_flags &= ~ITEM_LINKED;
                it->refcount = 0;
                it->h_next = NULL; // might not be necessary.
                STORAGE_delete(c->thread->storage, h_it);
                item_replace(h_it, it, hv);
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.recache_from_extstore++;
                pthread_mutex_unlock(&c->thread->stats.mutex);
            }
        }
        if (hold_lock)
            item_trylock_unlock(hold_lock);
    }
    if (do_free)
        slabs_free(it, ITEM_ntotal(it), ITEM_clsid(it));

    wrap->io.buf = NULL; // sanity.
    wrap->io.next = NULL;
    wrap->next = NULL;
    wrap->active = false;

    // TODO: reuse lock and/or hv.
    item_remove(wrap->hdr_it);
}
#endif
static void conn_release_items(conn *c) {
    assert(c != NULL);

    if (c->item) {
        item_remove(c->item);
        c->item = 0;
    }

#ifdef EXTSTORE
    if (c->io_wraplist) {
        io_wrap *tmp = c->io_wraplist;
        while (tmp) {
            io_wrap *next = tmp->next;
            recache_or_free(c, tmp);
            // malloc'ed iovec list used for chunked extstore fetches.
            if (tmp->io.iov) {
                free(tmp->io.iov);
                tmp->io.iov = NULL;
            }
            do_cache_free(c->thread->io_cache, tmp); // lockless
            tmp = next;
        }
        c->io_wraplist = NULL;
    }
#endif

    // Cull any unsent responses.
    if (c->resp_head) {
        mc_resp *resp = c->resp_head;
        // r_f() handles the chain maintenance.
        while (resp) {
            // temporary by default. hide behind a debug flag in the future:
            // double free detection. Transmit loops can drop out early, but
            // here we could infinite loop.
            if (resp->free) {
                fprintf(stderr, "ERROR: double free detected during conn_release_items(): [%d] [%s]\n",
                        c->sfd, c->protocol == binary_prot ? "binary" : "ascii");
                // Since this is a critical failure, just leak the memory.
                // If these errors are seen, an abort() can be used instead.
                c->resp_head = NULL;
                c->resp = NULL;
                break;
            }
            resp = resp_finish(c, resp);
        }
    }
}

static void conn_cleanup(conn *c) {
    assert(c != NULL);

    conn_release_items(c);

    if (c->sasl_conn) {
        assert(settings.sasl);
        sasl_dispose(&c->sasl_conn);
        c->sasl_conn = NULL;
    }

    if (IS_UDP(c->transport)) {
        conn_set_state(c, conn_read);
    }
}

/*
 * Frees a connection.
 */
void conn_free(conn *c) {
    if (c) {
        assert(c != NULL);
#ifndef SFD_VALUE_IS_RANDOM
        assert(c->sfd >= 0 && c->sfd < max_fds);
#endif /* #ifndef SFD_VALUE_IS_RANDOM */

        MEMCACHED_CONN_DESTROY(c);
#ifndef SFD_VALUE_IS_RANDOM
        conns[c->sfd] = NULL;
#endif /* #ifndef SFD_VALUE_IS_RANDOM */
        if (c->rbuf)
            free(c->rbuf);
#ifdef TLS
        if (c->ssl_wbuf)
            c->ssl_wbuf = NULL;
#endif

#ifndef SFD_VALUE_IS_RANDOM
        free(c);
#else
        conn_list_close(c);
#endif /* #ifndef SFD_VALUE_IS_RANDOM */
    }
}

static void conn_close(conn *c) {
    assert(c != NULL);

    /* delete the event, the socket and the conn */
    event_del(&c->event);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d connection closed.\n", c->sfd);

    conn_cleanup(c);

    // force release of read buffer.
    if (c->thread) {
        c->rbytes = 0;
        rbuf_release(c);
    }

    MEMCACHED_CONN_RELEASE(c->sfd);
    conn_set_state(c, conn_closed);
#ifdef TLS
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
#endif
    sock_close(c->sfd);
#ifdef SFD_VALUE_IS_RANDOM
    conn_list_close(c);
#endif /* #ifdef SFD_VALUE_IS_RANDOM */
    pthread_mutex_lock(&conn_lock);
    allow_new_conns = true;
    pthread_mutex_unlock(&conn_lock);

    STATS_LOCK();
    stats_state.curr_conns--;
    STATS_UNLOCK();

    return;
}

// Since some connections might be off on side threads and some are managed as
// listeners we need to walk through them all from a central point.
// Must be called with all worker threads hung or in the process of closing.
void conn_close_all(void) {
    int i;
    for (i = 0; i < max_fds; i++) {
        if (conns[i] && conns[i]->state != conn_closed) {
            conn_close(conns[i]);
        }
    }
}

/**
 * Convert a state name to a human readable form.
 */
static const char *state_text(enum conn_states state) {
    const char* const statenames[] = { "conn_listening",
                                       "conn_new_cmd",
                                       "conn_waiting",
                                       "conn_read",
                                       "conn_parse_cmd",
                                       "conn_write",
                                       "conn_nread",
                                       "conn_swallow",
                                       "conn_closing",
                                       "conn_mwrite",
                                       "conn_closed",
                                       "conn_watch" };
    return statenames[state];
}

/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
static void conn_set_state(conn *c, enum conn_states state) {
    assert(c != NULL);
    assert(state >= conn_listening && state < conn_max_state);

    if (state != c->state) {
        if (settings.verbose > 2) {
            fprintf(stderr, "%d: going from %s to %s\n",
                    c->sfd, state_text(c->state),
                    state_text(state));
        }

        if (state == conn_write || state == conn_mwrite) {
            MEMCACHED_PROCESS_COMMAND_END(c->sfd, c->resp->wbuf, c->resp->wbytes);
        }
        c->state = state;
    }
}

/*
 * response object helper functions
 */
static void resp_reset(mc_resp *resp) {
    if (resp->item) {
        item_remove(resp->item);
        resp->item = NULL;
    }
    if (resp->write_and_free) {
        free(resp->write_and_free);
        resp->write_and_free = NULL;
    }
    resp->wbytes = 0;
    resp->tosend = 0;
    resp->iovcnt = 0;
    resp->chunked_data_iov = 0;
    resp->chunked_total = 0;
    resp->skip = false;
}

static void resp_add_iov(mc_resp *resp, const void *buf, int len) {
    assert(resp->iovcnt < MC_RESP_IOVCOUNT);
    int x = resp->iovcnt;
    resp->iov[x].iov_base = (void *)buf;
    resp->iov[x].iov_len = len;
    resp->iovcnt++;
    resp->tosend += len;
}

// Notes that an IOV should be handled as a chunked item header.
// TODO: I'm hoping this isn't a permanent abstraction while I learn what the
// API should be.
static void resp_add_chunked_iov(mc_resp *resp, const void *buf, int len) {
    resp->chunked_data_iov = resp->iovcnt;
    resp->chunked_total = len;
    resp_add_iov(resp, buf, len);
}

static bool resp_start(conn *c) {
    mc_resp *resp = do_cache_alloc(c->thread->resp_cache);
    if (!resp) {
        THR_STATS_LOCK(c);
        c->thread->stats.response_obj_oom++;
        THR_STATS_UNLOCK(c);
        return false;
    }
    // FIXME: make wbuf indirect or use offsetof to zero up until wbuf
    memset(resp, 0, sizeof(*resp));
    if (!c->resp_head) {
        c->resp_head = resp;
    }
    if (!c->resp) {
        c->resp = resp;
    } else {
        c->resp->next = resp;
        c->resp = resp;
    }
    if (IS_UDP(c->transport)) {
        // need to hold on to some data for async responses.
        c->resp->request_id = c->request_id;
        c->resp->request_addr = c->request_addr;
        c->resp->request_addr_size = c->request_addr_size;
    }
    return true;
}

// returns next response in chain.
static mc_resp* resp_finish(conn *c, mc_resp *resp) {
    mc_resp *next = resp->next;
    if (resp->item) {
        // TODO: cache hash value in resp obj?
        item_remove(resp->item);
        resp->item = NULL;
    }
    if (resp->write_and_free) {
        free(resp->write_and_free);
    }
    if (c->resp_head == resp) {
        c->resp_head = next;
    }
    if (c->resp == resp) {
        c->resp = NULL;
    }
    resp->free = true;
    do_cache_free(c->thread->resp_cache, resp);
    return next;
}

// tells if connection has a depth of response objects to process.
static bool resp_has_stack(conn *c) {
    return c->resp_head->next != NULL ? true : false;
}

static void out_string(conn *c, const char *str) {
    size_t len;
    mc_resp *resp = c->resp;

    assert(c != NULL);
    // if response was original filled with something, but we're now writing
    // out an error or similar, have to reset the object first.
    // TODO: since this is often redundant with allocation, how many callers
    // are actually requiring it be reset? Can we fast test by just looking at
    // tosend and reset if nonzero?
    resp_reset(resp);

    if (c->noreply) {
        // TODO: just invalidate the response since nothing's been attempted
        // to send yet?
        resp->skip = true;
        if (settings.verbose > 1)
            fprintf(stderr, ">%d NOREPLY %s\n", c->sfd, str);
        conn_set_state(c, conn_new_cmd);
        return;
    }

    if (settings.verbose > 1)
        fprintf(stderr, ">%d %s\n", c->sfd, str);

    // Fill response object with static string.

    len = strlen(str);
    if ((len + 2) > WRITE_BUFFER_SIZE) {
        /* ought to be always enough. just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    memcpy(resp->wbuf, str, len);
    memcpy(resp->wbuf + len, "\r\n", 2);
    resp_add_iov(resp, resp->wbuf, len + 2);

    conn_set_state(c, conn_new_cmd);
    return;
}

// For metaget-style ASCII commands. Ignores noreply, ensuring clients see
// protocol level errors.
static void out_errstring(conn *c, const char *str) {
    c->noreply = false;
    out_string(c, str);
}

/*
 * Outputs a protocol-specific "out of memory" error. For ASCII clients,
 * this is equivalent to out_string().
 */
static void out_of_memory(conn *c, char *ascii_error) {
    const static char error_prefix[] = "SERVER_ERROR ";
    const static int error_prefix_len = sizeof(error_prefix) - 1;

    if (c->protocol == binary_prot) {
        /* Strip off the generic error prefix; it's irrelevant in binary */
        if (!strncmp(ascii_error, error_prefix, error_prefix_len)) {
            ascii_error += error_prefix_len;
        }
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, ascii_error, 0);
    } else {
        out_string(c, ascii_error);
    }
}

/*
 * we get here after reading the value in set/add/replace commands. The command
 * has been stored in c->cmd, and the item is ready in c->item.
 */
static void complete_nread_ascii(conn *c) {
    assert(c != NULL);

    item *it = c->item;
    int comm = c->cmd;
    enum store_item_type ret;
    bool is_valid = false;

    pthread_mutex_lock(&c->thread->stats.mutex);
    c->thread->stats.slab_stats[ITEM_clsid(it)].set_cmds++;
    pthread_mutex_unlock(&c->thread->stats.mutex);

    if ((it->it_flags & ITEM_CHUNKED) == 0) {
        if (strncmp(ITEM_data(it) + it->nbytes - 2, "\r\n", 2) == 0) {
            is_valid = true;
        }
    } else {
        char buf[2];
        /* should point to the final item chunk */
        item_chunk *ch = (item_chunk *) c->ritem;
        assert(ch->used != 0);
        /* :( We need to look at the last two bytes. This could span two
         * chunks.
         */
        if (ch->used > 1) {
            buf[0] = ch->data[ch->used - 2];
            buf[1] = ch->data[ch->used - 1];
        } else {
            assert(ch->prev);
            assert(ch->used == 1);
            buf[0] = ch->prev->data[ch->prev->used - 1];
            buf[1] = ch->data[ch->used - 1];
        }
        if (strncmp(buf, "\r\n", 2) == 0) {
            is_valid = true;
        } else {
            assert(1 == 0);
        }
    }

    if (!is_valid) {
        // metaset mode always returns errors.
        if (c->mset_res) {
            c->noreply = false;
        }
        out_string(c, "CLIENT_ERROR bad data chunk");
    } else {
      ret = store_item(it, comm, c);

#ifdef ENABLE_DTRACE
      uint64_t cas = ITEM_get_cas(it);
      switch (c->cmd) {
      case NREAD_ADD:
          MEMCACHED_COMMAND_ADD(c->sfd, ITEM_key(it), it->nkey,
                                (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_REPLACE:
          MEMCACHED_COMMAND_REPLACE(c->sfd, ITEM_key(it), it->nkey,
                                    (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_APPEND:
          MEMCACHED_COMMAND_APPEND(c->sfd, ITEM_key(it), it->nkey,
                                   (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_PREPEND:
          MEMCACHED_COMMAND_PREPEND(c->sfd, ITEM_key(it), it->nkey,
                                    (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_SET:
          MEMCACHED_COMMAND_SET(c->sfd, ITEM_key(it), it->nkey,
                                (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_CAS:
          MEMCACHED_COMMAND_CAS(c->sfd, ITEM_key(it), it->nkey, it->nbytes,
                                cas);
          break;
      }
#endif

      if (c->mset_res) {
          // Replace the status code in the response.
          // Rest was prepared during mset parsing.
          mc_resp *resp = c->resp;
          conn_set_state(c, conn_new_cmd);
          switch (ret) {
          case STORED:
              memcpy(resp->wbuf, "OK ", 3);
              // Only place noreply is used for meta cmds is a nominal response.
              if (c->noreply) {
                  resp->skip = true;
              }
              break;
          case EXISTS:
              memcpy(resp->wbuf, "EX ", 3);
              break;
          case NOT_FOUND:
              memcpy(resp->wbuf, "NF ", 3);
              break;
          case NOT_STORED:
              memcpy(resp->wbuf, "NS ", 3);
              break;
          default:
              c->noreply = false;
              out_string(c, "SERVER_ERROR Unhandled storage type.");
          }
      } else {
          switch (ret) {
          case STORED:
              out_string(c, "STORED");
              break;
          case EXISTS:
              out_string(c, "EXISTS");
              break;
          case NOT_FOUND:
              out_string(c, "NOT_FOUND");
              break;
          case NOT_STORED:
              out_string(c, "NOT_STORED");
              break;
          default:
              out_string(c, "SERVER_ERROR Unhandled storage type.");
          }
      }

    }

    c->set_stale = false; /* force flag to be off just in case */
    c->mset_res = false;
    item_remove(c->item);       /* release the c->item reference */
    c->item = 0;
}

/**
 * get a pointer to the key in this request
 */
static char* binary_get_key(conn *c) {
    return c->rcurr - (c->binary_header.request.keylen);
}

static void add_bin_header(conn *c, uint16_t err, uint8_t hdr_len, uint16_t key_len, uint32_t body_len) {
    protocol_binary_response_header* header;
    mc_resp *resp = c->resp;

    assert(c);

    resp_reset(resp);

    header = (protocol_binary_response_header *)resp->wbuf;

    header->response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header->response.opcode = c->binary_header.request.opcode;
    header->response.keylen = (uint16_t)htons(key_len);

    header->response.extlen = (uint8_t)hdr_len;
    header->response.datatype = (uint8_t)PROTOCOL_BINARY_RAW_BYTES;
    header->response.status = (uint16_t)htons(err);

    header->response.bodylen = htonl(body_len);
    header->response.opaque = c->opaque;
    header->response.cas = htonll(c->cas);

    if (settings.verbose > 1) {
        int ii;
        fprintf(stderr, ">%d Writing bin response:", c->sfd);
        for (ii = 0; ii < sizeof(header->bytes); ++ii) {
            if (ii % 4 == 0) {
                fprintf(stderr, "\n>%d  ", c->sfd);
            }
            fprintf(stderr, " 0x%02x", header->bytes[ii]);
        }
        fprintf(stderr, "\n");
    }

    resp->wbytes = sizeof(header->response);
    resp_add_iov(resp, resp->wbuf, resp->wbytes);
}

/**
 * Writes a binary error response. If errstr is supplied, it is used as the
 * error text; otherwise a generic description of the error status code is
 * included.
 */
static void write_bin_error(conn *c, protocol_binary_response_status err,
                            const char *errstr, int swallow) {
    size_t len;

    if (!errstr) {
        switch (err) {
        case PROTOCOL_BINARY_RESPONSE_ENOMEM:
            errstr = "Out of memory";
            break;
        case PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND:
            errstr = "Unknown command";
            break;
        case PROTOCOL_BINARY_RESPONSE_KEY_ENOENT:
            errstr = "Not found";
            break;
        case PROTOCOL_BINARY_RESPONSE_EINVAL:
            errstr = "Invalid arguments";
            break;
        case PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS:
            errstr = "Data exists for key.";
            break;
        case PROTOCOL_BINARY_RESPONSE_E2BIG:
            errstr = "Too large.";
            break;
        case PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL:
            errstr = "Non-numeric server-side value for incr or decr";
            break;
        case PROTOCOL_BINARY_RESPONSE_NOT_STORED:
            errstr = "Not stored.";
            break;
        case PROTOCOL_BINARY_RESPONSE_AUTH_ERROR:
            errstr = "Auth failure.";
            break;
        default:
            assert(false);
            errstr = "UNHANDLED ERROR";
            fprintf(stderr, ">%d UNHANDLED ERROR: %d\n", c->sfd, err);
        }
    }

    if (settings.verbose > 1) {
        fprintf(stderr, ">%d Writing an error: %s\n", c->sfd, errstr);
    }

    len = strlen(errstr);
    add_bin_header(c, err, 0, 0, len);
    if (len > 0) {
        resp_add_iov(c->resp, errstr, len);
    }
    if (swallow > 0) {
        c->sbytes = swallow;
        conn_set_state(c, conn_swallow);
    } else {
        conn_set_state(c, conn_mwrite);
    }
}

/* Form and send a response to a command over the binary protocol */
static void write_bin_response(conn *c, void *d, int hlen, int keylen, int dlen) {
    if (!c->noreply || c->cmd == PROTOCOL_BINARY_CMD_GET ||
        c->cmd == PROTOCOL_BINARY_CMD_GETK) {
        add_bin_header(c, 0, hlen, keylen, dlen);
        mc_resp *resp = c->resp;
        if (dlen > 0) {
            resp_add_iov(resp, d, dlen);
        }
    }

    conn_set_state(c, conn_new_cmd);
}

static void complete_incr_bin(conn *c, char *extbuf) {
    item *it;
    char *key;
    size_t nkey;
    /* Weird magic in add_delta forces me to pad here */
    char tmpbuf[INCR_MAX_STORAGE_LEN];
    uint64_t cas = 0;

    protocol_binary_response_incr* rsp = (protocol_binary_response_incr*)c->resp->wbuf;
    protocol_binary_request_incr* req = (void *)extbuf;

    assert(c != NULL);
    //assert(c->wsize >= sizeof(*rsp));

    /* fix byteorder in the request */
    req->message.body.delta = ntohll(req->message.body.delta);
    req->message.body.initial = ntohll(req->message.body.initial);
    req->message.body.expiration = ntohl(req->message.body.expiration);
    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;

    if (settings.verbose > 1) {
        int i;
        fprintf(stderr, "incr ");

        for (i = 0; i < nkey; i++) {
            fprintf(stderr, "%c", key[i]);
        }
        fprintf(stderr, " %lld, %llu, %d\n",
                (long long)req->message.body.delta,
                (long long)req->message.body.initial,
                req->message.body.expiration);
    }

    if (c->binary_header.request.cas != 0) {
        cas = c->binary_header.request.cas;
    }
    switch(add_delta(c, key, nkey, c->cmd == PROTOCOL_BINARY_CMD_INCREMENT,
                     req->message.body.delta, tmpbuf,
                     &cas)) {
    case OK:
        rsp->message.body.value = htonll(strtoull(tmpbuf, NULL, 10));
        if (cas) {
            c->cas = cas;
        }
        write_bin_response(c, &rsp->message.body, 0, 0,
                           sizeof(rsp->message.body.value));
        break;
    case NON_NUMERIC:
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL, NULL, 0);
        break;
    case EOM:
        out_of_memory(c, "SERVER_ERROR Out of memory incrementing value");
        break;
    case DELTA_ITEM_NOT_FOUND:
        if (req->message.body.expiration != 0xffffffff) {
            /* Save some room for the response */
            rsp->message.body.value = htonll(req->message.body.initial);

            snprintf(tmpbuf, INCR_MAX_STORAGE_LEN, "%llu",
                (unsigned long long)req->message.body.initial);
            int res = strlen(tmpbuf);
            it = item_alloc(key, nkey, 0, realtime(req->message.body.expiration),
                            res + 2);

            if (it != NULL) {
                memcpy(ITEM_data(it), tmpbuf, res);
                memcpy(ITEM_data(it) + res, "\r\n", 2);

                if (store_item(it, NREAD_ADD, c)) {
                    c->cas = ITEM_get_cas(it);
                    write_bin_response(c, &rsp->message.body, 0, 0, sizeof(rsp->message.body.value));
                } else {
                    write_bin_error(c, PROTOCOL_BINARY_RESPONSE_NOT_STORED,
                                    NULL, 0);
                }
                item_remove(it);         /* release our reference */
            } else {
                out_of_memory(c,
                        "SERVER_ERROR Out of memory allocating new item");
            }
        } else {
            pthread_mutex_lock(&c->thread->stats.mutex);
            if (c->cmd == PROTOCOL_BINARY_CMD_INCREMENT) {
                c->thread->stats.incr_misses++;
            } else {
                c->thread->stats.decr_misses++;
            }
            pthread_mutex_unlock(&c->thread->stats.mutex);

            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
        }
        break;
    case DELTA_ITEM_CAS_MISMATCH:
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, NULL, 0);
        break;
    }
}

static void complete_update_bin(conn *c) {
    protocol_binary_response_status eno = PROTOCOL_BINARY_RESPONSE_EINVAL;
    enum store_item_type ret = NOT_STORED;
    assert(c != NULL);

    item *it = c->item;
    pthread_mutex_lock(&c->thread->stats.mutex);
    c->thread->stats.slab_stats[ITEM_clsid(it)].set_cmds++;
    pthread_mutex_unlock(&c->thread->stats.mutex);

    /* We don't actually receive the trailing two characters in the bin
     * protocol, so we're going to just set them here */
    if ((it->it_flags & ITEM_CHUNKED) == 0) {
        *(ITEM_data(it) + it->nbytes - 2) = '\r';
        *(ITEM_data(it) + it->nbytes - 1) = '\n';
    } else {
        assert(c->ritem);
        item_chunk *ch = (item_chunk *) c->ritem;
        if (ch->size == ch->used)
            ch = ch->next;
        assert(ch->size - ch->used >= 2);
        ch->data[ch->used] = '\r';
        ch->data[ch->used + 1] = '\n';
        ch->used += 2;
    }

    ret = store_item(it, c->cmd, c);

#ifdef ENABLE_DTRACE
    uint64_t cas = ITEM_get_cas(it);
    switch (c->cmd) {
    case NREAD_ADD:
        MEMCACHED_COMMAND_ADD(c->sfd, ITEM_key(it), it->nkey,
                              (ret == STORED) ? it->nbytes : -1, cas);
        break;
    case NREAD_REPLACE:
        MEMCACHED_COMMAND_REPLACE(c->sfd, ITEM_key(it), it->nkey,
                                  (ret == STORED) ? it->nbytes : -1, cas);
        break;
    case NREAD_APPEND:
        MEMCACHED_COMMAND_APPEND(c->sfd, ITEM_key(it), it->nkey,
                                 (ret == STORED) ? it->nbytes : -1, cas);
        break;
    case NREAD_PREPEND:
        MEMCACHED_COMMAND_PREPEND(c->sfd, ITEM_key(it), it->nkey,
                                 (ret == STORED) ? it->nbytes : -1, cas);
        break;
    case NREAD_SET:
        MEMCACHED_COMMAND_SET(c->sfd, ITEM_key(it), it->nkey,
                              (ret == STORED) ? it->nbytes : -1, cas);
        break;
    }
#endif

    switch (ret) {
    case STORED:
        /* Stored */
        write_bin_response(c, NULL, 0, 0, 0);
        break;
    case EXISTS:
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, NULL, 0);
        break;
    case NOT_FOUND:
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
        break;
    case NOT_STORED:
    case TOO_LARGE:
    case NO_MEMORY:
        if (c->cmd == NREAD_ADD) {
            eno = PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
        } else if(c->cmd == NREAD_REPLACE) {
            eno = PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
        } else {
            eno = PROTOCOL_BINARY_RESPONSE_NOT_STORED;
        }
        write_bin_error(c, eno, NULL, 0);
    }

    item_remove(c->item);       /* release the c->item reference */
    c->item = 0;
}

static void write_bin_miss_response(conn *c, char *key, size_t nkey) {
    if (nkey) {
        add_bin_header(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT,
                0, nkey, nkey);
        char *ofs = c->resp->wbuf + sizeof(protocol_binary_response_header);
        memcpy(ofs, key, nkey);
        resp_add_iov(c->resp, ofs, nkey);
        conn_set_state(c, conn_new_cmd);
    } else {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT,
                        NULL, 0);
    }
}

static void process_bin_get_or_touch(conn *c, char *extbuf) {
    item *it;

    protocol_binary_response_get* rsp = (protocol_binary_response_get*)c->resp->wbuf;
    char* key = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;
    int should_touch = (c->cmd == PROTOCOL_BINARY_CMD_TOUCH ||
                        c->cmd == PROTOCOL_BINARY_CMD_GAT ||
                        c->cmd == PROTOCOL_BINARY_CMD_GATK);
    int should_return_key = (c->cmd == PROTOCOL_BINARY_CMD_GETK ||
                             c->cmd == PROTOCOL_BINARY_CMD_GATK);
    int should_return_value = (c->cmd != PROTOCOL_BINARY_CMD_TOUCH);
    bool failed = false;

    if (settings.verbose > 1) {
        fprintf(stderr, "<%d %s ", c->sfd, should_touch ? "TOUCH" : "GET");
        if (fwrite(key, 1, nkey, stderr)) {}
        fputc('\n', stderr);
    }

    if (should_touch) {
        protocol_binary_request_touch *t = (void *)extbuf;
        time_t exptime = ntohl(t->message.body.expiration);

        it = item_touch(key, nkey, realtime(exptime), c);
    } else {
        it = item_get(key, nkey, c, DO_UPDATE);
    }

    if (it) {
        /* the length has two unnecessary bytes ("\r\n") */
        uint16_t keylen = 0;
        uint32_t bodylen = sizeof(rsp->message.body) + (it->nbytes - 2);

        pthread_mutex_lock(&c->thread->stats.mutex);
        if (should_touch) {
            c->thread->stats.touch_cmds++;
            c->thread->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
        } else {
            c->thread->stats.get_cmds++;
            c->thread->stats.lru_hits[it->slabs_clsid]++;
        }
        pthread_mutex_unlock(&c->thread->stats.mutex);

        if (should_touch) {
            MEMCACHED_COMMAND_TOUCH(c->sfd, ITEM_key(it), it->nkey,
                                    it->nbytes, ITEM_get_cas(it));
        } else {
            MEMCACHED_COMMAND_GET(c->sfd, ITEM_key(it), it->nkey,
                                  it->nbytes, ITEM_get_cas(it));
        }

        if (c->cmd == PROTOCOL_BINARY_CMD_TOUCH) {
            bodylen -= it->nbytes - 2;
        } else if (should_return_key) {
            bodylen += nkey;
            keylen = nkey;
        }

        add_bin_header(c, 0, sizeof(rsp->message.body), keylen, bodylen);
        rsp->message.header.response.cas = htonll(ITEM_get_cas(it));

        // add the flags
        FLAGS_CONV(it, rsp->message.body.flags);
        rsp->message.body.flags = htonl(rsp->message.body.flags);
        resp_add_iov(c->resp, &rsp->message.body, sizeof(rsp->message.body));

        if (should_return_key) {
            resp_add_iov(c->resp, ITEM_key(it), nkey);
        }

        if (should_return_value) {
            /* Add the data minus the CRLF */
#ifdef EXTSTORE
            if (it->it_flags & ITEM_HDR) {
                if (_get_extstore(c, it, c->resp) != 0) {
                    pthread_mutex_lock(&c->thread->stats.mutex);
                    c->thread->stats.get_oom_extstore++;
                    pthread_mutex_unlock(&c->thread->stats.mutex);

                    failed = true;
                }
            } else if ((it->it_flags & ITEM_CHUNKED) == 0) {
                resp_add_iov(c->resp, ITEM_data(it), it->nbytes - 2);
            } else {
                // Allow transmit handler to find the item and expand iov's
                resp_add_chunked_iov(c->resp, it, it->nbytes - 2);
            }
#else
            if ((it->it_flags & ITEM_CHUNKED) == 0) {
                resp_add_iov(c->resp, ITEM_data(it), it->nbytes - 2);
            } else {
                resp_add_chunked_iov(c->resp, it, it->nbytes - 2);
            }
#endif
        }

        if (!failed) {
            conn_set_state(c, conn_new_cmd);
            /* Remember this command so we can garbage collect it later */
#ifdef EXTSTORE
            if ((it->it_flags & ITEM_HDR) != 0 && should_return_value) {
                // Only have extstore clean if header and returning value.
                c->resp->item = NULL;
            } else {
                c->resp->item = it;
            }
#else
            c->resp->item = it;
#endif
        } else {
            item_remove(it);
        }
    } else {
        failed = true;
    }

    if (failed) {
        pthread_mutex_lock(&c->thread->stats.mutex);
        if (should_touch) {
            c->thread->stats.touch_cmds++;
            c->thread->stats.touch_misses++;
        } else {
            c->thread->stats.get_cmds++;
            c->thread->stats.get_misses++;
        }
        pthread_mutex_unlock(&c->thread->stats.mutex);

        if (should_touch) {
            MEMCACHED_COMMAND_TOUCH(c->sfd, key, nkey, -1, 0);
        } else {
            MEMCACHED_COMMAND_GET(c->sfd, key, nkey, -1, 0);
        }

        if (c->noreply) {
            conn_set_state(c, conn_new_cmd);
        } else {
            if (should_return_key) {
                write_bin_miss_response(c, key, nkey);
            } else {
                write_bin_miss_response(c, NULL, 0);
            }
        }
    }

    if (settings.detail_enabled) {
        stats_prefix_record_get(key, nkey, NULL != it);
    }
}

static void append_bin_stats(const char *key, const uint16_t klen,
                             const char *val, const uint32_t vlen,
                             conn *c) {
    char *buf = c->stats.buffer + c->stats.offset;
    uint32_t bodylen = klen + vlen;
    protocol_binary_response_header header = {
        .response.magic = (uint8_t)PROTOCOL_BINARY_RES,
        .response.opcode = PROTOCOL_BINARY_CMD_STAT,
        .response.keylen = (uint16_t)htons(klen),
        .response.datatype = (uint8_t)PROTOCOL_BINARY_RAW_BYTES,
        .response.bodylen = htonl(bodylen),
        .response.opaque = c->opaque
    };

    memcpy(buf, header.bytes, sizeof(header.response));
    buf += sizeof(header.response);

    if (klen > 0) {
        memcpy(buf, key, klen);
        buf += klen;

        if (vlen > 0) {
            memcpy(buf, val, vlen);
        }
    }

    c->stats.offset += sizeof(header.response) + bodylen;
}

static void append_ascii_stats(const char *key, const uint16_t klen,
                               const char *val, const uint32_t vlen,
                               conn *c) {
    char *pos = c->stats.buffer + c->stats.offset;
    uint32_t nbytes = 0;
    int remaining = c->stats.size - c->stats.offset;
    int room = remaining - 1;

    if (klen == 0 && vlen == 0) {
        nbytes = snprintf(pos, room, "END\r\n");
    } else if (vlen == 0) {
        nbytes = snprintf(pos, room, "STAT %s\r\n", key);
    } else {
        nbytes = snprintf(pos, room, "STAT %s %s\r\n", key, val);
    }

    c->stats.offset += nbytes;
}

static bool grow_stats_buf(conn *c, size_t needed) {
    size_t nsize = c->stats.size;
    size_t available = nsize - c->stats.offset;
    bool rv = true;

    /* Special case: No buffer -- need to allocate fresh */
    if (c->stats.buffer == NULL) {
        nsize = 1024;
        available = c->stats.size = c->stats.offset = 0;
    }

    while (needed > available) {
        assert(nsize > 0);
        nsize = nsize << 1;
        available = nsize - c->stats.offset;
    }

    if (nsize != c->stats.size) {
        char *ptr = realloc(c->stats.buffer, nsize);
        if (ptr) {
            c->stats.buffer = ptr;
            c->stats.size = nsize;
        } else {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            rv = false;
        }
    }

    return rv;
}

static void append_stats(const char *key, const uint16_t klen,
                  const char *val, const uint32_t vlen,
                  const void *cookie)
{
    /* value without a key is invalid */
    if (klen == 0 && vlen > 0) {
        return;
    }

    conn *c = (conn*)cookie;

    if (c->protocol == binary_prot) {
        size_t needed = vlen + klen + sizeof(protocol_binary_response_header);
        if (!grow_stats_buf(c, needed)) {
            return;
        }
        append_bin_stats(key, klen, val, vlen, c);
    } else {
        size_t needed = vlen + klen + 10; // 10 == "STAT = \r\n"
        if (!grow_stats_buf(c, needed)) {
            return;
        }
        append_ascii_stats(key, klen, val, vlen, c);
    }

    assert(c->stats.offset <= c->stats.size);
}

static void process_bin_stat(conn *c) {
    char *subcommand = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;

    if (settings.verbose > 1) {
        int ii;
        fprintf(stderr, "<%d STATS ", c->sfd);
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", subcommand[ii]);
        }
        fprintf(stderr, "\n");
    }

    if (nkey == 0) {
        /* request all statistics */
        server_stats(&append_stats, c);
        (void)get_stats(NULL, 0, &append_stats, c);
    } else if (strncmp(subcommand, "reset", 5) == 0) {
        stats_reset();
    } else if (strncmp(subcommand, "settings", 8) == 0) {
        process_stat_settings(&append_stats, c);
    } else if (strncmp(subcommand, "detail", 6) == 0) {
        char *subcmd_pos = subcommand + 6;
        if (strncmp(subcmd_pos, " dump", 5) == 0) {
            int len;
            char *dump_buf = stats_prefix_dump(&len);
            if (dump_buf == NULL || len <= 0) {
                out_of_memory(c, "SERVER_ERROR Out of memory generating stats");
                if (dump_buf != NULL)
                    free(dump_buf);
                return;
            } else {
                append_stats("detailed", strlen("detailed"), dump_buf, len, c);
                free(dump_buf);
            }
        } else if (strncmp(subcmd_pos, " on", 3) == 0) {
            settings.detail_enabled = 1;
        } else if (strncmp(subcmd_pos, " off", 4) == 0) {
            settings.detail_enabled = 0;
        } else {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
            return;
        }
    } else {
        if (get_stats(subcommand, nkey, &append_stats, c)) {
            if (c->stats.buffer == NULL) {
                out_of_memory(c, "SERVER_ERROR Out of memory generating stats");
            } else {
                write_and_free(c, c->stats.buffer, c->stats.offset);
                c->stats.buffer = NULL;
            }
        } else {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
        }

        return;
    }

    /* Append termination package and start the transfer */
    append_stats(NULL, 0, NULL, 0, c);
    if (c->stats.buffer == NULL) {
        out_of_memory(c, "SERVER_ERROR Out of memory preparing to send stats");
    } else {
        write_and_free(c, c->stats.buffer, c->stats.offset);
        c->stats.buffer = NULL;
    }
}

/* Just write an error message and disconnect the client */
static void handle_binary_protocol_error(conn *c) {
    write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
    if (settings.verbose) {
        fprintf(stderr, "Protocol error (opcode %02x), close connection %d\n",
                c->binary_header.request.opcode, c->sfd);
    }
    c->close_after_write = true;
}

static void init_sasl_conn(conn *c) {
    assert(c);
    /* should something else be returned? */
    if (!settings.sasl)
        return;

    c->authenticated = false;

    if (!c->sasl_conn) {
        int result=sasl_server_new("memcached",
                                   NULL,
                                   my_sasl_hostname[0] ? my_sasl_hostname : NULL,
                                   NULL, NULL,
                                   NULL, 0, &c->sasl_conn);
        if (result != SASL_OK) {
            if (settings.verbose) {
                fprintf(stderr, "Failed to initialize SASL conn.\n");
            }
            c->sasl_conn = NULL;
        }
    }
}

static void bin_list_sasl_mechs(conn *c) {
    // Guard against a disabled SASL.
    if (!settings.sasl) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL,
                        c->binary_header.request.bodylen
                        - c->binary_header.request.keylen);
        return;
    }

    init_sasl_conn(c);
    const char *result_string = NULL;
    unsigned int string_length = 0;
    int result=sasl_listmech(c->sasl_conn, NULL,
                             "",   /* What to prepend the string with */
                             " ",  /* What to separate mechanisms with */
                             "",   /* What to append to the string */
                             &result_string, &string_length,
                             NULL);
    if (result != SASL_OK) {
        /* Perhaps there's a better error for this... */
        if (settings.verbose) {
            fprintf(stderr, "Failed to list SASL mechanisms.\n");
        }
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
        return;
    }
    write_bin_response(c, (char*)result_string, 0, 0, string_length);
}

static void process_bin_sasl_auth(conn *c) {
    // Guard for handling disabled SASL on the server.
    if (!settings.sasl) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL,
                        c->binary_header.request.bodylen
                        - c->binary_header.request.keylen);
        return;
    }

    assert(c->binary_header.request.extlen == 0);

    int nkey = c->binary_header.request.keylen;
    int vlen = c->binary_header.request.bodylen - nkey;

    if (nkey > MAX_SASL_MECH_LEN) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, vlen);
        conn_set_state(c, conn_swallow);
        return;
    }

    char *key = binary_get_key(c);
    assert(key);

    item *it = item_alloc(key, nkey, 0, 0, vlen+2);

    /* Can't use a chunked item for SASL authentication. */
    if (it == 0 || (it->it_flags & ITEM_CHUNKED)) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, vlen);
        conn_set_state(c, conn_swallow);
        if (it) {
            do_item_remove(it);
        }
        return;
    }

    c->item = it;
    c->ritem = ITEM_data(it);
    c->rlbytes = vlen;
    conn_set_state(c, conn_nread);
    c->substate = bin_reading_sasl_auth_data;
}

static void process_bin_complete_sasl_auth(conn *c) {
    assert(settings.sasl);
    const char *out = NULL;
    unsigned int outlen = 0;

    assert(c->item);
    init_sasl_conn(c);

    int nkey = c->binary_header.request.keylen;
    int vlen = c->binary_header.request.bodylen - nkey;

    if (nkey > ((item*) c->item)->nkey) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, vlen);
        conn_set_state(c, conn_swallow);
        return;
    }

    char mech[nkey+1];
    memcpy(mech, ITEM_key((item*)c->item), nkey);
    mech[nkey] = 0x00;

    if (settings.verbose)
        fprintf(stderr, "mech:  ``%s'' with %d bytes of data\n", mech, vlen);

    const char *challenge = vlen == 0 ? NULL : ITEM_data((item*) c->item);

    if (vlen > ((item*) c->item)->nbytes) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, vlen);
        conn_set_state(c, conn_swallow);
        return;
    }

    int result=-1;

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SASL_AUTH:
        result = sasl_server_start(c->sasl_conn, mech,
                                   challenge, vlen,
                                   &out, &outlen);
        c->sasl_started = (result == SASL_OK || result == SASL_CONTINUE);
        break;
    case PROTOCOL_BINARY_CMD_SASL_STEP:
        if (!c->sasl_started) {
            if (settings.verbose) {
                fprintf(stderr, "%d: SASL_STEP called but sasl_server_start "
                        "not called for this connection!\n", c->sfd);
            }
            break;
        }
        result = sasl_server_step(c->sasl_conn,
                                  challenge, vlen,
                                  &out, &outlen);
        break;
    default:
        assert(false); /* CMD should be one of the above */
        /* This code is pretty much impossible, but makes the compiler
           happier */
        if (settings.verbose) {
            fprintf(stderr, "Unhandled command %d with challenge %s\n",
                    c->cmd, challenge);
        }
        break;
    }

    if (settings.verbose) {
        fprintf(stderr, "sasl result code:  %d\n", result);
    }

    switch(result) {
    case SASL_OK:
        c->authenticated = true;
        write_bin_response(c, "Authenticated", 0, 0, strlen("Authenticated"));
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.auth_cmds++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
        break;
    case SASL_CONTINUE:
        add_bin_header(c, PROTOCOL_BINARY_RESPONSE_AUTH_CONTINUE, 0, 0, outlen);
        if (outlen > 0) {
            resp_add_iov(c->resp, out, outlen);
        }
        // Immediately flush our write.
        conn_set_state(c, conn_mwrite);
        break;
    default:
        if (settings.verbose)
            fprintf(stderr, "Unknown sasl response:  %d\n", result);
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.auth_cmds++;
        c->thread->stats.auth_errors++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    }
}

static bool authenticated(conn *c) {
    assert(settings.sasl);
    bool rv = false;

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_SASL_AUTH:       /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_SASL_STEP:       /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_VERSION:         /* FALLTHROUGH */
        rv = true;
        break;
    default:
        rv = c->authenticated;
    }

    if (settings.verbose > 1) {
        fprintf(stderr, "authenticated() in cmd 0x%02x is %s\n",
                c->cmd, rv ? "true" : "false");
    }

    return rv;
}

static void dispatch_bin_command(conn *c, char *extbuf) {
    int protocol_error = 0;

    uint8_t extlen = c->binary_header.request.extlen;
    uint16_t keylen = c->binary_header.request.keylen;
    uint32_t bodylen = c->binary_header.request.bodylen;

    if (keylen > bodylen || keylen + extlen > bodylen) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL, 0);
        c->close_after_write = true;
        return;
    }

    if (settings.sasl && !authenticated(c)) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
        c->close_after_write = true;
        return;
    }

    MEMCACHED_PROCESS_COMMAND_START(c->sfd, c->rcurr, c->rbytes);
    c->noreply = true;

    /* binprot supports 16bit keys, but internals are still 8bit */
    if (keylen > KEY_MAX_LENGTH) {
        handle_binary_protocol_error(c);
        return;
    }

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SETQ:
        c->cmd = PROTOCOL_BINARY_CMD_SET;
        break;
    case PROTOCOL_BINARY_CMD_ADDQ:
        c->cmd = PROTOCOL_BINARY_CMD_ADD;
        break;
    case PROTOCOL_BINARY_CMD_REPLACEQ:
        c->cmd = PROTOCOL_BINARY_CMD_REPLACE;
        break;
    case PROTOCOL_BINARY_CMD_DELETEQ:
        c->cmd = PROTOCOL_BINARY_CMD_DELETE;
        break;
    case PROTOCOL_BINARY_CMD_INCREMENTQ:
        c->cmd = PROTOCOL_BINARY_CMD_INCREMENT;
        break;
    case PROTOCOL_BINARY_CMD_DECREMENTQ:
        c->cmd = PROTOCOL_BINARY_CMD_DECREMENT;
        break;
    case PROTOCOL_BINARY_CMD_QUITQ:
        c->cmd = PROTOCOL_BINARY_CMD_QUIT;
        break;
    case PROTOCOL_BINARY_CMD_FLUSHQ:
        c->cmd = PROTOCOL_BINARY_CMD_FLUSH;
        break;
    case PROTOCOL_BINARY_CMD_APPENDQ:
        c->cmd = PROTOCOL_BINARY_CMD_APPEND;
        break;
    case PROTOCOL_BINARY_CMD_PREPENDQ:
        c->cmd = PROTOCOL_BINARY_CMD_PREPEND;
        break;
    case PROTOCOL_BINARY_CMD_GETQ:
        c->cmd = PROTOCOL_BINARY_CMD_GET;
        break;
    case PROTOCOL_BINARY_CMD_GETKQ:
        c->cmd = PROTOCOL_BINARY_CMD_GETK;
        break;
    case PROTOCOL_BINARY_CMD_GATQ:
        c->cmd = PROTOCOL_BINARY_CMD_GAT;
        break;
    case PROTOCOL_BINARY_CMD_GATKQ:
        c->cmd = PROTOCOL_BINARY_CMD_GATK;
        break;
    default:
        c->noreply = false;
    }

    switch (c->cmd) {
        case PROTOCOL_BINARY_CMD_VERSION:
            if (extlen == 0 && keylen == 0 && bodylen == 0) {
                write_bin_response(c, VERSION, 0, 0, strlen(VERSION));
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_FLUSH:
            if (keylen == 0 && bodylen == extlen && (extlen == 0 || extlen == 4)) {
                process_bin_flush(c, extbuf);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_NOOP:
            if (extlen == 0 && keylen == 0 && bodylen == 0) {
                write_bin_response(c, NULL, 0, 0, 0);
                // NOOP forces pipeline flush.
                conn_set_state(c, conn_mwrite);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_SET: /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_ADD: /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_REPLACE:
            if (extlen == 8 && keylen != 0 && bodylen >= (keylen + 8)) {
                process_bin_update(c, extbuf);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_GETQ:  /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_GET:   /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_GETKQ: /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_GETK:
            if (extlen == 0 && bodylen == keylen && keylen > 0) {
                process_bin_get_or_touch(c, extbuf);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_DELETE:
            if (keylen > 0 && extlen == 0 && bodylen == keylen) {
                process_bin_delete(c);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_INCREMENT:
        case PROTOCOL_BINARY_CMD_DECREMENT:
            if (keylen > 0 && extlen == 20 && bodylen == (keylen + extlen)) {
                complete_incr_bin(c, extbuf);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_APPEND:
        case PROTOCOL_BINARY_CMD_PREPEND:
            if (keylen > 0 && extlen == 0) {
                process_bin_append_prepend(c);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_STAT:
            if (extlen == 0) {
                process_bin_stat(c);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_QUIT:
            if (keylen == 0 && extlen == 0 && bodylen == 0) {
                write_bin_response(c, NULL, 0, 0, 0);
                conn_set_state(c, conn_mwrite);
                c->close_after_write = true;
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS:
            if (extlen == 0 && keylen == 0 && bodylen == 0) {
                bin_list_sasl_mechs(c);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_SASL_AUTH:
        case PROTOCOL_BINARY_CMD_SASL_STEP:
            if (extlen == 0 && keylen != 0) {
                process_bin_sasl_auth(c);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_TOUCH:
        case PROTOCOL_BINARY_CMD_GAT:
        case PROTOCOL_BINARY_CMD_GATQ:
        case PROTOCOL_BINARY_CMD_GATK:
        case PROTOCOL_BINARY_CMD_GATKQ:
            if (extlen == 4 && keylen != 0) {
                process_bin_get_or_touch(c, extbuf);
            } else {
                protocol_error = 1;
            }
            break;
        default:
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL,
                            bodylen);
    }

    if (protocol_error)
        handle_binary_protocol_error(c);
}

static void process_bin_update(conn *c, char *extbuf) {
    char *key;
    int nkey;
    int vlen;
    item *it;
    protocol_binary_request_set* req = (void *)extbuf;

    assert(c != NULL);

    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;

    /* fix byteorder in the request */
    req->message.body.flags = ntohl(req->message.body.flags);
    req->message.body.expiration = ntohl(req->message.body.expiration);

    vlen = c->binary_header.request.bodylen - (nkey + c->binary_header.request.extlen);

    if (settings.verbose > 1) {
        int ii;
        if (c->cmd == PROTOCOL_BINARY_CMD_ADD) {
            fprintf(stderr, "<%d ADD ", c->sfd);
        } else if (c->cmd == PROTOCOL_BINARY_CMD_SET) {
            fprintf(stderr, "<%d SET ", c->sfd);
        } else {
            fprintf(stderr, "<%d REPLACE ", c->sfd);
        }
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }

        fprintf(stderr, " Value len is %d", vlen);
        fprintf(stderr, "\n");
    }

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    it = item_alloc(key, nkey, req->message.body.flags,
            realtime(req->message.body.expiration), vlen+2);

    if (it == 0) {
        enum store_item_type status;
        if (! item_size_ok(nkey, req->message.body.flags, vlen + 2)) {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_E2BIG, NULL, vlen);
            status = TOO_LARGE;
        } else {
            out_of_memory(c, "SERVER_ERROR Out of memory allocating item");
            /* This error generating method eats the swallow value. Add here. */
            c->sbytes = vlen;
            status = NO_MEMORY;
        }
        /* FIXME: losing c->cmd since it's translated below. refactor? */
        LOGGER_LOG(c->thread->l, LOG_MUTATIONS, LOGGER_ITEM_STORE,
                NULL, status, 0, key, nkey, req->message.body.expiration,
                ITEM_clsid(it), c->sfd);

        /* Avoid stale data persisting in cache because we failed alloc.
         * Unacceptable for SET. Anywhere else too? */
        if (c->cmd == PROTOCOL_BINARY_CMD_SET) {
            it = item_get(key, nkey, c, DONT_UPDATE);
            if (it) {
                item_unlink(it);
                STORAGE_delete(c->thread->storage, it);
                item_remove(it);
            }
        }

        /* swallow the data line */
        conn_set_state(c, conn_swallow);
        return;
    }

    ITEM_set_cas(it, c->binary_header.request.cas);

    switch (c->cmd) {
        case PROTOCOL_BINARY_CMD_ADD:
            c->cmd = NREAD_ADD;
            break;
        case PROTOCOL_BINARY_CMD_SET:
            c->cmd = NREAD_SET;
            break;
        case PROTOCOL_BINARY_CMD_REPLACE:
            c->cmd = NREAD_REPLACE;
            break;
        default:
            assert(0);
    }

    if (ITEM_get_cas(it) != 0) {
        c->cmd = NREAD_CAS;
    }

    c->item = it;
#ifdef NEED_ALIGN
    if (it->it_flags & ITEM_CHUNKED) {
        c->ritem = ITEM_schunk(it);
    } else {
        c->ritem = ITEM_data(it);
    }
#else
    c->ritem = ITEM_data(it);
#endif
    c->rlbytes = vlen;
    conn_set_state(c, conn_nread);
    c->substate = bin_read_set_value;
}

static void process_bin_append_prepend(conn *c) {
    char *key;
    int nkey;
    int vlen;
    item *it;

    assert(c != NULL);

    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;
    vlen = c->binary_header.request.bodylen - nkey;

    if (settings.verbose > 1) {
        fprintf(stderr, "Value len is %d\n", vlen);
    }

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    it = item_alloc(key, nkey, 0, 0, vlen+2);

    if (it == 0) {
        if (! item_size_ok(nkey, 0, vlen + 2)) {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_E2BIG, NULL, vlen);
        } else {
            out_of_memory(c, "SERVER_ERROR Out of memory allocating item");
            /* OOM calls eat the swallow value. Add here. */
            c->sbytes = vlen;
        }
        /* swallow the data line */
        conn_set_state(c, conn_swallow);
        return;
    }

    ITEM_set_cas(it, c->binary_header.request.cas);

    switch (c->cmd) {
        case PROTOCOL_BINARY_CMD_APPEND:
            c->cmd = NREAD_APPEND;
            break;
        case PROTOCOL_BINARY_CMD_PREPEND:
            c->cmd = NREAD_PREPEND;
            break;
        default:
            assert(0);
    }

    c->item = it;
#ifdef NEED_ALIGN
    if (it->it_flags & ITEM_CHUNKED) {
        c->ritem = ITEM_schunk(it);
    } else {
        c->ritem = ITEM_data(it);
    }
#else
    c->ritem = ITEM_data(it);
#endif
    c->rlbytes = vlen;
    conn_set_state(c, conn_nread);
    c->substate = bin_read_set_value;
}

static void process_bin_flush(conn *c, char *extbuf) {
    time_t exptime = 0;
    protocol_binary_request_flush* req = (void *)extbuf;
    rel_time_t new_oldest = 0;

    if (!settings.flush_enabled) {
      // flush_all is not allowed but we log it on stats
      write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
      return;
    }

    if (c->binary_header.request.extlen == sizeof(req->message.body)) {
        exptime = ntohl(req->message.body.expiration);
    }

    if (exptime > 0) {
        new_oldest = realtime(exptime);
    } else {
        new_oldest = current_time;
    }
    if (settings.use_cas) {
        settings.oldest_live = new_oldest - 1;
        if (settings.oldest_live <= current_time)
            settings.oldest_cas = get_cas_id();
    } else {
        settings.oldest_live = new_oldest;
    }

    pthread_mutex_lock(&c->thread->stats.mutex);
    c->thread->stats.flush_cmds++;
    pthread_mutex_unlock(&c->thread->stats.mutex);

    write_bin_response(c, NULL, 0, 0, 0);
}

static void process_bin_delete(conn *c) {
    item *it;
    uint32_t hv;

    char* key = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;

    assert(c != NULL);

    if (settings.verbose > 1) {
        int ii;
        fprintf(stderr, "Deleting ");
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }
        fprintf(stderr, "\n");
    }

    if (settings.detail_enabled) {
        stats_prefix_record_delete(key, nkey);
    }

    it = item_get_locked(key, nkey, c, DONT_UPDATE, &hv);
    if (it) {
        uint64_t cas = c->binary_header.request.cas;
        if (cas == 0 || cas == ITEM_get_cas(it)) {
            MEMCACHED_COMMAND_DELETE(c->sfd, ITEM_key(it), it->nkey);
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.slab_stats[ITEM_clsid(it)].delete_hits++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            do_item_unlink(it, hv);
            STORAGE_delete(c->thread->storage, it);
            write_bin_response(c, NULL, 0, 0, 0);
        } else {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, NULL, 0);
        }
        do_item_remove(it);      /* release our reference */
    } else {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.delete_misses++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    }
    item_unlock(hv);
}

static void complete_nread_binary(conn *c) {
    assert(c != NULL);
    assert(c->cmd >= 0);

    switch(c->substate) {
    case bin_read_set_value:
        complete_update_bin(c);
        break;
    case bin_reading_sasl_auth_data:
        process_bin_complete_sasl_auth(c);
        if (c->item) {
            do_item_remove(c->item);
            c->item = NULL;
        }
        break;
    default:
        fprintf(stderr, "Not handling substate %d\n", c->substate);
        assert(0);
    }
}

static void reset_cmd_handler(conn *c) {
    c->cmd = -1;
    c->substate = bin_no_state;
    if (c->item != NULL) {
        // TODO: Any other way to get here?
        // SASL auth was mistakenly using it. Nothing else should?
        item_remove(c->item);
        c->item = NULL;
    }
    if (c->rbytes > 0) {
        conn_set_state(c, conn_parse_cmd);
    } else if (c->resp_head) {
        conn_set_state(c, conn_mwrite);
    } else {
        conn_set_state(c, conn_waiting);
    }
}

static void complete_nread(conn *c) {
    assert(c != NULL);
    assert(c->protocol == ascii_prot
           || c->protocol == binary_prot);

    if (c->protocol == ascii_prot) {
        complete_nread_ascii(c);
    } else if (c->protocol == binary_prot) {
        complete_nread_binary(c);
    }
}

/* Destination must always be chunked */
/* This should be part of item.c */
static int _store_item_copy_chunks(item *d_it, item *s_it, const int len) {
    item_chunk *dch = (item_chunk *) ITEM_schunk(d_it);
    /* Advance dch until we find free space */
    while (dch->size == dch->used) {
        if (dch->next) {
            dch = dch->next;
        } else {
            break;
        }
    }

    if (s_it->it_flags & ITEM_CHUNKED) {
        int remain = len;
        item_chunk *sch = (item_chunk *) ITEM_schunk(s_it);
        int copied = 0;
        /* Fills dch's to capacity, not straight copy sch in case data is
         * being added or removed (ie append/prepend)
         */
        while (sch && dch && remain) {
            assert(dch->used <= dch->size);
            int todo = (dch->size - dch->used < sch->used - copied)
                ? dch->size - dch->used : sch->used - copied;
            if (remain < todo)
                todo = remain;
            memcpy(dch->data + dch->used, sch->data + copied, todo);
            dch->used += todo;
            copied += todo;
            remain -= todo;
            assert(dch->used <= dch->size);
            if (dch->size == dch->used) {
                item_chunk *tch = do_item_alloc_chunk(dch, remain);
                if (tch) {
                    dch = tch;
                } else {
                    return -1;
                }
            }
            assert(copied <= sch->used);
            if (copied == sch->used) {
                copied = 0;
                sch = sch->next;
            }
        }
        /* assert that the destination had enough space for the source */
        assert(remain == 0);
    } else {
        int done = 0;
        /* Fill dch's via a non-chunked item. */
        while (len > done && dch) {
            int todo = (dch->size - dch->used < len - done)
                ? dch->size - dch->used : len - done;
            //assert(dch->size - dch->used != 0);
            memcpy(dch->data + dch->used, ITEM_data(s_it) + done, todo);
            done += todo;
            dch->used += todo;
            assert(dch->used <= dch->size);
            if (dch->size == dch->used) {
                item_chunk *tch = do_item_alloc_chunk(dch, len - done);
                if (tch) {
                    dch = tch;
                } else {
                    return -1;
                }
            }
        }
        assert(len == done);
    }
    return 0;
}

static int _store_item_copy_data(int comm, item *old_it, item *new_it, item *add_it) {
    if (comm == NREAD_APPEND) {
        if (new_it->it_flags & ITEM_CHUNKED) {
            if (_store_item_copy_chunks(new_it, old_it, old_it->nbytes - 2) == -1 ||
                _store_item_copy_chunks(new_it, add_it, add_it->nbytes) == -1) {
                return -1;
            }
        } else {
            memcpy(ITEM_data(new_it), ITEM_data(old_it), old_it->nbytes);
            memcpy(ITEM_data(new_it) + old_it->nbytes - 2 /* CRLF */, ITEM_data(add_it), add_it->nbytes);
        }
    } else {
        /* NREAD_PREPEND */
        if (new_it->it_flags & ITEM_CHUNKED) {
            if (_store_item_copy_chunks(new_it, add_it, add_it->nbytes - 2) == -1 ||
                _store_item_copy_chunks(new_it, old_it, old_it->nbytes) == -1) {
                return -1;
            }
        } else {
            memcpy(ITEM_data(new_it), ITEM_data(add_it), add_it->nbytes);
            memcpy(ITEM_data(new_it) + add_it->nbytes - 2 /* CRLF */, ITEM_data(old_it), old_it->nbytes);
        }
    }
    return 0;
}

/*
 * Stores an item in the cache according to the semantics of one of the set
 * commands. Protected by the item lock.
 *
 * Returns the state of storage.
 */
enum store_item_type do_store_item(item *it, int comm, conn *c, const uint32_t hv) {
    char *key = ITEM_key(it);
    item *old_it = do_item_get(key, it->nkey, hv, c, DONT_UPDATE);
    enum store_item_type stored = NOT_STORED;

    enum cas_result { CAS_NONE, CAS_MATCH, CAS_BADVAL, CAS_STALE, CAS_MISS };

    item *new_it = NULL;
    uint32_t flags;

    /* Do the CAS test up front so we can apply to all store modes */
    enum cas_result cas_res = CAS_NONE;

    bool do_store = false;
    if (old_it != NULL) {
        // Most of the CAS work requires something to compare to.
        uint64_t it_cas = ITEM_get_cas(it);
        uint64_t old_cas = ITEM_get_cas(old_it);
        if (it_cas == 0) {
            cas_res = CAS_NONE;
        } else if (it_cas == old_cas) {
            cas_res = CAS_MATCH;
        } else if (c->set_stale && it_cas < old_cas) {
            cas_res = CAS_STALE;
        } else {
            cas_res = CAS_BADVAL;
        }

        switch (comm) {
            case NREAD_ADD:
                /* add only adds a nonexistent item, but promote to head of LRU */
                do_item_update(old_it);
                break;
            case NREAD_CAS:
                if (cas_res == CAS_MATCH) {
                    // cas validates
                    // it and old_it may belong to different classes.
                    // I'm updating the stats for the one that's getting pushed out
                    pthread_mutex_lock(&c->thread->stats.mutex);
                    c->thread->stats.slab_stats[ITEM_clsid(old_it)].cas_hits++;
                    pthread_mutex_unlock(&c->thread->stats.mutex);
                    do_store = true;
                } else if (cas_res == CAS_STALE) {
                    // if we're allowed to set a stale value, CAS must be lower than
                    // the current item's CAS.
                    // This replaces the value, but should preserve TTL, and stale
                    // item marker bit + token sent if exists.
                    it->exptime = old_it->exptime;
                    it->it_flags |= ITEM_STALE;
                    if (old_it->it_flags & ITEM_TOKEN_SENT) {
                        it->it_flags |= ITEM_TOKEN_SENT;
                    }

                    pthread_mutex_lock(&c->thread->stats.mutex);
                    c->thread->stats.slab_stats[ITEM_clsid(old_it)].cas_hits++;
                    pthread_mutex_unlock(&c->thread->stats.mutex);
                    do_store = true;
                } else {
                    // NONE or BADVAL are the same for CAS cmd
                    pthread_mutex_lock(&c->thread->stats.mutex);
                    c->thread->stats.slab_stats[ITEM_clsid(old_it)].cas_badval++;
                    pthread_mutex_unlock(&c->thread->stats.mutex);

                    if (settings.verbose > 1) {
                        fprintf(stderr, "CAS:  failure: expected %llu, got %llu\n",
                                (unsigned long long)ITEM_get_cas(old_it),
                                (unsigned long long)ITEM_get_cas(it));
                    }
                    stored = EXISTS;
                }
                break;
            case NREAD_APPEND:
            case NREAD_PREPEND:
                if (cas_res != CAS_NONE && cas_res != CAS_MATCH) {
                    stored = EXISTS;
                    break;
                }
#ifdef EXTSTORE
                if ((old_it->it_flags & ITEM_HDR) != 0) {
                    /* block append/prepend from working with extstore-d items.
                     * leave response code to NOT_STORED default */
                    break;
                }
#endif
                /* we have it and old_it here - alloc memory to hold both */
                FLAGS_CONV(old_it, flags);
                new_it = do_item_alloc(key, it->nkey, flags, old_it->exptime, it->nbytes + old_it->nbytes - 2 /* CRLF */);

                // OOM trying to copy.
                if (new_it == NULL)
                    break;
                /* copy data from it and old_it to new_it */
                if (_store_item_copy_data(comm, old_it, new_it, it) == -1) {
                    // failed data copy
                    break;
                } else {
                    // refcount of new_it is 1 here. will end up 2 after link.
                    // it's original ref is managed outside of this function
                    it = new_it;
                    do_store = true;
                }
                break;
            case NREAD_REPLACE:
            case NREAD_SET:
                do_store = true;
                break;
        }

        if (do_store) {
            STORAGE_delete(c->thread->storage, old_it);
            item_replace(old_it, it, hv);
            stored = STORED;
        }

        do_item_remove(old_it);         /* release our reference */
        if (new_it != NULL) {
            // append/prepend end up with an extra reference for new_it.
            do_item_remove(new_it);
        }
    } else {
        /* No pre-existing item to replace or compare to. */
        if (ITEM_get_cas(it) != 0) {
            /* Asked for a CAS match but nothing to compare it to. */
            cas_res = CAS_MISS;
        }

        switch (comm) {
            case NREAD_ADD:
            case NREAD_SET:
                do_store = true;
                break;
            case NREAD_CAS:
                // LRU expired
                stored = NOT_FOUND;
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.cas_misses++;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                break;
            case NREAD_REPLACE:
            case NREAD_APPEND:
            case NREAD_PREPEND:
                /* Requires an existing item. */
                break;
        }

        if (do_store) {
            do_item_link(it, hv);
            stored = STORED;
        }
    }

    if (stored == STORED) {
        c->cas = ITEM_get_cas(it);
    }
    LOGGER_LOG(c->thread->l, LOG_MUTATIONS, LOGGER_ITEM_STORE, NULL,
            stored, comm, ITEM_key(it), it->nkey, it->exptime, ITEM_clsid(it), c->sfd);

    return stored;
}

typedef struct token_s {
    char *value;
    size_t length;
} token_t;

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1
#define KEY_TOKEN 1

#define MAX_TOKENS 24

#define WANT_TOKENS(ntokens, min, max) \
    do { \
        if ((min != -1 && ntokens < min) || (max != -1 && ntokens > max)) { \
            out_string(c, "ERROR"); \
            return; \
        } \
    } while (0)

#define WANT_TOKENS_OR(ntokens, a, b) \
    do { \
        if (ntokens != a && ntokens != b) { \
            out_string(c, "ERROR"); \
            return; \
        } \
    } while (0)

#define WANT_TOKENS_MIN(ntokens, min) \
    do { \
        if (ntokens < min) { \
            out_string(c, "ERROR"); \
            return; \
        } \
    } while (0)

/*
 * Tokenize the command string by replacing whitespace with '\0' and update
 * the token array tokens with pointer to start of each token and length.
 * Returns total number of tokens.  The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero).
 *
 * Usage example:
 *
 *  while(tokenize_command(command, ncommand, tokens, max_tokens) > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ...
 *      }
 *      ncommand = tokens[ix].value - command;
 *      command  = tokens[ix].value;
 *   }
 */
static size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens) {
    char *s, *e;
    size_t ntokens = 0;
    size_t len = strlen(command);
    unsigned int i = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    s = e = command;
    for (i = 0; i < len; i++) {
        if (*e == ' ') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
                if (ntokens == max_tokens - 1) {
                    e++;
                    s = e; /* so we don't add an extra token */
                    break;
                }
            }
            s = e + 1;
        }
        e++;
    }

    if (s != e) {
        tokens[ntokens].value = s;
        tokens[ntokens].length = e - s;
        ntokens++;
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value =  *e == '\0' ? NULL : e;
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

/* set up a connection to write a buffer then free it, used for stats */
static void write_and_free(conn *c, char *buf, int bytes) {
    if (buf) {
        mc_resp *resp = c->resp;
        resp->write_and_free = buf;
        resp_add_iov(resp, buf, bytes);
        conn_set_state(c, conn_new_cmd);
    } else {
        out_of_memory(c, "SERVER_ERROR out of memory writing stats");
    }
}

static inline bool set_noreply_maybe(conn *c, token_t *tokens, size_t ntokens)
{
    int noreply_index = ntokens - 2;

    /*
      NOTE: this function is not the first place where we are going to
      send the reply.  We could send it instead from process_command()
      if the request line has wrong number of tokens.  However parsing
      malformed line for "noreply" option is not reliable anyway, so
      it can't be helped.
    */
    if (tokens[noreply_index].value
        && strcmp(tokens[noreply_index].value, "noreply") == 0) {
        c->noreply = true;
    }
    return c->noreply;
}

void append_stat(const char *name, ADD_STAT add_stats, conn *c,
                 const char *fmt, ...) {
    char val_str[STAT_VAL_LEN];
    int vlen;
    va_list ap;

    assert(name);
    assert(add_stats);
    assert(c);
    assert(fmt);

    va_start(ap, fmt);
    vlen = vsnprintf(val_str, sizeof(val_str) - 1, fmt, ap);
    va_end(ap);

    add_stats(name, strlen(name), val_str, vlen, c);
}

inline static void process_stats_detail(conn *c, const char *command) {
    assert(c != NULL);

    if (strcmp(command, "on") == 0) {
        settings.detail_enabled = 1;
        out_string(c, "OK");
    }
    else if (strcmp(command, "off") == 0) {
        settings.detail_enabled = 0;
        out_string(c, "OK");
    }
    else if (strcmp(command, "dump") == 0) {
        int len;
        char *stats = stats_prefix_dump(&len);
        write_and_free(c, stats, len);
    }
    else {
        out_string(c, "CLIENT_ERROR usage: stats detail on|off|dump");
    }
}

/* return server specific stats only */
static void server_stats(ADD_STAT add_stats, conn *c) {
    pid_t pid = getpid();
    rel_time_t now = current_time;

    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);
    struct slab_stats slab_stats;
    slab_stats_aggregate(&thread_stats, &slab_stats);
#ifdef EXTSTORE
    struct extstore_stats st;
#endif
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);

    STATS_LOCK();

    APPEND_STAT("pid", "%lu", (long)pid);
    APPEND_STAT("uptime", "%u", now - ITEM_UPDATE_INTERVAL);
    APPEND_STAT("time", "%ld", now + (long)process_started);
    APPEND_STAT("version", "%s", VERSION);
    APPEND_STAT("libevent", "%s", event_get_version());
    APPEND_STAT("pointer_size", "%d", (int)(8 * sizeof(void *)));

    append_stat("rusage_user", add_stats, c, "%ld.%06ld",
                (long)usage.ru_utime.tv_sec,
                (long)usage.ru_utime.tv_usec);
    append_stat("rusage_system", add_stats, c, "%ld.%06ld",
                (long)usage.ru_stime.tv_sec,
                (long)usage.ru_stime.tv_usec);

    APPEND_STAT("max_connections", "%d", settings.maxconns);
    APPEND_STAT("curr_connections", "%llu", (unsigned long long)stats_state.curr_conns - 1);
    APPEND_STAT("total_connections", "%llu", (unsigned long long)stats.total_conns);
    if (settings.maxconns_fast) {
        APPEND_STAT("rejected_connections", "%llu", (unsigned long long)stats.rejected_conns);
    }
#ifndef SFD_VALUE_IS_RANDOM
    APPEND_STAT("connection_structures", "%u", stats_state.conn_structs);
#else
    APPEND_STAT("connection_structures", "%u", conn_list_alloc_count());
#endif /* #ifdef SFD_VALUE_IS_RANDOM */
    APPEND_STAT("response_obj_bytes", "%llu", (unsigned long long)thread_stats.response_obj_bytes);
    APPEND_STAT("response_obj_total", "%llu", (unsigned long long)thread_stats.response_obj_total);
    APPEND_STAT("response_obj_free", "%llu", (unsigned long long)thread_stats.response_obj_free);
    APPEND_STAT("response_obj_oom", "%llu", (unsigned long long)thread_stats.response_obj_oom);
    APPEND_STAT("read_buf_bytes", "%llu", (unsigned long long)thread_stats.read_buf_bytes);
    APPEND_STAT("read_buf_bytes_free", "%llu", (unsigned long long)thread_stats.read_buf_bytes_free);
    APPEND_STAT("read_buf_oom", "%llu", (unsigned long long)thread_stats.read_buf_oom);
    APPEND_STAT("reserved_fds", "%u", stats_state.reserved_fds);
    APPEND_STAT("cmd_get", "%llu", (unsigned long long)thread_stats.get_cmds);
    APPEND_STAT("cmd_set", "%llu", (unsigned long long)slab_stats.set_cmds);
    APPEND_STAT("cmd_flush", "%llu", (unsigned long long)thread_stats.flush_cmds);
    APPEND_STAT("cmd_touch", "%llu", (unsigned long long)thread_stats.touch_cmds);
    APPEND_STAT("cmd_meta", "%llu", (unsigned long long)thread_stats.meta_cmds);
    APPEND_STAT("get_hits", "%llu", (unsigned long long)slab_stats.get_hits);
    APPEND_STAT("get_misses", "%llu", (unsigned long long)thread_stats.get_misses);
    APPEND_STAT("get_expired", "%llu", (unsigned long long)thread_stats.get_expired);
    APPEND_STAT("get_flushed", "%llu", (unsigned long long)thread_stats.get_flushed);
#ifdef EXTSTORE
    if (c->thread->storage) {
        APPEND_STAT("get_extstore", "%llu", (unsigned long long)thread_stats.get_extstore);
        APPEND_STAT("get_aborted_extstore", "%llu", (unsigned long long)thread_stats.get_aborted_extstore);
        APPEND_STAT("get_oom_extstore", "%llu", (unsigned long long)thread_stats.get_oom_extstore);
        APPEND_STAT("recache_from_extstore", "%llu", (unsigned long long)thread_stats.recache_from_extstore);
        APPEND_STAT("miss_from_extstore", "%llu", (unsigned long long)thread_stats.miss_from_extstore);
        APPEND_STAT("badcrc_from_extstore", "%llu", (unsigned long long)thread_stats.badcrc_from_extstore);
    }
#endif
    APPEND_STAT("delete_misses", "%llu", (unsigned long long)thread_stats.delete_misses);
    APPEND_STAT("delete_hits", "%llu", (unsigned long long)slab_stats.delete_hits);
    APPEND_STAT("incr_misses", "%llu", (unsigned long long)thread_stats.incr_misses);
    APPEND_STAT("incr_hits", "%llu", (unsigned long long)slab_stats.incr_hits);
    APPEND_STAT("decr_misses", "%llu", (unsigned long long)thread_stats.decr_misses);
    APPEND_STAT("decr_hits", "%llu", (unsigned long long)slab_stats.decr_hits);
    APPEND_STAT("cas_misses", "%llu", (unsigned long long)thread_stats.cas_misses);
    APPEND_STAT("cas_hits", "%llu", (unsigned long long)slab_stats.cas_hits);
    APPEND_STAT("cas_badval", "%llu", (unsigned long long)slab_stats.cas_badval);
    APPEND_STAT("touch_hits", "%llu", (unsigned long long)slab_stats.touch_hits);
    APPEND_STAT("touch_misses", "%llu", (unsigned long long)thread_stats.touch_misses);
    APPEND_STAT("auth_cmds", "%llu", (unsigned long long)thread_stats.auth_cmds);
    APPEND_STAT("auth_errors", "%llu", (unsigned long long)thread_stats.auth_errors);
    if (settings.idle_timeout) {
        APPEND_STAT("idle_kicks", "%llu", (unsigned long long)thread_stats.idle_kicks);
    }
    APPEND_STAT("bytes_read", "%llu", (unsigned long long)thread_stats.bytes_read);
    APPEND_STAT("bytes_written", "%llu", (unsigned long long)thread_stats.bytes_written);
    APPEND_STAT("limit_maxbytes", "%llu", (unsigned long long)settings.maxbytes);
    APPEND_STAT("accepting_conns", "%u", stats_state.accepting_conns);
    APPEND_STAT("listen_disabled_num", "%llu", (unsigned long long)stats.listen_disabled_num);
    APPEND_STAT("time_in_listen_disabled_us", "%llu", stats.time_in_listen_disabled_us);
    APPEND_STAT("threads", "%d", settings.num_threads);
    APPEND_STAT("conn_yields", "%llu", (unsigned long long)thread_stats.conn_yields);
    APPEND_STAT("hash_power_level", "%u", stats_state.hash_power_level);
    APPEND_STAT("hash_bytes", "%llu", (unsigned long long)stats_state.hash_bytes);
    APPEND_STAT("hash_is_expanding", "%u", stats_state.hash_is_expanding);
    if (settings.slab_reassign) {
        APPEND_STAT("slab_reassign_rescues", "%llu", stats.slab_reassign_rescues);
        APPEND_STAT("slab_reassign_chunk_rescues", "%llu", stats.slab_reassign_chunk_rescues);
        APPEND_STAT("slab_reassign_evictions_nomem", "%llu", stats.slab_reassign_evictions_nomem);
        APPEND_STAT("slab_reassign_inline_reclaim", "%llu", stats.slab_reassign_inline_reclaim);
        APPEND_STAT("slab_reassign_busy_items", "%llu", stats.slab_reassign_busy_items);
        APPEND_STAT("slab_reassign_busy_deletes", "%llu", stats.slab_reassign_busy_deletes);
        APPEND_STAT("slab_reassign_running", "%u", stats_state.slab_reassign_running);
        APPEND_STAT("slabs_moved", "%llu", stats.slabs_moved);
    }
    if (settings.lru_crawler) {
        APPEND_STAT("lru_crawler_running", "%u", stats_state.lru_crawler_running);
        APPEND_STAT("lru_crawler_starts", "%u", stats.lru_crawler_starts);
    }
    if (settings.lru_maintainer_thread) {
        APPEND_STAT("lru_maintainer_juggles", "%llu", (unsigned long long)stats.lru_maintainer_juggles);
    }
    APPEND_STAT("malloc_fails", "%llu",
                (unsigned long long)stats.malloc_fails);
    APPEND_STAT("log_worker_dropped", "%llu", (unsigned long long)stats.log_worker_dropped);
    APPEND_STAT("log_worker_written", "%llu", (unsigned long long)stats.log_worker_written);
    APPEND_STAT("log_watcher_skipped", "%llu", (unsigned long long)stats.log_watcher_skipped);
    APPEND_STAT("log_watcher_sent", "%llu", (unsigned long long)stats.log_watcher_sent);
    STATS_UNLOCK();
#ifdef EXTSTORE
    if (c->thread->storage) {
        STATS_LOCK();
        APPEND_STAT("extstore_compact_lost", "%llu", (unsigned long long)stats.extstore_compact_lost);
        APPEND_STAT("extstore_compact_rescues", "%llu", (unsigned long long)stats.extstore_compact_rescues);
        APPEND_STAT("extstore_compact_skipped", "%llu", (unsigned long long)stats.extstore_compact_skipped);
        STATS_UNLOCK();
        extstore_get_stats(c->thread->storage, &st);
        APPEND_STAT("extstore_page_allocs", "%llu", (unsigned long long)st.page_allocs);
        APPEND_STAT("extstore_page_evictions", "%llu", (unsigned long long)st.page_evictions);
        APPEND_STAT("extstore_page_reclaims", "%llu", (unsigned long long)st.page_reclaims);
        APPEND_STAT("extstore_pages_free", "%llu", (unsigned long long)st.pages_free);
        APPEND_STAT("extstore_pages_used", "%llu", (unsigned long long)st.pages_used);
        APPEND_STAT("extstore_objects_evicted", "%llu", (unsigned long long)st.objects_evicted);
        APPEND_STAT("extstore_objects_read", "%llu", (unsigned long long)st.objects_read);
        APPEND_STAT("extstore_objects_written", "%llu", (unsigned long long)st.objects_written);
        APPEND_STAT("extstore_objects_used", "%llu", (unsigned long long)st.objects_used);
        APPEND_STAT("extstore_bytes_evicted", "%llu", (unsigned long long)st.bytes_evicted);
        APPEND_STAT("extstore_bytes_written", "%llu", (unsigned long long)st.bytes_written);
        APPEND_STAT("extstore_bytes_read", "%llu", (unsigned long long)st.bytes_read);
        APPEND_STAT("extstore_bytes_used", "%llu", (unsigned long long)st.bytes_used);
        APPEND_STAT("extstore_bytes_fragmented", "%llu", (unsigned long long)st.bytes_fragmented);
        APPEND_STAT("extstore_limit_maxbytes", "%llu", (unsigned long long)(st.page_count * st.page_size));
        APPEND_STAT("extstore_io_queue", "%llu", (unsigned long long)(st.io_queue));
    }
#endif
#ifdef TLS
    if (settings.ssl_enabled) {
        if (settings.ssl_session_cache) {
            APPEND_STAT("ssl_new_sessions", "%llu", (unsigned long long)stats.ssl_new_sessions);
        }
        APPEND_STAT("ssl_handshake_errors", "%llu", (unsigned long long)stats.ssl_handshake_errors);
        APPEND_STAT("time_since_server_cert_refresh", "%u", now - settings.ssl_last_cert_refresh_time);
    }
#endif
}

static void process_stat_settings(ADD_STAT add_stats, void *c) {
    assert(add_stats);
    APPEND_STAT("maxbytes", "%llu", (unsigned long long)settings.maxbytes);
    APPEND_STAT("maxconns", "%d", settings.maxconns);
    APPEND_STAT("tcpport", "%d", settings.port);
    APPEND_STAT("udpport", "%d", settings.udpport);
    APPEND_STAT("inter", "%s", settings.inter ? settings.inter : "NULL");
    APPEND_STAT("verbosity", "%d", settings.verbose);
    APPEND_STAT("oldest", "%lu", (unsigned long)settings.oldest_live);
    APPEND_STAT("evictions", "%s", settings.evict_to_free ? "on" : "off");
    APPEND_STAT("domain_socket", "%s",
                settings.socketpath ? settings.socketpath : "NULL");
    APPEND_STAT("umask", "%o", settings.access);
    APPEND_STAT("growth_factor", "%.2f", settings.factor);
    APPEND_STAT("chunk_size", "%d", settings.chunk_size);
    APPEND_STAT("num_threads", "%d", settings.num_threads);
    APPEND_STAT("num_threads_per_udp", "%d", settings.num_threads_per_udp);
    APPEND_STAT("stat_key_prefix", "%c", settings.prefix_delimiter);
    APPEND_STAT("detail_enabled", "%s",
                settings.detail_enabled ? "yes" : "no");
    APPEND_STAT("reqs_per_event", "%d", settings.reqs_per_event);
    APPEND_STAT("cas_enabled", "%s", settings.use_cas ? "yes" : "no");
    APPEND_STAT("tcp_backlog", "%d", settings.backlog);
    APPEND_STAT("binding_protocol", "%s",
                prot_text(settings.binding_protocol));
    APPEND_STAT("auth_enabled_sasl", "%s", settings.sasl ? "yes" : "no");
    APPEND_STAT("auth_enabled_ascii", "%s", settings.auth_file ? settings.auth_file : "no");
    APPEND_STAT("item_size_max", "%d", settings.item_size_max);
    APPEND_STAT("maxconns_fast", "%s", settings.maxconns_fast ? "yes" : "no");
    APPEND_STAT("hashpower_init", "%d", settings.hashpower_init);
    APPEND_STAT("slab_reassign", "%s", settings.slab_reassign ? "yes" : "no");
    APPEND_STAT("slab_automove", "%d", settings.slab_automove);
    APPEND_STAT("slab_automove_ratio", "%.2f", settings.slab_automove_ratio);
    APPEND_STAT("slab_automove_window", "%u", settings.slab_automove_window);
    APPEND_STAT("slab_chunk_max", "%d", settings.slab_chunk_size_max);
    APPEND_STAT("lru_crawler", "%s", settings.lru_crawler ? "yes" : "no");
    APPEND_STAT("lru_crawler_sleep", "%d", settings.lru_crawler_sleep);
    APPEND_STAT("lru_crawler_tocrawl", "%lu", (unsigned long)settings.lru_crawler_tocrawl);
    APPEND_STAT("tail_repair_time", "%d", settings.tail_repair_time);
    APPEND_STAT("flush_enabled", "%s", settings.flush_enabled ? "yes" : "no");
    APPEND_STAT("dump_enabled", "%s", settings.dump_enabled ? "yes" : "no");
    APPEND_STAT("hash_algorithm", "%s", settings.hash_algorithm);
    APPEND_STAT("lru_maintainer_thread", "%s", settings.lru_maintainer_thread ? "yes" : "no");
    APPEND_STAT("lru_segmented", "%s", settings.lru_segmented ? "yes" : "no");
    APPEND_STAT("hot_lru_pct", "%d", settings.hot_lru_pct);
    APPEND_STAT("warm_lru_pct", "%d", settings.warm_lru_pct);
    APPEND_STAT("hot_max_factor", "%.2f", settings.hot_max_factor);
    APPEND_STAT("warm_max_factor", "%.2f", settings.warm_max_factor);
    APPEND_STAT("temp_lru", "%s", settings.temp_lru ? "yes" : "no");
    APPEND_STAT("temporary_ttl", "%u", settings.temporary_ttl);
    APPEND_STAT("idle_timeout", "%d", settings.idle_timeout);
    APPEND_STAT("watcher_logbuf_size", "%u", settings.logger_watcher_buf_size);
    APPEND_STAT("worker_logbuf_size", "%u", settings.logger_buf_size);
    APPEND_STAT("resp_obj_mem_limit", "%u", settings.resp_obj_mem_limit);
    APPEND_STAT("read_buf_mem_limit", "%u", settings.read_buf_mem_limit);
    APPEND_STAT("track_sizes", "%s", item_stats_sizes_status() ? "yes" : "no");
    APPEND_STAT("inline_ascii_response", "%s", "no"); // setting is dead, cannot be yes.
#ifdef HAVE_DROP_PRIVILEGES
    APPEND_STAT("drop_privileges", "%s", settings.drop_privileges ? "yes" : "no");
#endif
#ifdef EXTSTORE
    APPEND_STAT("ext_item_size", "%u", settings.ext_item_size);
    APPEND_STAT("ext_item_age", "%u", settings.ext_item_age);
    APPEND_STAT("ext_low_ttl", "%u", settings.ext_low_ttl);
    APPEND_STAT("ext_recache_rate", "%u", settings.ext_recache_rate);
    APPEND_STAT("ext_wbuf_size", "%u", settings.ext_wbuf_size);
    APPEND_STAT("ext_compact_under", "%u", settings.ext_compact_under);
    APPEND_STAT("ext_drop_under", "%u", settings.ext_drop_under);
    APPEND_STAT("ext_max_frag", "%.2f", settings.ext_max_frag);
    APPEND_STAT("slab_automove_freeratio", "%.3f", settings.slab_automove_freeratio);
    APPEND_STAT("ext_drop_unread", "%s", settings.ext_drop_unread ? "yes" : "no");
#endif
#ifdef TLS
    APPEND_STAT("ssl_enabled", "%s", settings.ssl_enabled ? "yes" : "no");
    APPEND_STAT("ssl_chain_cert", "%s", settings.ssl_chain_cert);
    APPEND_STAT("ssl_key", "%s", settings.ssl_key);
    APPEND_STAT("ssl_verify_mode", "%d", settings.ssl_verify_mode);
    APPEND_STAT("ssl_keyformat", "%d", settings.ssl_keyformat);
    APPEND_STAT("ssl_ciphers", "%s", settings.ssl_ciphers ? settings.ssl_ciphers : "NULL");
    APPEND_STAT("ssl_ca_cert", "%s", settings.ssl_ca_cert ? settings.ssl_ca_cert : "NULL");
    APPEND_STAT("ssl_wbuf_size", "%u", settings.ssl_wbuf_size);
    APPEND_STAT("ssl_session_cache", "%s", settings.ssl_session_cache ? "yes" : "no");
#endif
}

static int nz_strcmp(int nzlength, const char *nz, const char *z) {
    int zlength=strlen(z);
    return (zlength == nzlength) && (strncmp(nz, z, zlength) == 0) ? 0 : -1;
}

static bool get_stats(const char *stat_type, int nkey, ADD_STAT add_stats, void *c) {
    bool ret = true;

    if (add_stats != NULL) {
        if (!stat_type) {
            /* prepare general statistics for the engine */
            STATS_LOCK();
            APPEND_STAT("bytes", "%llu", (unsigned long long)stats_state.curr_bytes);
            APPEND_STAT("curr_items", "%llu", (unsigned long long)stats_state.curr_items);
            APPEND_STAT("total_items", "%llu", (unsigned long long)stats.total_items);
            STATS_UNLOCK();
            APPEND_STAT("slab_global_page_pool", "%u", global_page_pool_size(NULL));
            item_stats_totals(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "items") == 0) {
            item_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "slabs") == 0) {
            slabs_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes") == 0) {
            item_stats_sizes(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes_enable") == 0) {
            item_stats_sizes_enable(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes_disable") == 0) {
            item_stats_sizes_disable(add_stats, c);
        } else {
            ret = false;
        }
    } else {
        ret = false;
    }

    return ret;
}

static inline void get_conn_text(const conn *c, const int af,
                char* addr, struct sockaddr *sock_addr) {
    char addr_text[MAXPATHLEN];
    addr_text[0] = '\0';
    const char *protoname = "?";
    unsigned short port = 0;

    switch (af) {
        case AF_INET:
            (void) inet_ntop(af,
                    &((struct sockaddr_in *)sock_addr)->sin_addr,
                    addr_text,
                    sizeof(addr_text) - 1);
            port = ntohs(((struct sockaddr_in *)sock_addr)->sin_port);
            protoname = IS_UDP(c->transport) ? "udp" : "tcp";
            break;

        case AF_INET6:
            addr_text[0] = '[';
            addr_text[1] = '\0';
            if (inet_ntop(af,
                    &((struct sockaddr_in6 *)sock_addr)->sin6_addr,
                    addr_text + 1,
                    sizeof(addr_text) - 2)) {
                strcat(addr_text, "]");
            }
            port = ntohs(((struct sockaddr_in6 *)sock_addr)->sin6_port);
            protoname = IS_UDP(c->transport) ? "udp6" : "tcp6";
            break;

#ifndef DISABLE_UNIX_SOCKET
        case AF_UNIX:
        {
            size_t pathlen = 0;
            // this strncpy call originally could piss off an address
            // sanitizer; we supplied the size of the dest buf as a limiter,
            // but optimized versions of strncpy could read past the end of
            // *src while looking for a null terminator. Since buf and
            // sun_path here are both on the stack they could even overlap,
            // which is "undefined". In all OSS versions of strncpy I could
            // find this has no effect; it'll still only copy until the first null
            // terminator is found. Thus it's possible to get the OS to
            // examine past the end of sun_path but it's unclear to me if this
            // can cause any actual problem.
            //
            // We need a safe_strncpy util function but I'll punt on figuring
            // that out for now.
            pathlen = sizeof(((struct sockaddr_un *)sock_addr)->sun_path);
            if (MAXPATHLEN <= pathlen) {
                pathlen = MAXPATHLEN - 1;
            }
            strncpy(addr_text,
                    ((struct sockaddr_un *)sock_addr)->sun_path,
                    pathlen);
            addr_text[pathlen] = '\0';
            protoname = "unix";
        }
            break;
#endif /* #ifndef DISABLE_UNIX_SOCKET */
    }

    if (strlen(addr_text) < 2) {
        /* Most likely this is a connected UNIX-domain client which
         * has no peer socket address, but there's no portable way
         * to tell for sure.
         */
        sprintf(addr_text, "<AF %d>", af);
    }

    if (port) {
        sprintf(addr, "%s:%s:%u", protoname, addr_text, port);
    } else {
        sprintf(addr, "%s:%s", protoname, addr_text);
    }
}

static void conn_to_str(const conn *c, char *addr, char *svr_addr) {
    if (!c) {
        strcpy(addr, "<null>");
    } else if (c->state == conn_closed) {
        strcpy(addr, "<closed>");
    } else {
        struct sockaddr_in6 local_addr;
        struct sockaddr *sock_addr = (void *)&c->request_addr;

        /* For listen ports and idle UDP ports, show listen address */
        if (c->state == conn_listening ||
                (IS_UDP(c->transport) &&
                 c->state == conn_read)) {
            socklen_t local_addr_len = sizeof(local_addr);

            if (getsockname(c->sfd,
                        (struct sockaddr *)&local_addr,
                        &local_addr_len) == 0) {
                sock_addr = (struct sockaddr *)&local_addr;
            }
        }
        get_conn_text(c, sock_addr->sa_family, addr, sock_addr);

        if (c->state != conn_listening && !(IS_UDP(c->transport) &&
                 c->state == conn_read)) {
            struct sockaddr_storage svr_sock_addr;
            socklen_t svr_addr_len = sizeof(svr_sock_addr);
            getsockname(c->sfd, (struct sockaddr *)&svr_sock_addr, &svr_addr_len);
            get_conn_text(c, svr_sock_addr.ss_family, svr_addr, (struct sockaddr *)&svr_sock_addr);
        }
    }
}

static void process_stats_conns(ADD_STAT add_stats, void *c) {
    int i;
    char key_str[STAT_KEY_LEN];
    char val_str[STAT_VAL_LEN];
    size_t extras_len = sizeof("unix:") + sizeof("65535");
    char addr[MAXPATHLEN + extras_len];
    char svr_addr[MAXPATHLEN + extras_len];
    int klen = 0, vlen = 0;

    assert(add_stats);

    for (i = 0; i < max_fds; i++) {
        if (conns[i]) {
            /* This is safe to do unlocked because conns are never freed; the
             * worst that'll happen will be a minor inconsistency in the
             * output -- not worth the complexity of the locking that'd be
             * required to prevent it.
             */
            if (IS_UDP(conns[i]->transport)) {
                APPEND_NUM_STAT(i, "UDP", "%s", "UDP");
            }
            if (conns[i]->state != conn_closed) {
                conn_to_str(conns[i], addr, svr_addr);

                APPEND_NUM_STAT(i, "addr", "%s", addr);
                if (conns[i]->state != conn_listening &&
                    !(IS_UDP(conns[i]->transport) && conns[i]->state == conn_read)) {
                    APPEND_NUM_STAT(i, "listen_addr", "%s", svr_addr);
                }
                APPEND_NUM_STAT(i, "state", "%s",
                        state_text(conns[i]->state));
                APPEND_NUM_STAT(i, "secs_since_last_cmd", "%d",
                        current_time - conns[i]->last_cmd_time);
            }
        }
    }
}
#ifdef EXTSTORE
static void process_extstore_stats(ADD_STAT add_stats, conn *c) {
    int i;
    char key_str[STAT_KEY_LEN];
    char val_str[STAT_VAL_LEN];
    int klen = 0, vlen = 0;
    struct extstore_stats st;

    assert(add_stats);

    void *storage = c->thread->storage;
    extstore_get_stats(storage, &st);
    st.page_data = calloc(st.page_count, sizeof(struct extstore_page_data));
    extstore_get_page_data(storage, &st);

    for (i = 0; i < st.page_count; i++) {
        APPEND_NUM_STAT(i, "version", "%llu",
                (unsigned long long) st.page_data[i].version);
        APPEND_NUM_STAT(i, "bytes", "%llu",
                (unsigned long long) st.page_data[i].bytes_used);
        APPEND_NUM_STAT(i, "bucket", "%u",
                st.page_data[i].bucket);
        APPEND_NUM_STAT(i, "free_bucket", "%u",
                st.page_data[i].free_bucket);
    }
}
#endif
static void process_stat(conn *c, token_t *tokens, const size_t ntokens) {
    const char *subcommand = tokens[SUBCOMMAND_TOKEN].value;
    assert(c != NULL);

    if (ntokens < 2) {
        out_string(c, "CLIENT_ERROR bad command line");
        return;
    }

    if (ntokens == 2) {
        server_stats(&append_stats, c);
        (void)get_stats(NULL, 0, &append_stats, c);
    } else if (strcmp(subcommand, "reset") == 0) {
        stats_reset();
        out_string(c, "RESET");
        return;
    } else if (strcmp(subcommand, "detail") == 0) {
        /* NOTE: how to tackle detail with binary? */
        if (ntokens < 4)
            process_stats_detail(c, "");  /* outputs the error message */
        else
            process_stats_detail(c, tokens[2].value);
        /* Output already generated */
        return;
    } else if (strcmp(subcommand, "settings") == 0) {
        process_stat_settings(&append_stats, c);
    } else if (strcmp(subcommand, "cachedump") == 0) {
        char *buf;
        unsigned int bytes, id, limit = 0;

        if (!settings.dump_enabled) {
            out_string(c, "CLIENT_ERROR stats cachedump not allowed");
            return;
        }

        if (ntokens < 5) {
            out_string(c, "CLIENT_ERROR bad command line");
            return;
        }

        if (!safe_strtoul(tokens[2].value, &id) ||
            !safe_strtoul(tokens[3].value, &limit)) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }

        if (id >= MAX_NUMBER_OF_SLAB_CLASSES) {
            out_string(c, "CLIENT_ERROR Illegal slab id");
            return;
        }

        buf = item_cachedump(id, limit, &bytes);
        write_and_free(c, buf, bytes);
        return;
    } else if (strcmp(subcommand, "conns") == 0) {
        process_stats_conns(&append_stats, c);
#ifdef EXTSTORE
    } else if (strcmp(subcommand, "extstore") == 0) {
        process_extstore_stats(&append_stats, c);
#endif
    } else {
        /* getting here means that the subcommand is either engine specific or
           is invalid. query the engine and see. */
        if (get_stats(subcommand, strlen(subcommand), &append_stats, c)) {
            if (c->stats.buffer == NULL) {
                out_of_memory(c, "SERVER_ERROR out of memory writing stats");
            } else {
                write_and_free(c, c->stats.buffer, c->stats.offset);
                c->stats.buffer = NULL;
            }
        } else {
            out_string(c, "ERROR");
        }
        return;
    }

    /* append terminator and start the transfer */
    append_stats(NULL, 0, NULL, 0, c);

    if (c->stats.buffer == NULL) {
        out_of_memory(c, "SERVER_ERROR out of memory writing stats");
    } else {
        write_and_free(c, c->stats.buffer, c->stats.offset);
        c->stats.buffer = NULL;
    }
}

/* client flags == 0 means use no storage for client flags */
static inline int make_ascii_get_suffix(char *suffix, item *it, bool return_cas, int nbytes) {
    char *p = suffix;
    *p = ' ';
    p++;
    if (FLAGS_SIZE(it) == 0) {
        *p = '0';
        p++;
    } else {
        p = itoa_u32(*((uint32_t *) ITEM_suffix(it)), p);
    }
    *p = ' ';
    p = itoa_u32(nbytes-2, p+1);

    if (return_cas) {
        *p = ' ';
        p = itoa_u64(ITEM_get_cas(it), p+1);
    }

    *p = '\r';
    *(p+1) = '\n';
    *(p+2) = '\0';
    return (p - suffix) + 2;
}

#define IT_REFCOUNT_LIMIT 60000
static inline item* limited_get(char *key, size_t nkey, conn *c, uint32_t exptime, bool should_touch, bool do_update, bool *overflow) {
    item *it;
    if (should_touch) {
        it = item_touch(key, nkey, exptime, c);
    } else {
        it = item_get(key, nkey, c, do_update);
    }
    if (it && it->refcount > IT_REFCOUNT_LIMIT) {
        item_remove(it);
        it = NULL;
        *overflow = true;
    } else {
        *overflow = false;
    }
    return it;
}

// Semantics are different than limited_get; since the item is returned
// locked, caller can directly change what it needs.
// though it might eventually be a better interface to sink it all into
// items.c.
static inline item* limited_get_locked(char *key, size_t nkey, conn *c, bool do_update, uint32_t *hv, bool *overflow) {
    item *it;
    it = item_get_locked(key, nkey, c, do_update, hv);
    if (it && it->refcount > IT_REFCOUNT_LIMIT) {
        do_item_remove(it);
        it = NULL;
        item_unlock(*hv);
        *overflow = true;
    } else {
        *overflow = false;
    }
    return it;
}

#ifdef EXTSTORE
// FIXME: This runs in the IO thread. to get better IO performance this should
// simply mark the io wrapper with the return value and decrement wrapleft, if
// zero redispatching. Still a bit of work being done in the side thread but
// minimized at least.
static void _get_extstore_cb(void *e, obj_io *io, int ret) {
    // FIXME: assumes success
    io_wrap *wrap = (io_wrap *)io->data;
    mc_resp *resp = wrap->resp;
    conn *c = wrap->c;
    assert(wrap->active == true);
    item *read_it = (item *)io->buf;
    bool miss = false;

    // TODO: How to do counters for hit/misses?
    if (ret < 1) {
        miss = true;
    } else {
        uint32_t crc2;
        uint32_t crc = (uint32_t) read_it->exptime;
        int x;
        // item is chunked, crc the iov's
        if (io->iov != NULL) {
            // first iov is the header, which we don't use beyond crc
            crc2 = crc32c(0, (char *)io->iov[0].iov_base+STORE_OFFSET, io->iov[0].iov_len-STORE_OFFSET);
            // make sure it's not sent. hack :(
            io->iov[0].iov_len = 0;
            for (x = 1; x < io->iovcnt; x++) {
                crc2 = crc32c(crc2, (char *)io->iov[x].iov_base, io->iov[x].iov_len);
            }
        } else {
            crc2 = crc32c(0, (char *)read_it+STORE_OFFSET, io->len-STORE_OFFSET);
        }

        if (crc != crc2) {
            miss = true;
            wrap->badcrc = true;
        }
    }

    if (miss) {
        if (wrap->noreply) {
            // In all GET cases, noreply means we send nothing back.
            resp->skip = true;
        } else {
            // TODO: This should be movable to the worker thread.
            // Convert the binprot response into a miss response.
            // The header requires knowing a bunch of stateful crap, so rather
            // than simply writing out a "new" miss response we mangle what's
            // already there.
            if (c->protocol == binary_prot) {
                protocol_binary_response_header *header =
                    (protocol_binary_response_header *)resp->wbuf;

                // cut the extra nbytes off of the body_len
                uint32_t body_len = ntohl(header->response.bodylen);
                uint8_t hdr_len = header->response.extlen;
                body_len -= resp->iov[wrap->iovec_data].iov_len + hdr_len;
                resp->tosend -= resp->iov[wrap->iovec_data].iov_len + hdr_len;
                header->response.extlen = 0;
                header->response.status = (uint16_t)htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
                header->response.bodylen = htonl(body_len);

                // truncate the data response.
                resp->iov[wrap->iovec_data].iov_len = 0;
                // wipe the extlen iov... wish it was just a flat buffer.
                resp->iov[wrap->iovec_data-1].iov_len = 0;
                resp->chunked_data_iov = 0;
            } else {
                int i;
                // Meta commands have EN status lines for miss, rather than
                // END as a trailer as per normal ascii.
                if (resp->iov[0].iov_len >= 3
                        && memcmp(resp->iov[0].iov_base, "VA ", 3) == 0) {
                    // TODO: These miss translators should use specific callback
                    // functions attached to the io wrap. This is weird :(
                    resp->iovcnt = 1;
                    resp->iov[0].iov_len = 4;
                    resp->iov[0].iov_base = "EN\r\n";
                    resp->tosend = 4;
                } else {
                    // Wipe the iovecs up through our data injection.
                    // Allows trailers to be returned (END)
                    for (i = 0; i <= wrap->iovec_data; i++) {
                        resp->tosend -= resp->iov[i].iov_len;
                        resp->iov[i].iov_len = 0;
                        resp->iov[i].iov_base = NULL;
                    }
                }
                resp->chunked_total = 0;
                resp->chunked_data_iov = 0;
            }
        }
        wrap->miss = true;
    } else {
        assert(read_it->slabs_clsid != 0);
        // TODO: should always use it instead of ITEM_data to kill more
        // chunked special casing.
        if ((read_it->it_flags & ITEM_CHUNKED) == 0) {
            resp->iov[wrap->iovec_data].iov_base = ITEM_data(read_it);
        }
        wrap->miss = false;
    }

    c->io_wrapleft--;
    wrap->active = false;
    //assert(c->io_wrapleft >= 0);

    // All IO's have returned, lets re-attach this connection to our original
    // thread.
    if (c->io_wrapleft == 0) {
        assert(c->io_queued == true);
        c->io_queued = false;
        redispatch_conn(c);
    }
}

static inline int _get_extstore(conn *c, item *it, mc_resp *resp) {
#ifdef NEED_ALIGN
    item_hdr hdr;
    memcpy(&hdr, ITEM_data(it), sizeof(hdr));
#else
    item_hdr *hdr = (item_hdr *)ITEM_data(it);
#endif
    size_t ntotal = ITEM_ntotal(it);
    unsigned int clsid = slabs_clsid(ntotal);
    item *new_it;
    bool chunked = false;
    if (ntotal > settings.slab_chunk_size_max) {
        // Pull a chunked item header.
        uint32_t flags;
        FLAGS_CONV(it, flags);
        new_it = item_alloc(ITEM_key(it), it->nkey, flags, it->exptime, it->nbytes);
        assert(new_it == NULL || (new_it->it_flags & ITEM_CHUNKED));
        chunked = true;
    } else {
        new_it = do_item_alloc_pull(ntotal, clsid);
    }
    if (new_it == NULL)
        return -1;
    assert(!c->io_queued); // FIXME: debugging.
    // so we can free the chunk on a miss
    new_it->slabs_clsid = clsid;

    io_wrap *io = do_cache_alloc(c->thread->io_cache);
    io->active = true;
    io->miss = false;
    io->badcrc = false;
    io->noreply = c->noreply;
    // io_wrap owns the reference for this object now.
    io->hdr_it = it;
    io->resp = resp;
    io->io.iov = NULL;

    // FIXME: error handling.
    if (chunked) {
        unsigned int ciovcnt = 0;
        size_t remain = new_it->nbytes;
        item_chunk *chunk = (item_chunk *) ITEM_schunk(new_it);
        // TODO: This might make sense as a _global_ cache vs a per-thread.
        // but we still can't load objects requiring > IOV_MAX iovs.
        // In the meantime, these objects are rare/slow enough that
        // malloc/freeing a statically sized object won't cause us much pain.
        io->io.iov = malloc(sizeof(struct iovec) * IOV_MAX);
        if (io->io.iov == NULL) {
            item_remove(new_it);
            do_cache_free(c->thread->io_cache, io);
            return -1;
        }

        // fill the header so we can get the full data + crc back.
        io->io.iov[0].iov_base = new_it;
        io->io.iov[0].iov_len = ITEM_ntotal(new_it) - new_it->nbytes;
        ciovcnt++;

        while (remain > 0) {
            chunk = do_item_alloc_chunk(chunk, remain);
            // FIXME: _pure evil_, silently erroring if item is too large.
            if (chunk == NULL || ciovcnt > IOV_MAX-1) {
                item_remove(new_it);
                free(io->io.iov);
                // TODO: wrapper function for freeing up an io wrap?
                io->io.iov = NULL;
                do_cache_free(c->thread->io_cache, io);
                return -1;
            }
            io->io.iov[ciovcnt].iov_base = chunk->data;
            io->io.iov[ciovcnt].iov_len = (remain < chunk->size) ? remain : chunk->size;
            chunk->used = (remain < chunk->size) ? remain : chunk->size;
            remain -= chunk->size;
            ciovcnt++;
        }

        io->io.iovcnt = ciovcnt;
    }

    // Chunked or non chunked we reserve a response iov here.
    io->iovec_data = resp->iovcnt;
    int iovtotal = (c->protocol == binary_prot) ? it->nbytes - 2 : it->nbytes;
    if (chunked) {
        resp_add_chunked_iov(resp, new_it, iovtotal);
    } else {
        resp_add_iov(resp, "", iovtotal);
    }

    io->io.buf = (void *)new_it;
    io->c = c;

    // We need to stack the sub-struct IO's together as well.
    if (c->io_wraplist) {
        io->io.next = &c->io_wraplist->io;
    } else {
        io->io.next = NULL;
    }

    // IO queue for this connection.
    io->next = c->io_wraplist;
    c->io_wraplist = io;
    assert(c->io_wrapleft >= 0);
    c->io_wrapleft++;
    // reference ourselves for the callback.
    io->io.data = (void *)io;

    // Now, fill in io->io based on what was in our header.
#ifdef NEED_ALIGN
    io->io.page_version = hdr.page_version;
    io->io.page_id = hdr.page_id;
    io->io.offset = hdr.offset;
#else
    io->io.page_version = hdr->page_version;
    io->io.page_id = hdr->page_id;
    io->io.offset = hdr->offset;
#endif
    io->io.len = ntotal;
    io->io.mode = OBJ_IO_READ;
    io->io.cb = _get_extstore_cb;

    //fprintf(stderr, "EXTSTORE: IO stacked %u\n", io->iovec_data);
    // FIXME: This stat needs to move to reflect # of flash hits vs misses
    // for now it's a good gauge on how often we request out to flash at
    // least.
    pthread_mutex_lock(&c->thread->stats.mutex);
    c->thread->stats.get_extstore++;
    pthread_mutex_unlock(&c->thread->stats.mutex);

    return 0;
}
#endif

/* ntokens is overwritten here... shrug.. */
static inline void process_get_command(conn *c, token_t *tokens, size_t ntokens, bool return_cas, bool should_touch) {
    char *key;
    size_t nkey;
    item *it;
    token_t *key_token = &tokens[KEY_TOKEN];
    int32_t exptime_int = 0;
    rel_time_t exptime = 0;
    bool fail_length = false;
    assert(c != NULL);
    mc_resp *resp = c->resp;

    if (should_touch) {
        // For get and touch commands, use first token as exptime
        if (!safe_strtol(tokens[1].value, &exptime_int)) {
            out_string(c, "CLIENT_ERROR invalid exptime argument");
            return;
        }
        key_token++;
        exptime = realtime(EXPTIME_TO_POSITIVE_TIME(exptime_int));
    }

    do {
        while(key_token->length != 0) {
            bool overflow; // not used here.
            key = key_token->value;
            nkey = key_token->length;

            if (nkey > KEY_MAX_LENGTH) {
                fail_length = true;
                goto stop;
            }

            it = limited_get(key, nkey, c, exptime, should_touch, DO_UPDATE, &overflow);
            if (settings.detail_enabled) {
                stats_prefix_record_get(key, nkey, NULL != it);
            }
            if (it) {
                /*
                 * Construct the response. Each hit adds three elements to the
                 * outgoing data list:
                 *   "VALUE "
                 *   key
                 *   " " + flags + " " + data length + "\r\n" + data (with \r\n)
                 */

                {
                  MEMCACHED_COMMAND_GET(c->sfd, ITEM_key(it), it->nkey,
                                        it->nbytes, ITEM_get_cas(it));
                  int nbytes = it->nbytes;;
                  nbytes = it->nbytes;
                  char *p = resp->wbuf;
                  memcpy(p, "VALUE ", 6);
                  p += 6;
                  memcpy(p, ITEM_key(it), it->nkey);
                  p += it->nkey;
                  p += make_ascii_get_suffix(p, it, return_cas, nbytes);
                  resp_add_iov(resp, resp->wbuf, p - resp->wbuf);

#ifdef EXTSTORE
                  if (it->it_flags & ITEM_HDR) {
                      if (_get_extstore(c, it, resp) != 0) {
                          pthread_mutex_lock(&c->thread->stats.mutex);
                          c->thread->stats.get_oom_extstore++;
                          pthread_mutex_unlock(&c->thread->stats.mutex);

                          item_remove(it);
                          goto stop;
                      }
                  } else if ((it->it_flags & ITEM_CHUNKED) == 0) {
                      resp_add_iov(resp, ITEM_data(it), it->nbytes);
                  } else {
                      resp_add_chunked_iov(resp, it, it->nbytes);
                  }
#else
                  if ((it->it_flags & ITEM_CHUNKED) == 0) {
                      resp_add_iov(resp, ITEM_data(it), it->nbytes);
                  } else {
                      resp_add_chunked_iov(resp, it, it->nbytes);
                  }
#endif
                }

                if (settings.verbose > 1) {
                    int ii;
                    fprintf(stderr, ">%d sending key ", c->sfd);
                    for (ii = 0; ii < it->nkey; ++ii) {
                        fprintf(stderr, "%c", key[ii]);
                    }
                    fprintf(stderr, "\n");
                }

                /* item_get() has incremented it->refcount for us */
                pthread_mutex_lock(&c->thread->stats.mutex);
                if (should_touch) {
                    c->thread->stats.touch_cmds++;
                    c->thread->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
                } else {
                    c->thread->stats.lru_hits[it->slabs_clsid]++;
                    c->thread->stats.get_cmds++;
                }
                pthread_mutex_unlock(&c->thread->stats.mutex);
#ifdef EXTSTORE
                /* If ITEM_HDR, an io_wrap owns the reference. */
                if ((it->it_flags & ITEM_HDR) == 0) {
                    resp->item = it;
                }
#else
                resp->item = it;
#endif
            } else {
                pthread_mutex_lock(&c->thread->stats.mutex);
                if (should_touch) {
                    c->thread->stats.touch_cmds++;
                    c->thread->stats.touch_misses++;
                } else {
                    c->thread->stats.get_misses++;
                    c->thread->stats.get_cmds++;
                }
                MEMCACHED_COMMAND_GET(c->sfd, key, nkey, -1, 0);
                pthread_mutex_unlock(&c->thread->stats.mutex);
            }

            key_token++;
            if (key_token->length != 0) {
                if (!resp_start(c)) {
                    goto stop;
                }
                resp = c->resp;
            }
        }

        /*
         * If the command string hasn't been fully processed, get the next set
         * of tokens.
         */
        if (key_token->value != NULL) {
            ntokens = tokenize_command(key_token->value, tokens, MAX_TOKENS);
            key_token = tokens;
            if (!resp_start(c)) {
                goto stop;
            }
            resp = c->resp;
        }
    } while(key_token->value != NULL);
stop:

    if (settings.verbose > 1)
        fprintf(stderr, ">%d END\n", c->sfd);

    /*
        If the loop was terminated because of out-of-memory, it is not
        reliable to add END\r\n to the buffer, because it might not end
        in \r\n. So we send SERVER_ERROR instead.
    */
    if (key_token->value != NULL) {
        // Kill any stacked responses we had.
        conn_release_items(c);
        // Start a new response object for the error message.
        if (!resp_start(c)) {
            // severe out of memory error.
            conn_set_state(c, conn_closing);
            return;
        }
        if (fail_length) {
            out_string(c, "CLIENT_ERROR bad command line format");
        } else {
            out_of_memory(c, "SERVER_ERROR out of memory writing get response");
        }
    } else {
        // Tag the end token onto the most recent response object.
        resp_add_iov(resp, "END\r\n", 5);
        conn_set_state(c, conn_mwrite);
    }
}

// slow snprintf for debugging purposes.
static void process_meta_command(conn *c, token_t *tokens, const size_t ntokens) {
    assert(c != NULL);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    char *key = tokens[KEY_TOKEN].value;
    size_t nkey = tokens[KEY_TOKEN].length;

    bool overflow; // not used here.
    item *it = limited_get(key, nkey, c, 0, false, DONT_UPDATE, &overflow);
    if (it) {
        mc_resp *resp = c->resp;
        size_t total = 0;
        size_t ret;
        // similar to out_string().
        memcpy(resp->wbuf, "ME ", 3);
        total += 3;
        memcpy(resp->wbuf + total, ITEM_key(it), it->nkey);
        total += it->nkey;
        resp->wbuf[total] = ' ';
        total++;

        ret = snprintf(resp->wbuf + total, WRITE_BUFFER_SIZE - (it->nkey + 12),
                "exp=%d la=%llu cas=%llu fetch=%s cls=%u size=%lu\r\n",
                (it->exptime == 0) ? -1 : (current_time - it->exptime),
                (unsigned long long)(current_time - it->time),
                (unsigned long long)ITEM_get_cas(it),
                (it->it_flags & ITEM_FETCHED) ? "yes" : "no",
                ITEM_clsid(it),
                (unsigned long) ITEM_ntotal(it));

        item_remove(it);
        resp->wbytes = total + ret;
        resp_add_iov(resp, resp->wbuf, resp->wbytes);
        conn_set_state(c, conn_new_cmd);
    } else {
        out_string(c, "EN");
    }
    pthread_mutex_lock(&c->thread->stats.mutex);
    c->thread->stats.meta_cmds++;
    pthread_mutex_unlock(&c->thread->stats.mutex);
}

#define MFLAG_MAX_OPT_LENGTH 20
#define MFLAG_MAX_OPAQUE_LENGTH 32

struct _meta_flags {
    unsigned int has_error :1; // flipped if we found an error during parsing.
    unsigned int no_update :1;
    unsigned int locked :1;
    unsigned int vivify :1;
    unsigned int la :1;
    unsigned int hit :1;
    unsigned int value :1;
    unsigned int set_stale :1;
    unsigned int no_reply :1;
    unsigned int has_cas :1;
    unsigned int new_ttl :1;
    char mode; // single character mode switch, common to ms/ma
    rel_time_t exptime;
    rel_time_t autoviv_exptime;
    rel_time_t recache_time;
    int32_t value_len;
    uint32_t client_flags;
    uint64_t req_cas_id;
    uint64_t delta; // ma
    uint64_t initial; // ma
};

static int _meta_flag_preparse(token_t *tokens, const size_t ntokens,
        struct _meta_flags *of, char **errstr) {
    unsigned int i;
    int32_t tmp_int;
    uint8_t seen[127] = {0};
    // Start just past the key token. Look at first character of each token.
    for (i = KEY_TOKEN+1; i < ntokens-1; i++) {
        uint8_t o = (uint8_t)tokens[i].value[0];
        // zero out repeat flags so we don't over-parse for return data.
        if (o >= 127 || seen[o] != 0) {
            *errstr = "CLIENT_ERROR duplicate flag";
            return -1;
        }
        seen[o] = 1;
        switch (o) {
            /* Negative exptimes can underflow and end up immortal. realtime() will
               immediately expire values that are greater than REALTIME_MAXDELTA, but less
               than process_started, so lets aim for that. */
            case 'N':
                of->locked = 1;
                of->vivify = 1;
                if (!safe_strtol(tokens[i].value+1, &tmp_int)) {
                    *errstr = "CLIENT_ERROR bad token in command line format";
                    of->has_error = 1;
                } else {
                    of->autoviv_exptime = realtime(EXPTIME_TO_POSITIVE_TIME(tmp_int));
                }
                break;
            case 'T':
                of->locked = 1;
                if (!safe_strtol(tokens[i].value+1, &tmp_int)) {
                    *errstr = "CLIENT_ERROR bad token in command line format";
                    of->has_error = 1;
                } else {
                    of->exptime = realtime(EXPTIME_TO_POSITIVE_TIME(tmp_int));
                    of->new_ttl = true;
                }
                break;
            case 'R':
                of->locked = 1;
                if (!safe_strtol(tokens[i].value+1, &tmp_int)) {
                    *errstr = "CLIENT_ERROR bad token in command line format";
                    of->has_error = 1;
                } else {
                    of->recache_time = realtime(EXPTIME_TO_POSITIVE_TIME(tmp_int));
                }
                break;
            case 'l':
                of->la = 1;
                of->locked = 1; // need locked to delay LRU bump
                break;
            case 'O':
                break;
            case 'k': // known but no special handling
            case 's':
            case 't':
            case 'c':
            case 'f':
                break;
            case 'v':
                of->value = 1;
                break;
            case 'h':
                of->locked = 1; // need locked to delay LRU bump
                break;
            case 'u':
                of->no_update = 1;
                break;
            case 'q':
                of->no_reply = 1;
                break;
            // mset-related.
            case 'F':
                if (!safe_strtoul(tokens[i].value+1, &of->client_flags)) {
                    of->has_error = true;
                }
                break;
            case 'S':
                if (!safe_strtol(tokens[i].value+1, &tmp_int)) {
                    of->has_error = true;
                } else {
                    // Size is adjusted for underflow or overflow once the
                    // \r\n terminator is added.
                    if (tmp_int < 0 || tmp_int > (INT_MAX - 2)) {
                        *errstr = "CLIENT_ERROR invalid length";
                        of->has_error = true;
                    } else {
                        of->value_len = tmp_int + 2; // \r\n
                    }
                }
                break;
            case 'C': // mset, mdelete, marithmetic
                if (!safe_strtoull(tokens[i].value+1, &of->req_cas_id)) {
                    *errstr = "CLIENT_ERROR bad token in command line format";
                    of->has_error = true;
                } else {
                    of->has_cas = true;
                }
                break;
            case 'M': // mset and marithmetic mode switch
                if (tokens[i].length != 2) {
                    *errstr = "CLIENT_ERROR incorrect length for M token";
                    of->has_error = 1;
                } else {
                    of->mode = tokens[i].value[1];
                }
                break;
            case 'J': // marithmetic initial value
                if (!safe_strtoull(tokens[i].value+1, &of->initial)) {
                    *errstr = "CLIENT_ERROR invalid numeric initial value";
                    of->has_error = 1;
                }
                break;
            case 'D': // marithmetic delta value
                if (!safe_strtoull(tokens[i].value+1, &of->delta)) {
                    *errstr = "CLIENT_ERROR invalid numeric delta value";
                    of->has_error = 1;
                }
                break;
            case 'I':
                of->set_stale = 1;
                break;
            default: // unknown flag, bail.
                *errstr = "CLIENT_ERROR invalid flag";
                return -1;
        }
    }

    return of->has_error ? -1 : 0;
}

#define META_SPACE(p) { \
    *p = ' '; \
    p++; \
}

#define META_CHAR(p, c) { \
    *p = ' '; \
    *(p+1) = c; \
    p += 2; \
}

static void process_mget_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    item *it;
    unsigned int i = 0;
    struct _meta_flags of = {0}; // option bitflags.
    uint32_t hv; // cached hash value for unlocking an item.
    bool failed = false;
    bool item_created = false;
    bool won_token = false;
    bool ttl_set = false;
    char *errstr = "CLIENT_ERROR bad command line format";
    mc_resp *resp = c->resp;
    char *p = resp->wbuf;

    assert(c != NULL);
    WANT_TOKENS_MIN(ntokens, 3);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_errstring(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    // NOTE: final token has length == 0.
    // KEY_TOKEN == 1. 0 is command.

    if (ntokens == 3) {
        // TODO: any way to fix this?
        out_errstring(c, "CLIENT_ERROR bad command line format");
        return;
    } else if (ntokens > MFLAG_MAX_OPT_LENGTH) {
        // TODO: ensure the command tokenizer gives us at least this many
        out_errstring(c, "CLIENT_ERROR options flags are too long");
        return;
    }

    // scrubs duplicated options and sets flags for how to load the item.
    if (_meta_flag_preparse(tokens, ntokens, &of, &errstr) != 0) {
        out_errstring(c, errstr);
        return;
    }
    c->noreply = of.no_reply;

    // TODO: need to indicate if the item was overflowed or not?
    // I think we do, since an overflow shouldn't trigger an alloc/replace.
    bool overflow = false;
    if (!of.locked) {
        it = limited_get(key, nkey, c, 0, false, !of.no_update, &overflow);
    } else {
        // If we had to lock the item, we're doing our own bump later.
        it = limited_get_locked(key, nkey, c, DONT_UPDATE, &hv, &overflow);
    }

    // Since we're a new protocol, we can actually inform users that refcount
    // overflow is happening by straight up throwing an error.
    // We definitely don't want to re-autovivify by accident.
    if (overflow) {
        assert(it == NULL);
        out_errstring(c, "SERVER_ERROR refcount overflow during fetch");
        return;
    }

    if (it == NULL && of.vivify) {
        // Fill in the exptime during parsing later.
        it = item_alloc(key, nkey, 0, realtime(0), 2);
        // We don't actually need any of do_store_item's logic:
        // - already fetched and missed an existing item.
        // - lock is still held.
        // - not append/prepend/replace
        // - not testing CAS
        if (it != NULL) {
            // I look forward to the day I get rid of this :)
            memcpy(ITEM_data(it), "\r\n", 2);
            // NOTE: This initializes the CAS value.
            do_item_link(it, hv);
            item_created = true;
        }
    }

    // don't have to check result of add_iov() since the iov size defaults are
    // enough.
    if (it) {
        if (of.value) {
            memcpy(p, "VA ", 3);
            p = itoa_u32(it->nbytes-2, p+3);
        } else {
            memcpy(p, "OK", 2);
            p += 2;
        }

        for (i = KEY_TOKEN+1; i < ntokens-1; i++) {
            switch (tokens[i].value[0]) {
                case 'T':
                    ttl_set = true;
                    it->exptime = of.exptime;
                    break;
                case 'N':
                    if (item_created) {
                        it->exptime = of.autoviv_exptime;
                        won_token = true;
                    }
                    break;
                case 'R':
                    // If we haven't autovivified and supplied token is less
                    // than current TTL, mark a win.
                    if ((it->it_flags & ITEM_TOKEN_SENT) == 0
                            && !item_created
                            && it->exptime != 0
                            && it->exptime < of.recache_time) {
                        won_token = true;
                    }
                    break;
                case 's':
                    META_CHAR(p, 's');
                    p = itoa_u32(it->nbytes-2, p);
                    break;
                case 't':
                    // TTL remaining as of this request.
                    // needs to be relative because server clocks may not be in sync.
                    META_CHAR(p, 't');
                    if (it->exptime == 0) {
                        *p = '-';
                        *(p+1) = '1';
                        p += 2;
                    } else {
                        p = itoa_u32(it->exptime - current_time, p);
                    }
                    break;
                case 'c':
                    META_CHAR(p, 'c');
                    p = itoa_u64(ITEM_get_cas(it), p);
                    break;
                case 'f':
                    META_CHAR(p, 'f');
                    if (FLAGS_SIZE(it) == 0) {
                        *p = '0';
                        p++;
                    } else {
                        p = itoa_u32(*((uint32_t *) ITEM_suffix(it)), p);
                    }
                    break;
                case 'l':
                    META_CHAR(p, 'l');
                    p = itoa_u32(current_time - it->time, p);
                    break;
                case 'h':
                    META_CHAR(p, 'h');
                    if (it->it_flags & ITEM_FETCHED) {
                        *p = '1';
                    } else {
                        *p = '0';
                    }
                    p++;
                    break;
                case 'O':
                    if (tokens[i].length > MFLAG_MAX_OPAQUE_LENGTH) {
                        errstr = "CLIENT_ERROR opaque token too long";
                        goto error;
                    }
                    META_SPACE(p);
                    memcpy(p, tokens[i].value, tokens[i].length);
                    p += tokens[i].length;
                    break;
                case 'k':
                    META_CHAR(p, 'k');
                    memcpy(p, ITEM_key(it), it->nkey);
                    p += it->nkey;
                    break;
            }
        }

        // Has this item already sent a token?
        // Important to do this here so we don't send W with Z.
        // Isn't critical, but easier for client authors to understand.
        if (it->it_flags & ITEM_TOKEN_SENT) {
            META_CHAR(p, 'Z');
        }
        if (it->it_flags & ITEM_STALE) {
            META_CHAR(p, 'X');
            // FIXME: think hard about this. is this a default, or a flag?
            if ((it->it_flags & ITEM_TOKEN_SENT) == 0) {
                // If we're stale but no token already sent, now send one.
                won_token = true;
            }
        }

        if (won_token) {
            // Mark a win into the flag buffer.
            META_CHAR(p, 'W');
            it->it_flags |= ITEM_TOKEN_SENT;
        }

        *p = '\r';
        *(p+1) = '\n';
        *(p+2) = '\0';
        p += 2;
        // finally, chain in the buffer.
        resp_add_iov(resp, resp->wbuf, p - resp->wbuf);

        if (of.value) {
#ifdef EXTSTORE
            if (it->it_flags & ITEM_HDR) {
                if (_get_extstore(c, it, resp) != 0) {
                    pthread_mutex_lock(&c->thread->stats.mutex);
                    c->thread->stats.get_oom_extstore++;
                    pthread_mutex_unlock(&c->thread->stats.mutex);

                    failed = true;
                }
            } else if ((it->it_flags & ITEM_CHUNKED) == 0) {
                resp_add_iov(resp, ITEM_data(it), it->nbytes);
            } else {
                resp_add_chunked_iov(resp, it, it->nbytes);
            }
#else
            if ((it->it_flags & ITEM_CHUNKED) == 0) {
                resp_add_iov(resp, ITEM_data(it), it->nbytes);
            } else {
                resp_add_chunked_iov(resp, it, it->nbytes);
            }
#endif
        }

        // need to hold the ref at least because of the key above.
#ifdef EXTSTORE
        if (!failed) {
            if ((it->it_flags & ITEM_HDR) != 0 && of.value) {
                // Only have extstore clean if header and returning value.
                resp->item = NULL;
            } else {
                resp->item = it;
            }
        } else {
            // Failed to set up extstore fetch.
            if (of.locked) {
                do_item_remove(it);
            } else {
                item_remove(it);
            }
        }
#else
        resp->item = it;
#endif
    } else {
        failed = true;
    }

    if (of.locked) {
        // Delayed bump so we could get fetched/last access time pre-update.
        if (!of.no_update && it != NULL) {
            do_item_bump(c, it, hv);
        }
        item_unlock(hv);
    }

    // we count this command as a normal one if we've gotten this far.
    // TODO: for autovivify case, miss never happens. Is this okay?
    if (!failed) {
        pthread_mutex_lock(&c->thread->stats.mutex);
        if (ttl_set) {
            c->thread->stats.touch_cmds++;
            c->thread->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
        } else {
            c->thread->stats.lru_hits[it->slabs_clsid]++;
            c->thread->stats.get_cmds++;
        }
        pthread_mutex_unlock(&c->thread->stats.mutex);

        conn_set_state(c, conn_new_cmd);
    } else {
        pthread_mutex_lock(&c->thread->stats.mutex);
        if (ttl_set) {
            c->thread->stats.touch_cmds++;
            c->thread->stats.touch_misses++;
        } else {
            c->thread->stats.get_misses++;
            c->thread->stats.get_cmds++;
        }
        MEMCACHED_COMMAND_GET(c->sfd, key, nkey, -1, 0);
        pthread_mutex_unlock(&c->thread->stats.mutex);

        // This gets elided in noreply mode.
        out_string(c, "EN");
    }
    return;
error:
    if (it) {
        do_item_remove(it);
        if (of.locked) {
            item_unlock(hv);
        }
    }
    out_errstring(c, errstr);
}

static void process_mset_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    item *it;
    int i;
    short comm = NREAD_SET;
    struct _meta_flags of = {0}; // option bitflags.
    char *errstr = "CLIENT_ERROR bad command line format";
    uint32_t hv;
    mc_resp *resp = c->resp;
    char *p = resp->wbuf;

    assert(c != NULL);
    WANT_TOKENS_MIN(ntokens, 3);

    // TODO: most of this is identical to mget.
    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_errstring(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (ntokens == 3) {
        out_errstring(c, "CLIENT_ERROR bad command line format");
        return;
    }

    if (ntokens > MFLAG_MAX_OPT_LENGTH) {
        out_errstring(c, "CLIENT_ERROR options flags too long");
        return;
    }

    // leave space for the status code.
    p = resp->wbuf + 3;

    // We need to at least try to get the size to properly slurp bad bytes
    // after an error.
    if (_meta_flag_preparse(tokens, ntokens, &of, &errstr) != 0) {
        goto error;
    }

    // Set noreply after tokens are understood.
    c->noreply = of.no_reply;

    bool has_error = false;
    for (i = KEY_TOKEN+1; i < ntokens-1; i++) {
        switch (tokens[i].value[0]) {
            // TODO: macro perhaps?
            case 'O':
                if (tokens[i].length > MFLAG_MAX_OPAQUE_LENGTH) {
                    errstr = "CLIENT_ERROR opaque token too long";
                    has_error = true;
                    break;
                }
                META_SPACE(p);
                memcpy(p, tokens[i].value, tokens[i].length);
                p += tokens[i].length;
                break;
            case 'k':
                META_CHAR(p, 'k');
                memcpy(p, key, nkey);
                p += nkey;
                break;
        }
    }

    // "mode switch" to alternative commands
    switch (of.mode) {
        case 0:
            break; // no mode supplied.
        case 'E': // Add...
            comm = NREAD_ADD;
            break;
        case 'A': // Append.
            comm = NREAD_APPEND;
            break;
        case 'P': // Prepend.
            comm = NREAD_PREPEND;
            break;
        case 'R': // Replace.
            comm = NREAD_REPLACE;
            break;
        case 'S': // Set. Default.
            comm = NREAD_SET;
            break;
        default:
            errstr = "CLIENT_ERROR invalid mode for ms M token";
            goto error;
    }

    // The item storage function doesn't exactly map to mset.
    // If a CAS value is supplied, upgrade default SET mode to CAS mode.
    // Also allows REPLACE to work, as REPLACE + CAS works the same as CAS.
    // add-with-cas works the same as add; but could only LRU bump if match..
    // APPEND/PREPEND allow a simplified CAS check.
    if (of.has_cas && (comm == NREAD_SET || comm == NREAD_REPLACE)) {
        comm = NREAD_CAS;
    }

    // We attempt to process as much as we can in hopes of getting a valid and
    // adjusted vlen, or else the data swallowed after error will be for 0b.
    if (has_error)
        goto error;

    it = item_alloc(key, nkey, of.client_flags, of.exptime, of.value_len);

    if (it == 0) {
        enum store_item_type status;
        // TODO: These could be normalized codes (TL and OM). Need to
        // reorganize the output stuff a bit though.
        if (! item_size_ok(nkey, of.client_flags, of.value_len)) {
            errstr = "SERVER_ERROR object too large for cache";
            status = TOO_LARGE;
        } else {
            errstr = "SERVER_ERROR out of memory storing object";
            status = NO_MEMORY;
        }
        // FIXME: LOGGER_LOG specific to mset, include options.
        LOGGER_LOG(c->thread->l, LOG_MUTATIONS, LOGGER_ITEM_STORE,
                NULL, status, comm, key, nkey, 0, 0);

        /* Avoid stale data persisting in cache because we failed alloc. */
        // NOTE: only if SET mode?
        it = item_get_locked(key, nkey, c, DONT_UPDATE, &hv);
        if (it) {
            do_item_unlink(it, hv);
            STORAGE_delete(c->thread->storage, it);
            do_item_remove(it);
        }
        item_unlock(hv);

        goto error;
    }
    ITEM_set_cas(it, of.req_cas_id);

    c->item = it;
#ifdef NEED_ALIGN
    if (it->it_flags & ITEM_CHUNKED) {
        c->ritem = ITEM_schunk(it);
    } else {
        c->ritem = ITEM_data(it);
    }
#else
    c->ritem = ITEM_data(it);
#endif
    c->rlbytes = it->nbytes;
    c->cmd = comm;
    if (of.set_stale && comm == NREAD_CAS) {
        c->set_stale = true;
    }
    resp->wbytes = p - resp->wbuf;
    memcpy(resp->wbuf + resp->wbytes, "\r\n", 2);
    resp->wbytes += 2;
    // We've written the status line into wbuf, use wbytes to finalize later.
    resp_add_iov(resp, resp->wbuf, resp->wbytes);
    c->mset_res = true;
    conn_set_state(c, conn_nread);
    return;
error:
    /* swallow the data line */
    c->sbytes = of.value_len;

    // Note: no errors possible after the item was successfully allocated.
    // So we're just looking at dumping error codes and returning.
    out_errstring(c, errstr);
    // TODO: pass state in? else switching twice meh.
    conn_set_state(c, conn_swallow);
}

static void process_mdelete_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    uint64_t req_cas_id = 0;
    item *it = NULL;
    int i;
    uint32_t hv;
    struct _meta_flags of = {0}; // option bitflags.
    char *errstr = "CLIENT_ERROR bad command line format";
    mc_resp *resp = c->resp;
    // reserve 3 bytes for status code
    char *p = resp->wbuf + 3;

    assert(c != NULL);
    WANT_TOKENS_MIN(ntokens, 3);

    // TODO: most of this is identical to mget.
    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (ntokens > MFLAG_MAX_OPT_LENGTH) {
        out_string(c, "CLIENT_ERROR options flags too long");
        return;
    }

    // scrubs duplicated options and sets flags for how to load the item.
    if (_meta_flag_preparse(tokens, ntokens, &of, &errstr) != 0) {
        out_errstring(c, "CLIENT_ERROR invalid or duplicate flag");
        return;
    }
    c->noreply = of.no_reply;

    assert(c != NULL);
    for (i = KEY_TOKEN+1; i < ntokens-1; i++) {
        switch (tokens[i].value[0]) {
            // TODO: macro perhaps?
            case 'O':
                if (tokens[i].length > MFLAG_MAX_OPAQUE_LENGTH) {
                    errstr = "CLIENT_ERROR opaque token too long";
                    goto error;
                }
                META_SPACE(p);
                memcpy(p, tokens[i].value, tokens[i].length);
                p += tokens[i].length;
                break;
            case 'k':
                META_CHAR(p, 'k');
                memcpy(p, key, nkey);
                p += nkey;
                break;
        }
    }

    it = item_get_locked(key, nkey, c, DONT_UPDATE, &hv);
    if (it) {
        MEMCACHED_COMMAND_DELETE(c->sfd, ITEM_key(it), it->nkey);

        // allow only deleting/marking if a CAS value matches.
        if (of.has_cas && ITEM_get_cas(it) != req_cas_id) {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.delete_misses++;
            pthread_mutex_unlock(&c->thread->stats.mutex);

            memcpy(resp->wbuf, "EX ", 3);
            goto cleanup;
        }

        // If we're to set this item as stale, we don't actually want to
        // delete it. We mark the stale bit, bump CAS, and update exptime if
        // we were supplied a new TTL.
        if (of.set_stale) {
            if (of.new_ttl) {
                it->exptime = of.exptime;
            }
            it->it_flags |= ITEM_STALE;
            // Also need to remove TOKEN_SENT, so next client can win.
            it->it_flags &= ~ITEM_TOKEN_SENT;

            ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);

            // Clients can noreply nominal responses.
            if (c->noreply)
                resp->skip = true;
            memcpy(resp->wbuf, "OK ", 3);
        } else {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.slab_stats[ITEM_clsid(it)].delete_hits++;
            pthread_mutex_unlock(&c->thread->stats.mutex);

            do_item_unlink(it, hv);
            STORAGE_delete(c->thread->storage, it);
            if (c->noreply)
                resp->skip = true;
            memcpy(resp->wbuf, "OK ", 3);
        }
        goto cleanup;
    } else {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.delete_misses++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        memcpy(resp->wbuf, "NF ", 3);
        goto cleanup;
    }
cleanup:
    if (it) {
        do_item_remove(it);
    }
    // Item is always returned locked, even if missing.
    item_unlock(hv);
    resp->wbytes = p - resp->wbuf;
    memcpy(resp->wbuf + resp->wbytes, "\r\n", 2);
    resp->wbytes += 2;
    resp_add_iov(resp, resp->wbuf, resp->wbytes);
    conn_set_state(c, conn_new_cmd);
    return;
error:
    out_errstring(c, errstr);
}

static void process_marithmetic_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    int i;
    struct _meta_flags of = {0}; // option bitflags.
    char *errstr = "CLIENT_ERROR bad command line format";
    mc_resp *resp = c->resp;
    // no reservation (like del/set) since we post-process the status line.
    char *p = resp->wbuf;

    // If no argument supplied, incr or decr by one.
    of.delta = 1;
    of.initial = 0; // redundant, for clarity.
    bool incr = true; // default mode is to increment.
    bool locked = false;
    uint32_t hv = 0;
    item *it = NULL; // item returned by do_add_delta.

    assert(c != NULL);
    WANT_TOKENS_MIN(ntokens, 3);

    // TODO: most of this is identical to mget.
    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (ntokens > MFLAG_MAX_OPT_LENGTH) {
        out_string(c, "CLIENT_ERROR options flags too long");
        return;
    }

    // scrubs duplicated options and sets flags for how to load the item.
    if (_meta_flag_preparse(tokens, ntokens, &of, &errstr) != 0) {
        out_errstring(c, "CLIENT_ERROR invalid or duplicate flag");
        return;
    }
    c->noreply = of.no_reply;

    assert(c != NULL);
    // "mode switch" to alternative commands
    switch (of.mode) {
        case 0: // no switch supplied.
            break;
        case 'I': // Incr (default)
        case '+':
            incr = true;
            break;
        case 'D': // Decr.
        case '-':
            incr = false;
            break;
        default:
            errstr = "CLIENT_ERROR invalid mode for ma M token";
            goto error;
            break;
    }

    // take hash value and manually lock item... hold lock during store phase
    // on miss and avoid recalculating the hash multiple times.
    hv = hash(key, nkey);
    item_lock(hv);
    locked = true;
    char tmpbuf[INCR_MAX_STORAGE_LEN];

    // return a referenced item if it exists, so we can modify it here, rather
    // than adding even more parameters to do_add_delta.
    bool item_created = false;
    switch(do_add_delta(c, key, nkey, incr, of.delta, tmpbuf, &of.req_cas_id, hv, &it)) {
    case OK:
        if (c->noreply)
            resp->skip = true;
        memcpy(resp->wbuf, "OK ", 3);
        break;
    case NON_NUMERIC:
        errstr = "CLIENT_ERROR cannot increment or decrement non-numeric value";
        goto error;
        break;
    case EOM:
        errstr = "SERVER_ERROR out of memory";
        goto error;
        break;
    case DELTA_ITEM_NOT_FOUND:
        if (of.vivify) {
            itoa_u64(of.initial, tmpbuf);
            int vlen = strlen(tmpbuf);

            it = item_alloc(key, nkey, 0, 0, vlen+2);
            if (it != NULL) {
                memcpy(ITEM_data(it), tmpbuf, vlen);
                memcpy(ITEM_data(it) + vlen, "\r\n", 2);
                if (do_store_item(it, NREAD_ADD, c, hv)) {
                    item_created = true;
                } else {
                    // Not sure how we can get here if we're holding the lock.
                    memcpy(resp->wbuf, "NS ", 3);
                }
            } else {
                errstr = "SERVER_ERROR Out of memory allocating new item";
                goto error;
            }
        } else {
            pthread_mutex_lock(&c->thread->stats.mutex);
            if (incr) {
                c->thread->stats.incr_misses++;
            } else {
                c->thread->stats.decr_misses++;
            }
            pthread_mutex_unlock(&c->thread->stats.mutex);
            // won't have a valid it here.
            memcpy(p, "NF ", 3);
            p += 3;
        }
        break;
    case DELTA_ITEM_CAS_MISMATCH:
        // also returns without a valid it.
        memcpy(p, "EX ", 3);
        p += 3;
        break;
    }

    // final loop
    // allows building the response with information after vivifying from a
    // miss, or returning a new CAS value after add_delta().
    if (it) {
        size_t vlen = strlen(tmpbuf);
        if (of.value) {
            memcpy(p, "VA ", 3);
            p = itoa_u32(vlen, p+3);
        } else {
            memcpy(p, "OK", 2);
            p += 2;
        }

        for (i = KEY_TOKEN+1; i < ntokens-1; i++) {
            switch (tokens[i].value[0]) {
                case 'c':
                    META_CHAR(p, 'c');
                    p = itoa_u64(ITEM_get_cas(it), p);
                    break;
                case 't':
                    META_CHAR(p, 't');
                    if (it->exptime == 0) {
                        *p = '-';
                        *(p+1) = '1';
                        p += 2;
                    } else {
                        p = itoa_u32(it->exptime - current_time, p);
                    }
                    break;
                case 'T':
                    it->exptime = of.exptime;
                    break;
                case 'N':
                    if (item_created) {
                        it->exptime = of.autoviv_exptime;
                    }
                    break;
                // TODO: macro perhaps?
                case 'O':
                    if (tokens[i].length > MFLAG_MAX_OPAQUE_LENGTH) {
                        errstr = "CLIENT_ERROR opaque token too long";
                        goto error;
                    }
                    META_SPACE(p);
                    memcpy(p, tokens[i].value, tokens[i].length);
                    p += tokens[i].length;
                    break;
                case 'k':
                    META_CHAR(p, 'k');
                    memcpy(p, key, nkey);
                    p += nkey;
                    break;
            }
        }

        if (of.value) {
            *p = '\r';
            *(p+1) = '\n';
            p += 2;
            memcpy(p, tmpbuf, vlen);
            p += vlen;
        }

        do_item_remove(it);
    } else {
        // No item to handle. still need to return opaque/key tokens
        for (i = KEY_TOKEN+1; i < ntokens-1; i++) {
            switch (tokens[i].value[0]) {
                // TODO: macro perhaps?
                case 'O':
                    if (tokens[i].length > MFLAG_MAX_OPAQUE_LENGTH) {
                        errstr = "CLIENT_ERROR opaque token too long";
                        goto error;
                    }
                    META_SPACE(p);
                    memcpy(p, tokens[i].value, tokens[i].length);
                    p += tokens[i].length;
                    break;
                case 'k':
                    META_CHAR(p, 'k');
                    memcpy(p, key, nkey);
                    p += nkey;
                    break;
            }
        }
    }

    item_unlock(hv);

    resp->wbytes = p - resp->wbuf;
    memcpy(resp->wbuf + resp->wbytes, "\r\n", 2);
    resp->wbytes += 2;
    resp_add_iov(resp, resp->wbuf, resp->wbytes);
    conn_set_state(c, conn_new_cmd);
    return;
error:
    if (it != NULL)
        do_item_remove(it);
    if (locked)
        item_unlock(hv);
    out_errstring(c, errstr);
}


static void process_update_command(conn *c, token_t *tokens, const size_t ntokens, int comm, bool handle_cas) {
    char *key;
    size_t nkey;
    unsigned int flags;
    int32_t exptime_int = 0;
    rel_time_t exptime = 0;
    int vlen;
    uint64_t req_cas_id=0;
    item *it;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (! (safe_strtoul(tokens[2].value, (uint32_t *)&flags)
           && safe_strtol(tokens[3].value, &exptime_int)
           && safe_strtol(tokens[4].value, (int32_t *)&vlen))) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    exptime = realtime(EXPTIME_TO_POSITIVE_TIME(exptime_int));

    // does cas value exist?
    if (handle_cas) {
        if (!safe_strtoull(tokens[5].value, &req_cas_id)) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }
    }

    if (vlen < 0 || vlen > (INT_MAX - 2)) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }
    vlen += 2;

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    it = item_alloc(key, nkey, flags, exptime, vlen);

    if (it == 0) {
        enum store_item_type status;
        if (! item_size_ok(nkey, flags, vlen)) {
            out_string(c, "SERVER_ERROR object too large for cache");
            status = TOO_LARGE;
        } else {
            out_of_memory(c, "SERVER_ERROR out of memory storing object");
            status = NO_MEMORY;
        }
        LOGGER_LOG(c->thread->l, LOG_MUTATIONS, LOGGER_ITEM_STORE,
                NULL, status, comm, key, nkey, 0, 0, c->sfd);
        /* swallow the data line */
        conn_set_state(c, conn_swallow);
        c->sbytes = vlen;

        /* Avoid stale data persisting in cache because we failed alloc.
         * Unacceptable for SET. Anywhere else too? */
        if (comm == NREAD_SET) {
            it = item_get(key, nkey, c, DONT_UPDATE);
            if (it) {
                item_unlink(it);
                STORAGE_delete(c->thread->storage, it);
                item_remove(it);
            }
        }

        return;
    }
    ITEM_set_cas(it, req_cas_id);

    c->item = it;
#ifdef NEED_ALIGN
    if (it->it_flags & ITEM_CHUNKED) {
        c->ritem = ITEM_schunk(it);
    } else {
        c->ritem = ITEM_data(it);
    }
#else
    c->ritem = ITEM_data(it);
#endif
    c->rlbytes = it->nbytes;
    c->cmd = comm;
    conn_set_state(c, conn_nread);
}

static void process_touch_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    int32_t exptime_int = 0;
    rel_time_t exptime = 0;
    item *it;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (!safe_strtol(tokens[2].value, &exptime_int)) {
        out_string(c, "CLIENT_ERROR invalid exptime argument");
        return;
    }

    exptime = realtime(EXPTIME_TO_POSITIVE_TIME(exptime_int));
    it = item_touch(key, nkey, exptime, c);
    if (it) {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.touch_cmds++;
        c->thread->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "TOUCHED");
        item_remove(it);
    } else {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.touch_cmds++;
        c->thread->stats.touch_misses++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "NOT_FOUND");
    }
}

static void process_arithmetic_command(conn *c, token_t *tokens, const size_t ntokens, const bool incr) {
    char temp[INCR_MAX_STORAGE_LEN];
    uint64_t delta;
    char *key;
    size_t nkey;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (!safe_strtoull(tokens[2].value, &delta)) {
        out_string(c, "CLIENT_ERROR invalid numeric delta argument");
        return;
    }

    switch(add_delta(c, key, nkey, incr, delta, temp, NULL)) {
    case OK:
        out_string(c, temp);
        break;
    case NON_NUMERIC:
        out_string(c, "CLIENT_ERROR cannot increment or decrement non-numeric value");
        break;
    case EOM:
        out_of_memory(c, "SERVER_ERROR out of memory");
        break;
    case DELTA_ITEM_NOT_FOUND:
        pthread_mutex_lock(&c->thread->stats.mutex);
        if (incr) {
            c->thread->stats.incr_misses++;
        } else {
            c->thread->stats.decr_misses++;
        }
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "NOT_FOUND");
        break;
    case DELTA_ITEM_CAS_MISMATCH:
        break; /* Should never get here */
    }
}

/*
 * adds a delta value to a numeric item.
 *
 * c     connection requesting the operation
 * it    item to adjust
 * incr  true to increment value, false to decrement
 * delta amount to adjust value by
 * buf   buffer for response string
 *
 * returns a response string to send back to the client.
 */
enum delta_result_type do_add_delta(conn *c, const char *key, const size_t nkey,
                                    const bool incr, const int64_t delta,
                                    char *buf, uint64_t *cas,
                                    const uint32_t hv,
                                    item **it_ret) {
    char *ptr;
    uint64_t value;
    int res;
    item *it;

    it = do_item_get(key, nkey, hv, c, DONT_UPDATE);
    if (!it) {
        return DELTA_ITEM_NOT_FOUND;
    }

    /* Can't delta zero byte values. 2-byte are the "\r\n" */
    /* Also can't delta for chunked items. Too large to be a number */
#ifdef EXTSTORE
    if (it->nbytes <= 2 || (it->it_flags & (ITEM_CHUNKED|ITEM_HDR)) != 0) {
#else
    if (it->nbytes <= 2 || (it->it_flags & (ITEM_CHUNKED)) != 0) {
#endif
        do_item_remove(it);
        return NON_NUMERIC;
    }

    if (cas != NULL && *cas != 0 && ITEM_get_cas(it) != *cas) {
        do_item_remove(it);
        return DELTA_ITEM_CAS_MISMATCH;
    }

    ptr = ITEM_data(it);

    if (!safe_strtoull(ptr, &value)) {
        do_item_remove(it);
        return NON_NUMERIC;
    }

    if (incr) {
        value += delta;
        MEMCACHED_COMMAND_INCR(c->sfd, ITEM_key(it), it->nkey, value);
    } else {
        if(delta > value) {
            value = 0;
        } else {
            value -= delta;
        }
        MEMCACHED_COMMAND_DECR(c->sfd, ITEM_key(it), it->nkey, value);
    }

    pthread_mutex_lock(&c->thread->stats.mutex);
    if (incr) {
        c->thread->stats.slab_stats[ITEM_clsid(it)].incr_hits++;
    } else {
        c->thread->stats.slab_stats[ITEM_clsid(it)].decr_hits++;
    }
    pthread_mutex_unlock(&c->thread->stats.mutex);

    itoa_u64(value, buf);
    res = strlen(buf);
    /* refcount == 2 means we are the only ones holding the item, and it is
     * linked. We hold the item's lock in this function, so refcount cannot
     * increase. */
    if (res + 2 <= it->nbytes && it->refcount == 2) { /* replace in-place */
        /* When changing the value without replacing the item, we
           need to update the CAS on the existing item. */
        /* We also need to fiddle it in the sizes tracker in case the tracking
         * was enabled at runtime, since it relies on the CAS value to know
         * whether to remove an item or not. */
        item_stats_sizes_remove(it);
        ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
        item_stats_sizes_add(it);
        memcpy(ITEM_data(it), buf, res);
        memset(ITEM_data(it) + res, ' ', it->nbytes - res - 2);
        do_item_update(it);
    } else if (it->refcount > 1) {
        item *new_it;
        uint32_t flags;
        FLAGS_CONV(it, flags);
        new_it = do_item_alloc(ITEM_key(it), it->nkey, flags, it->exptime, res + 2);
        if (new_it == 0) {
            do_item_remove(it);
            return EOM;
        }
        memcpy(ITEM_data(new_it), buf, res);
        memcpy(ITEM_data(new_it) + res, "\r\n", 2);
        item_replace(it, new_it, hv);
        // Overwrite the older item's CAS with our new CAS since we're
        // returning the CAS of the old item below.
        ITEM_set_cas(it, (settings.use_cas) ? ITEM_get_cas(new_it) : 0);
        do_item_remove(new_it);       /* release our reference */
    } else {
        /* Should never get here. This means we somehow fetched an unlinked
         * item. TODO: Add a counter? */
        if (settings.verbose) {
            fprintf(stderr, "Tried to do incr/decr on invalid item\n");
        }
        if (it->refcount == 1)
            do_item_remove(it);
        return DELTA_ITEM_NOT_FOUND;
    }

    if (cas) {
        *cas = ITEM_get_cas(it);    /* swap the incoming CAS value */
    }
    if (it_ret != NULL) {
        *it_ret = it;
    } else {
        do_item_remove(it);         /* release our reference */
    }
    return OK;
}

static void process_delete_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    item *it;
    uint32_t hv;

    assert(c != NULL);

    if (ntokens > 3) {
        bool hold_is_zero = strcmp(tokens[KEY_TOKEN+1].value, "0") == 0;
        bool sets_noreply = set_noreply_maybe(c, tokens, ntokens);
        bool valid = (ntokens == 4 && (hold_is_zero || sets_noreply))
            || (ntokens == 5 && hold_is_zero && sets_noreply);
        if (!valid) {
            out_string(c, "CLIENT_ERROR bad command line format.  "
                       "Usage: delete <key> [noreply]");
            return;
        }
    }


    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if(nkey > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    if (settings.detail_enabled) {
        stats_prefix_record_delete(key, nkey);
    }

    it = item_get_locked(key, nkey, c, DONT_UPDATE, &hv);
    if (it) {
        MEMCACHED_COMMAND_DELETE(c->sfd, ITEM_key(it), it->nkey);

        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.slab_stats[ITEM_clsid(it)].delete_hits++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        do_item_unlink(it, hv);
        STORAGE_delete(c->thread->storage, it);
        do_item_remove(it);      /* release our reference */
        out_string(c, "DELETED");
    } else {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.delete_misses++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "NOT_FOUND");
    }
    item_unlock(hv);
}

static void process_verbosity_command(conn *c, token_t *tokens, const size_t ntokens) {
    unsigned int level;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    level = strtoul(tokens[1].value, NULL, 10);
    settings.verbose = level > MAX_VERBOSITY_LEVEL ? MAX_VERBOSITY_LEVEL : level;
    out_string(c, "OK");
    return;
}

#ifdef MEMCACHED_DEBUG
static void process_misbehave_command(conn *c) {
    int allowed = 0;

    // try opening new TCP socket
    int i = socket(AF_INET, SOCK_STREAM, 0);
    if (i != -1) {
        allowed++;
        sock_close(i);
    }

    // try executing new commands
    i = system("sleep 0");
    if (i != -1) {
        allowed++;
    }

    if (allowed) {
        out_string(c, "ERROR");
    } else {
        out_string(c, "OK");
    }
}
#endif

static void process_slabs_automove_command(conn *c, token_t *tokens, const size_t ntokens) {
    unsigned int level;
    double ratio;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (strcmp(tokens[2].value, "ratio") == 0) {
        if (ntokens < 5 || !safe_strtod(tokens[3].value, &ratio)) {
            out_string(c, "ERROR");
            return;
        }
        settings.slab_automove_ratio = ratio;
    } else {
        level = strtoul(tokens[2].value, NULL, 10);
        if (level == 0) {
            settings.slab_automove = 0;
        } else if (level == 1 || level == 2) {
            settings.slab_automove = level;
        } else {
            out_string(c, "ERROR");
            return;
        }
    }
    out_string(c, "OK");
    return;
}

/* TODO: decide on syntax for sampling? */
static void process_watch_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint16_t f = 0;
    int x;
    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);
    if (!settings.watch_enabled) {
        out_string(c, "CLIENT_ERROR watch commands not allowed");
        return;
    }

    if (resp_has_stack(c)) {
        out_string(c, "ERROR cannot pipeline other commands before watch");
        return;
    }

    if (ntokens > 2) {
        for (x = COMMAND_TOKEN + 1; x < ntokens - 1; x++) {
            if ((strcmp(tokens[x].value, "rawcmds") == 0)) {
                f |= LOG_RAWCMDS;
            } else if ((strcmp(tokens[x].value, "evictions") == 0)) {
                f |= LOG_EVICTIONS;
            } else if ((strcmp(tokens[x].value, "fetchers") == 0)) {
                f |= LOG_FETCHERS;
            } else if ((strcmp(tokens[x].value, "mutations") == 0)) {
                f |= LOG_MUTATIONS;
            } else if ((strcmp(tokens[x].value, "sysevents") == 0)) {
                f |= LOG_SYSEVENTS;
            } else {
                out_string(c, "ERROR");
                return;
            }
        }
    } else {
        f |= LOG_FETCHERS;
    }

    switch(logger_add_watcher(c, c->sfd, f)) {
        case LOGGER_ADD_WATCHER_TOO_MANY:
            out_string(c, "WATCHER_TOO_MANY log watcher limit reached");
            break;
        case LOGGER_ADD_WATCHER_FAILED:
            out_string(c, "WATCHER_FAILED failed to add log watcher");
            break;
        case LOGGER_ADD_WATCHER_OK:
            conn_set_state(c, conn_watch);
            event_del(&c->event);
            break;
    }
}

static void process_memlimit_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint32_t memlimit;
    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (!safe_strtoul(tokens[1].value, &memlimit)) {
        out_string(c, "ERROR");
    } else {
        if (memlimit < 8) {
            out_string(c, "MEMLIMIT_TOO_SMALL cannot set maxbytes to less than 8m");
        } else {
            if (memlimit > 1000000000) {
                out_string(c, "MEMLIMIT_ADJUST_FAILED input value is megabytes not bytes");
            } else if (slabs_adjust_mem_limit((size_t) memlimit * 1024 * 1024)) {
                if (settings.verbose > 0) {
                    fprintf(stderr, "maxbytes adjusted to %llum\n", (unsigned long long)memlimit);
                }

                out_string(c, "OK");
            } else {
                out_string(c, "MEMLIMIT_ADJUST_FAILED out of bounds or unable to adjust");
            }
        }
    }
}

static void process_lru_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint32_t pct_hot;
    uint32_t pct_warm;
    double hot_factor;
    int32_t ttl;
    double factor;

    set_noreply_maybe(c, tokens, ntokens);

    if (strcmp(tokens[1].value, "tune") == 0 && ntokens >= 7) {
        if (!safe_strtoul(tokens[2].value, &pct_hot) ||
            !safe_strtoul(tokens[3].value, &pct_warm) ||
            !safe_strtod(tokens[4].value, &hot_factor) ||
            !safe_strtod(tokens[5].value, &factor)) {
            out_string(c, "ERROR");
        } else {
            if (pct_hot + pct_warm > 80) {
                out_string(c, "ERROR hot and warm pcts must not exceed 80");
            } else if (factor <= 0 || hot_factor <= 0) {
                out_string(c, "ERROR hot/warm age factors must be greater than 0");
            } else {
                settings.hot_lru_pct = pct_hot;
                settings.warm_lru_pct = pct_warm;
                settings.hot_max_factor = hot_factor;
                settings.warm_max_factor = factor;
                out_string(c, "OK");
            }
        }
    } else if (strcmp(tokens[1].value, "mode") == 0 && ntokens >= 4 &&
               settings.lru_maintainer_thread) {
        if (strcmp(tokens[2].value, "flat") == 0) {
            settings.lru_segmented = false;
            out_string(c, "OK");
        } else if (strcmp(tokens[2].value, "segmented") == 0) {
            settings.lru_segmented = true;
            out_string(c, "OK");
        } else {
            out_string(c, "ERROR");
        }
    } else if (strcmp(tokens[1].value, "temp_ttl") == 0 && ntokens >= 4 &&
               settings.lru_maintainer_thread) {
        if (!safe_strtol(tokens[2].value, &ttl)) {
            out_string(c, "ERROR");
        } else {
            if (ttl < 0) {
                settings.temp_lru = false;
            } else {
                settings.temp_lru = true;
                settings.temporary_ttl = ttl;
            }
            out_string(c, "OK");
        }
    } else {
        out_string(c, "ERROR");
    }
}
#ifdef EXTSTORE
static void process_extstore_command(conn *c, token_t *tokens, const size_t ntokens) {
    set_noreply_maybe(c, tokens, ntokens);
    bool ok = true;
    if (ntokens < 4) {
        ok = false;
    } else if (strcmp(tokens[1].value, "free_memchunks") == 0 && ntokens > 4) {
        /* per-slab-class free chunk setting. */
        unsigned int clsid = 0;
        unsigned int limit = 0;
        if (!safe_strtoul(tokens[2].value, &clsid) ||
                !safe_strtoul(tokens[3].value, &limit)) {
            ok = false;
        } else {
            if (clsid < MAX_NUMBER_OF_SLAB_CLASSES) {
                settings.ext_free_memchunks[clsid] = limit;
            } else {
                ok = false;
            }
        }
    } else if (strcmp(tokens[1].value, "item_size") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_item_size))
            ok = false;
    } else if (strcmp(tokens[1].value, "item_age") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_item_age))
            ok = false;
    } else if (strcmp(tokens[1].value, "low_ttl") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_low_ttl))
            ok = false;
    } else if (strcmp(tokens[1].value, "recache_rate") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_recache_rate))
            ok = false;
    } else if (strcmp(tokens[1].value, "compact_under") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_compact_under))
            ok = false;
    } else if (strcmp(tokens[1].value, "drop_under") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_drop_under))
            ok = false;
    } else if (strcmp(tokens[1].value, "max_frag") == 0) {
        if (!safe_strtod(tokens[2].value, &settings.ext_max_frag))
            ok = false;
    } else if (strcmp(tokens[1].value, "drop_unread") == 0) {
        unsigned int v;
        if (!safe_strtoul(tokens[2].value, &v)) {
            ok = false;
        } else {
            settings.ext_drop_unread = v == 0 ? false : true;
        }
    } else {
        ok = false;
    }
    if (!ok) {
        out_string(c, "ERROR");
    } else {
        out_string(c, "OK");
    }
}
#endif
static void process_flush_all_command(conn *c, token_t *tokens, const size_t ntokens) {
    time_t exptime = 0;
    rel_time_t new_oldest = 0;

    set_noreply_maybe(c, tokens, ntokens);

    pthread_mutex_lock(&c->thread->stats.mutex);
    c->thread->stats.flush_cmds++;
    pthread_mutex_unlock(&c->thread->stats.mutex);

    if (!settings.flush_enabled) {
        // flush_all is not allowed but we log it on stats
        out_string(c, "CLIENT_ERROR flush_all not allowed");
        return;
    }

    if (ntokens != (c->noreply ? 3 : 2)) {
        exptime = strtol(tokens[1].value, NULL, 10);
        if(errno == ERANGE) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }
    }

    /*
      If exptime is zero realtime() would return zero too, and
      realtime(exptime) - 1 would overflow to the max unsigned
      value.  So we process exptime == 0 the same way we do when
      no delay is given at all.
    */
    if (exptime > 0) {
        new_oldest = realtime(exptime);
    } else { /* exptime == 0 */
        new_oldest = current_time;
    }

    if (settings.use_cas) {
        settings.oldest_live = new_oldest - 1;
        if (settings.oldest_live <= current_time)
            settings.oldest_cas = get_cas_id();
    } else {
        settings.oldest_live = new_oldest;
    }
    out_string(c, "OK");
}

static void process_version_command(conn *c) {
    out_string(c, "VERSION " VERSION);
}

static void process_quit_command(conn *c) {
    conn_set_state(c, conn_mwrite);
    c->close_after_write = true;
}

static void process_shutdown_command(conn *c) {
    if (settings.shutdown_command) {
        conn_set_state(c, conn_closing);
#ifndef _WIN32
        raise(SIGINT);
#else
        /* Graceful exit for Windows */
        raise(SIGTERM);
#endif /* #ifndef _WIN32 */

    } else {
        out_string(c, "ERROR: shutdown not enabled");
    }
}

static void process_slabs_command(conn *c, token_t *tokens, const size_t ntokens) {
    if (ntokens == 5 && strcmp(tokens[COMMAND_TOKEN + 1].value, "reassign") == 0) {
        int src, dst, rv;

        if (settings.slab_reassign == false) {
            out_string(c, "CLIENT_ERROR slab reassignment disabled");
            return;
        }

        src = strtol(tokens[2].value, NULL, 10);
        dst = strtol(tokens[3].value, NULL, 10);

        if (errno == ERANGE) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }

        rv = slabs_reassign(src, dst);
        switch (rv) {
        case REASSIGN_OK:
            out_string(c, "OK");
            break;
        case REASSIGN_RUNNING:
            out_string(c, "BUSY currently processing reassign request");
            break;
        case REASSIGN_BADCLASS:
            out_string(c, "BADCLASS invalid src or dst class id");
            break;
        case REASSIGN_NOSPARE:
            out_string(c, "NOSPARE source class has no spare pages");
            break;
        case REASSIGN_SRC_DST_SAME:
            out_string(c, "SAME src and dst class are identical");
            break;
        }
        return;
    } else if (ntokens >= 4 &&
        (strcmp(tokens[COMMAND_TOKEN + 1].value, "automove") == 0)) {
        process_slabs_automove_command(c, tokens, ntokens);
    } else {
        out_string(c, "ERROR");
    }
}

static void process_lru_crawler_command(conn *c, token_t *tokens, const size_t ntokens) {
    if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "crawl") == 0) {
        int rv;
        if (settings.lru_crawler == false) {
            out_string(c, "CLIENT_ERROR lru crawler disabled");
            return;
        }

        rv = lru_crawler_crawl(tokens[2].value, CRAWLER_EXPIRED, NULL, 0,
                settings.lru_crawler_tocrawl);
        switch(rv) {
        case CRAWLER_OK:
            out_string(c, "OK");
            break;
        case CRAWLER_RUNNING:
            out_string(c, "BUSY currently processing crawler request");
            break;
        case CRAWLER_BADCLASS:
            out_string(c, "BADCLASS invalid class id");
            break;
        case CRAWLER_NOTSTARTED:
            out_string(c, "NOTSTARTED no items to crawl");
            break;
        case CRAWLER_ERROR:
            out_string(c, "ERROR an unknown error happened");
            break;
        }
        return;
    } else if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "metadump") == 0) {
        if (settings.lru_crawler == false) {
            out_string(c, "CLIENT_ERROR lru crawler disabled");
            return;
        }
        if (!settings.dump_enabled) {
            out_string(c, "ERROR metadump not allowed");
            return;
        }
        if (resp_has_stack(c)) {
            out_string(c, "ERROR cannot pipeline other commands before metadump");
            return;
        }

        int rv = lru_crawler_crawl(tokens[2].value, CRAWLER_METADUMP,
                c, c->sfd, LRU_CRAWLER_CAP_REMAINING);
        switch(rv) {
            case CRAWLER_OK:
                // TODO: documentation says this string is returned, but
                // it never was before. We never switch to conn_write so
                // this o_s call never worked. Need to talk to users and
                // decide if removing the OK from docs is fine.
                //out_string(c, "OK");
                // TODO: Don't reuse conn_watch here.
                conn_set_state(c, conn_watch);
                event_del(&c->event);
                break;
            case CRAWLER_RUNNING:
                out_string(c, "BUSY currently processing crawler request");
                break;
            case CRAWLER_BADCLASS:
                out_string(c, "BADCLASS invalid class id");
                break;
            case CRAWLER_NOTSTARTED:
                out_string(c, "NOTSTARTED no items to crawl");
                break;
            case CRAWLER_ERROR:
                out_string(c, "ERROR an unknown error happened");
                break;
        }
        return;
    } else if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "tocrawl") == 0) {
        uint32_t tocrawl;
         if (!safe_strtoul(tokens[2].value, &tocrawl)) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }
        settings.lru_crawler_tocrawl = tocrawl;
        out_string(c, "OK");
        return;
    } else if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "sleep") == 0) {
        uint32_t tosleep;
        if (!safe_strtoul(tokens[2].value, &tosleep)) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }
        if (tosleep > 1000000) {
            out_string(c, "CLIENT_ERROR sleep must be one second or less");
            return;
        }
        settings.lru_crawler_sleep = tosleep;
        out_string(c, "OK");
        return;
    } else if (ntokens == 3) {
        if ((strcmp(tokens[COMMAND_TOKEN + 1].value, "enable") == 0)) {
            if (start_item_crawler_thread() == 0) {
                out_string(c, "OK");
            } else {
                out_string(c, "ERROR failed to start lru crawler thread");
            }
        } else if ((strcmp(tokens[COMMAND_TOKEN + 1].value, "disable") == 0)) {
            if (stop_item_crawler_thread(CRAWLER_NOWAIT) == 0) {
                out_string(c, "OK");
            } else {
                out_string(c, "ERROR failed to stop lru crawler thread");
            }
        } else {
            out_string(c, "ERROR");
        }
        return;
    } else {
        out_string(c, "ERROR");
    }
}
#ifdef TLS
static void process_refresh_certs_command(conn *c, token_t *tokens, const size_t ntokens) {
    set_noreply_maybe(c, tokens, ntokens);
    char *errmsg = NULL;
    if (refresh_certs(&errmsg)) {
        out_string(c, "OK");
    } else {
        write_and_free(c, errmsg, strlen(errmsg));
    }
    return;
}
#endif

// TODO: pipelined commands are incompatible with shifting connections to a
// side thread. Given this only happens in two instances (watch and
// lru_crawler metadump) it should be fine for things to bail. It _should_ be
// unusual for these commands.
// This is hard to fix since tokenize_command() mutilates the read buffer, so
// we can't drop out and back in again.
// Leaving this note here to spend more time on a fix when necessary, or if an
// opportunity becomes obvious.
static void process_command(conn *c, char *command) {

    token_t tokens[MAX_TOKENS];
    size_t ntokens;
    int comm;

    assert(c != NULL);

    MEMCACHED_PROCESS_COMMAND_START(c->sfd, c->rcurr, c->rbytes);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d %s\n", c->sfd, command);

    /*
     * for commands set/add/replace, we build an item and read the data
     * directly into it, then continue in nread_complete().
     */

    // Prep the response object for this query.
    if (!resp_start(c)) {
        conn_set_state(c, conn_closing);
        return;
    }

    ntokens = tokenize_command(command, tokens, MAX_TOKENS);
    // All commands need a minimum of two tokens: cmd and NULL finalizer
    // There are also no valid commands shorter than two bytes.
    if (ntokens < 2 || tokens[COMMAND_TOKEN].length < 2) {
        out_string(c, "ERROR");
        return;
    }

    // Meta commands are all 2-char in length.
    char first = tokens[COMMAND_TOKEN].value[0];
    if (first == 'm' && tokens[COMMAND_TOKEN].length == 2) {
        switch (tokens[COMMAND_TOKEN].value[1]) {
            case 'g':
                process_mget_command(c, tokens, ntokens);
                break;
            case 's':
                process_mset_command(c, tokens, ntokens);
                break;
            case 'd':
                process_mdelete_command(c, tokens, ntokens);
                break;
            case 'n':
                out_string(c, "MN");
                // mn command forces immediate writeback flush.
                conn_set_state(c, conn_mwrite);
                break;
            case 'a':
                process_marithmetic_command(c, tokens, ntokens);
                break;
            case 'e':
                process_meta_command(c, tokens, ntokens);
                break;
            default:
                out_string(c, "ERROR");
                break;
        }
    } else if (first == 'g') {
        // Various get commands are very common.
        WANT_TOKENS_MIN(ntokens, 3);
        if (strcmp(tokens[COMMAND_TOKEN].value, "get") == 0) {

            process_get_command(c, tokens, ntokens, false, false);
        } else if (strcmp(tokens[COMMAND_TOKEN].value, "gets") == 0) {

            process_get_command(c, tokens, ntokens, true, false);
        } else if (strcmp(tokens[COMMAND_TOKEN].value, "gat") == 0) {

            process_get_command(c, tokens, ntokens, false, true);
        } else if (strcmp(tokens[COMMAND_TOKEN].value, "gats") == 0) {

            process_get_command(c, tokens, ntokens, true, true);
        } else {
            out_string(c, "ERROR");
        }
    } else if (first == 's') {
        if (strcmp(tokens[COMMAND_TOKEN].value, "set") == 0 && (comm = NREAD_SET)) {

            WANT_TOKENS_OR(ntokens, 6, 7);
            process_update_command(c, tokens, ntokens, comm, false);
        } else if (strcmp(tokens[COMMAND_TOKEN].value, "stats") == 0) {

            process_stat(c, tokens, ntokens);
        } else if (strcmp(tokens[COMMAND_TOKEN].value, "shutdown") == 0) {

            process_shutdown_command(c);
        } else if (strcmp(tokens[COMMAND_TOKEN].value, "slabs") == 0) {

            process_slabs_command(c, tokens, ntokens);
        } else {
            out_string(c, "ERROR");
        }
    } else if (first == 'a') {
        if ((strcmp(tokens[COMMAND_TOKEN].value, "add") == 0 && (comm = NREAD_ADD)) ||
            (strcmp(tokens[COMMAND_TOKEN].value, "append") == 0 && (comm = NREAD_APPEND)) ) {

            WANT_TOKENS_OR(ntokens, 6, 7);
            process_update_command(c, tokens, ntokens, comm, false);
        } else {
            out_string(c, "ERROR");
        }
    } else if (first == 'c') {
        if (strcmp(tokens[COMMAND_TOKEN].value, "cas") == 0 && (comm = NREAD_CAS)) {

            WANT_TOKENS_OR(ntokens, 7, 8);
            process_update_command(c, tokens, ntokens, comm, true);
        } else if (strcmp(tokens[COMMAND_TOKEN].value, "cache_memlimit") == 0) {

            WANT_TOKENS_OR(ntokens, 3, 4);
            process_memlimit_command(c, tokens, ntokens);
        } else {
            out_string(c, "ERROR");
        }
    } else if (first == 'i') {
        if (strcmp(tokens[COMMAND_TOKEN].value, "incr") == 0) {

            WANT_TOKENS_OR(ntokens, 4, 5);
            process_arithmetic_command(c, tokens, ntokens, 1);
        } else {
            out_string(c, "ERROR");
        }
    } else if (first == 'd') {
        if (strcmp(tokens[COMMAND_TOKEN].value, "delete") == 0) {

            WANT_TOKENS(ntokens, 3, 5);
            process_delete_command(c, tokens, ntokens);
        } else if (strcmp(tokens[COMMAND_TOKEN].value, "decr") == 0) {

            WANT_TOKENS_OR(ntokens, 4, 5);
            process_arithmetic_command(c, tokens, ntokens, 0);
        } else {
            out_string(c, "ERROR");
        }
    } else if (first == 't') {
        if (strcmp(tokens[COMMAND_TOKEN].value, "touch") == 0) {

            WANT_TOKENS_OR(ntokens, 4, 5);
            process_touch_command(c, tokens, ntokens);
        } else {
            out_string(c, "ERROR");
        }
    } else if (
                (strcmp(tokens[COMMAND_TOKEN].value, "replace") == 0 && (comm = NREAD_REPLACE)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "prepend") == 0 && (comm = NREAD_PREPEND)) ) {

        WANT_TOKENS_OR(ntokens, 6, 7);
        process_update_command(c, tokens, ntokens, comm, false);

    } else if (strcmp(tokens[COMMAND_TOKEN].value, "bget") == 0) {
        // ancient "binary get" command which isn't in any documentation, was
        // removed > 10 years ago, etc. Keeping for compatibility reasons but
        // we should look deeper into client code and remove this.
        WANT_TOKENS_MIN(ntokens, 3);
        process_get_command(c, tokens, ntokens, false, false);

    } else if (strcmp(tokens[COMMAND_TOKEN].value, "flush_all") == 0) {

        WANT_TOKENS(ntokens, 2, 4);
        process_flush_all_command(c, tokens, ntokens);

    } else if (strcmp(tokens[COMMAND_TOKEN].value, "version") == 0) {

        process_version_command(c);

    } else if (strcmp(tokens[COMMAND_TOKEN].value, "quit") == 0) {

        process_quit_command(c);

    } else if (strcmp(tokens[COMMAND_TOKEN].value, "lru_crawler") == 0) {

        process_lru_crawler_command(c, tokens, ntokens);

    } else if (strcmp(tokens[COMMAND_TOKEN].value, "watch") == 0) {

        process_watch_command(c, tokens, ntokens);

    } else if (strcmp(tokens[COMMAND_TOKEN].value, "verbosity") == 0) {
        WANT_TOKENS_OR(ntokens, 3, 4);
        process_verbosity_command(c, tokens, ntokens);
    } else if (strcmp(tokens[COMMAND_TOKEN].value, "lru") == 0) {
        WANT_TOKENS_MIN(ntokens, 3);
        process_lru_command(c, tokens, ntokens);
#ifdef MEMCACHED_DEBUG
    // commands which exist only for testing the memcached's security protection
    } else if (strcmp(tokens[COMMAND_TOKEN].value, "misbehave") == 0) {
        process_misbehave_command(c);
#endif
#ifdef EXTSTORE
    } else if (strcmp(tokens[COMMAND_TOKEN].value, "extstore") == 0) {
        WANT_TOKENS_MIN(ntokens, 3);
        process_extstore_command(c, tokens, ntokens);
#endif
#ifdef TLS
    } else if (strcmp(tokens[COMMAND_TOKEN].value, "refresh_certs") == 0) {
        process_refresh_certs_command(c, tokens, ntokens);
#endif
    } else {
        if (strncmp(tokens[ntokens - 2].value, "HTTP/", 5) == 0) {
            conn_set_state(c, conn_closing);
        } else {
            out_string(c, "ERROR");
        }
    }
    return;
}

static int try_read_command_negotiate(conn *c) {
    assert(c->protocol == negotiating_prot);
    assert(c != NULL);
    assert(c->rcurr <= (c->rbuf + c->rsize));
    assert(c->rbytes > 0);

    if ((unsigned char)c->rbuf[0] == (unsigned char)PROTOCOL_BINARY_REQ) {
        c->protocol = binary_prot;
        c->try_read_command = try_read_command_binary;
    } else {
        // authentication doesn't work with negotiated protocol.
        c->protocol = ascii_prot;
        c->try_read_command = try_read_command_ascii;
    }

    if (settings.verbose > 1) {
        fprintf(stderr, "%d: Client using the %s protocol\n", c->sfd,
                prot_text(c->protocol));
    }

    return c->try_read_command(c);
}

static int try_read_command_udp(conn *c) {
    assert(c != NULL);
    assert(c->rcurr <= (c->rbuf + c->rsize));
    assert(c->rbytes > 0);

    if ((unsigned char)c->rbuf[0] == (unsigned char)PROTOCOL_BINARY_REQ) {
        c->protocol = binary_prot;
        return try_read_command_binary(c);
    } else {
        c->protocol = ascii_prot;
        return try_read_command_ascii(c);
    }
}

static int try_read_command_binary(conn *c) {
    /* Do we have the complete packet header? */
    if (c->rbytes < sizeof(c->binary_header)) {
        /* need more data! */
        return 0;
    } else {
        memcpy(&c->binary_header, c->rcurr, sizeof(c->binary_header));
        protocol_binary_request_header* req;
        req = &c->binary_header;

        if (settings.verbose > 1) {
            /* Dump the packet before we convert it to host order */
            int ii;
            fprintf(stderr, "<%d Read binary protocol data:", c->sfd);
            for (ii = 0; ii < sizeof(req->bytes); ++ii) {
                if (ii % 4 == 0) {
                    fprintf(stderr, "\n<%d   ", c->sfd);
                }
                fprintf(stderr, " 0x%02x", req->bytes[ii]);
            }
            fprintf(stderr, "\n");
        }

        c->binary_header = *req;
        c->binary_header.request.keylen = ntohs(req->request.keylen);
        c->binary_header.request.bodylen = ntohl(req->request.bodylen);
        c->binary_header.request.cas = ntohll(req->request.cas);

        if (c->binary_header.request.magic != PROTOCOL_BINARY_REQ) {
            if (settings.verbose) {
                fprintf(stderr, "Invalid magic:  %x\n",
                        c->binary_header.request.magic);
            }
            conn_set_state(c, conn_closing);
            return -1;
        }

        uint8_t extlen = c->binary_header.request.extlen;
        uint16_t keylen = c->binary_header.request.keylen;
        if (c->rbytes < keylen + extlen + sizeof(c->binary_header)) {
            // Still need more bytes. Let try_read_network() realign the
            // read-buffer and fetch more data as necessary.
            return 0;
        }

        if (!resp_start(c)) {
            conn_set_state(c, conn_closing);
            return -1;
        }

        c->cmd = c->binary_header.request.opcode;
        c->keylen = c->binary_header.request.keylen;
        c->opaque = c->binary_header.request.opaque;
        /* clear the returned cas value */
        c->cas = 0;

        c->last_cmd_time = current_time;
        // sigh. binprot has no "largest possible extlen" define, and I don't
        // want to refactor a ton of code either. Header is only ever used out
        // of c->binary_header, but the extlen stuff is used for the latter
        // bytes. Just wastes 24 bytes on the stack this way.
        char extbuf[sizeof(c->binary_header) + BIN_MAX_EXTLEN+1];
        memcpy(extbuf + sizeof(c->binary_header), c->rcurr + sizeof(c->binary_header),
                extlen > BIN_MAX_EXTLEN ? BIN_MAX_EXTLEN : extlen);
        c->rbytes -= sizeof(c->binary_header) + extlen + keylen;
        c->rcurr += sizeof(c->binary_header) + extlen + keylen;

        dispatch_bin_command(c, extbuf);
    }

    return 1;
}

static int try_read_command_asciiauth(conn *c) {
    token_t tokens[MAX_TOKENS];
    size_t ntokens;
    char *cont = NULL;

    // TODO: move to another function.
    if (!c->sasl_started) {
        char *el;
        uint32_t size = 0;

        // impossible for the auth command to be this short.
        if (c->rbytes < 2)
            return 0;

        el = memchr(c->rcurr, '\n', c->rbytes);

        // If no newline after 1k, getting junk data, close out.
        if (!el) {
            if (c->rbytes > 1024) {
                conn_set_state(c, conn_closing);
                return 1;
            }
            return 0;
        }

        // Looking for: "set foo 0 0 N\r\nuser pass\r\n"
        // key, flags, and ttl are ignored. N is used to see if we have the rest.

        // so tokenize doesn't walk past into the value.
        // it's fine to leave the \r in, as strtoul will stop at it.
        *el = '\0';

        ntokens = tokenize_command(c->rcurr, tokens, MAX_TOKENS);
        // ensure the buffer is consumed.
        c->rbytes -= (el - c->rcurr) + 1;
        c->rcurr += (el - c->rcurr) + 1;

        // final token is a NULL ender, so we have one more than expected.
        if (ntokens < 6
                || strcmp(tokens[0].value, "set") != 0
                || !safe_strtoul(tokens[4].value, &size)) {
            if (!c->resp) {
                if (!resp_start(c)) {
                    conn_set_state(c, conn_closing);
                    return 1;
                }
            }
            out_string(c, "CLIENT_ERROR unauthenticated");
            return 1;
        }

        // we don't actually care about the key at all; it can be anything.
        // we do care about the size of the remaining read.
        c->rlbytes = size + 2;

        c->sasl_started = true; // reuse from binprot sasl, but not sasl :)
    }

    if (c->rbytes < c->rlbytes) {
        // need more bytes.
        return 0;
    }

    // Going to respond at this point, so attach a response object.
    if (!c->resp) {
        if (!resp_start(c)) {
            conn_set_state(c, conn_closing);
            return 1;
        }
    }

    cont = c->rcurr;
    // advance buffer. no matter what we're stopping.
    c->rbytes -= c->rlbytes;
    c->rcurr += c->rlbytes;
    c->sasl_started = false;

    // must end with \r\n
    // NB: I thought ASCII sets also worked with just \n, but according to
    // complete_nread_ascii only \r\n is valid.
    if (strncmp(cont + c->rlbytes - 2, "\r\n", 2) != 0) {
        out_string(c, "CLIENT_ERROR bad command line termination");
        return 1;
    }

    // payload should be "user pass", so we can use the tokenizer.
    cont[c->rlbytes - 2] = '\0';
    ntokens = tokenize_command(cont, tokens, MAX_TOKENS);

    if (ntokens < 3) {
        out_string(c, "CLIENT_ERROR bad authentication token format");
        return 1;
    }

    if (authfile_check(tokens[0].value, tokens[1].value) == 1) {
        out_string(c, "STORED");
        c->authenticated = true;
        c->try_read_command = try_read_command_ascii;
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.auth_cmds++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    } else {
        out_string(c, "CLIENT_ERROR authentication failure");
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.auth_cmds++;
        c->thread->stats.auth_errors++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    }

    return 1;
}

static int try_read_command_ascii(conn *c) {
    char *el, *cont;

    if (c->rbytes == 0)
        return 0;

    el = memchr(c->rcurr, '\n', c->rbytes);
    if (!el) {
        if (c->rbytes > 1024) {
            /*
             * We didn't have a '\n' in the first k. This _has_ to be a
             * large multiget, if not we should just nuke the connection.
             */
            char *ptr = c->rcurr;
            while (*ptr == ' ') { /* ignore leading whitespaces */
                ++ptr;
            }

            if (ptr - c->rcurr > 100 ||
                (strncmp(ptr, "get ", 4) && strncmp(ptr, "gets ", 5))) {

                conn_set_state(c, conn_closing);
                return 1;
            }

            // ASCII multigets are unbound, so our fixed size rbuf may not
            // work for this particular workload... For backcompat we'll use a
            // malloc/realloc/free routine just for this.
            if (!c->rbuf_malloced) {
                if (!rbuf_switch_to_malloc(c)) {
                    conn_set_state(c, conn_closing);
                    return 1;
                }
            }
        }

        return 0;
    }
    cont = el + 1;
    if ((el - c->rcurr) > 1 && *(el - 1) == '\r') {
        el--;
    }
    *el = '\0';

    assert(cont <= (c->rcurr + c->rbytes));

    c->last_cmd_time = current_time;
    process_command(c, c->rcurr);

    c->rbytes -= (cont - c->rcurr);
    c->rcurr = cont;

    assert(c->rcurr <= (c->rbuf + c->rsize));

    return 1;
}

/*
 * read a UDP request.
 */
static enum try_read_result try_read_udp(conn *c) {
    int res;

    assert(c != NULL);

    c->request_addr_size = sizeof(c->request_addr);
    res = recvfrom(c->sfd, c->rbuf, c->rsize,
                   0, (struct sockaddr *)&c->request_addr,
                   &c->request_addr_size);
    if (res > 8) {
        unsigned char *buf = (unsigned char *)c->rbuf;
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.bytes_read += res;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        /* Beginning of UDP packet is the request ID; save it. */
        c->request_id = buf[0] * 256 + buf[1];

        /* If this is a multi-packet request, drop it. */
        if (buf[4] != 0 || buf[5] != 1) {
            out_string(c, "SERVER_ERROR multi-packet request not supported");
            return READ_NO_DATA_RECEIVED;
        }

        /* Don't care about any of the rest of the header. */
        res -= 8;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes = res;
        c->rcurr = c->rbuf;
        return READ_DATA_RECEIVED;
    }
    return READ_NO_DATA_RECEIVED;
}

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 *
 * To protect us from someone flooding a connection with bogus data causing
 * the connection to eat up all available memory, break out and start looking
 * at the data I've got after a number of reallocs...
 *
 * @return enum try_read_result
 */
static enum try_read_result try_read_network(conn *c) {
    enum try_read_result gotdata = READ_NO_DATA_RECEIVED;
    int res;
    int num_allocs = 0;
    assert(c != NULL);

    if (c->rcurr != c->rbuf) {
        if (c->rbytes != 0) /* otherwise there's nothing to copy */
            memmove(c->rbuf, c->rcurr, c->rbytes);
        c->rcurr = c->rbuf;
    }

    while (1) {
        // TODO: move to rbuf_* func?
        if (c->rbytes >= c->rsize && c->rbuf_malloced) {
            if (num_allocs == 4) {
                return gotdata;
            }
            ++num_allocs;
            char *new_rbuf = realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf) {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if (settings.verbose > 0) {
                    fprintf(stderr, "Couldn't realloc input buffer\n");
                }
                c->rbytes = 0; /* ignore what we read */
                out_of_memory(c, "SERVER_ERROR out of memory reading request");
                c->close_after_write = true;
                return READ_MEMORY_ERROR;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        int avail = c->rsize - c->rbytes;
        res = c->read(c, c->rbuf + c->rbytes, avail);
        if (res > 0) {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.bytes_read += res;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            gotdata = READ_DATA_RECEIVED;
            c->rbytes += res;
            if (res == avail && c->rbuf_malloced) {
                // Resize rbuf and try a few times if huge ascii multiget.
                continue;
            } else {
                break;
            }
        }
        if (res == 0) {
            return READ_ERROR;
        }
        if (res == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return READ_ERROR;
        }
    }
    return gotdata;
}

static bool update_event(conn *c, const int new_flags) {
    assert(c != NULL);

    struct event_base *base = c->event.ev_base;
    if (c->ev_flags == new_flags)
        return true;
    if (event_del(&c->event) == -1) return false;
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1) return false;
    return true;
}

/*
 * Sets whether we are listening for new connections or not.
 */
void do_accept_new_conns(const bool do_accept) {
    conn *next;

    for (next = listen_conn; next; next = next->next) {
        if (do_accept) {
            update_event(next, EV_READ | EV_PERSIST);
            if (listen(next->sfd, settings.backlog) != 0) {
                perror("listen");
            }
        }
        else {
            update_event(next, 0);
            if (listen(next->sfd, 0) != 0) {
                perror("listen");
            }
        }
    }

    if (do_accept) {
        struct timeval maxconns_exited;
        uint64_t elapsed_us;
        gettimeofday(&maxconns_exited,NULL);
        STATS_LOCK();
        elapsed_us =
            (maxconns_exited.tv_sec - stats.maxconns_entered.tv_sec) * 1000000
            + (maxconns_exited.tv_usec - stats.maxconns_entered.tv_usec);
        stats.time_in_listen_disabled_us += elapsed_us;
        stats_state.accepting_conns = true;
        STATS_UNLOCK();
    } else {
        STATS_LOCK();
        stats_state.accepting_conns = false;
        gettimeofday(&stats.maxconns_entered,NULL);
        stats.listen_disabled_num++;
        STATS_UNLOCK();
        allow_new_conns = false;
        maxconns_handler(-42, 0, 0);
    }
}

#define TRANSMIT_ONE_RESP true
#define TRANSMIT_ALL_RESP false
static int _transmit_pre(conn *c, struct iovec *iovs, int iovused, bool one_resp) {
    mc_resp *resp = c->resp_head;
    while (resp && iovused + resp->iovcnt < IOV_MAX-1) {
        if (resp->skip) {
            // Don't actually unchain the resp obj here since it's singly-linked.
            // Just let the post function handle it linearly.
            resp = resp->next;
            continue;
        }
        if (resp->chunked_data_iov) {
            // Handle chunked items specially.
            // They spend much more time in send so we can be a bit wasteful
            // in rebuilding iovecs for them.
            item_chunk *ch = (item_chunk *)ITEM_schunk((item *)resp->iov[resp->chunked_data_iov].iov_base);
            int x;
            for (x = 0; x < resp->iovcnt; x++) {
                // This iov is tracking how far we've copied so far.
                if (x == resp->chunked_data_iov) {
                    int done = resp->chunked_total - resp->iov[x].iov_len;
                    // Start from the len to allow binprot to cut the \r\n
                    int todo = resp->iov[x].iov_len;
                    while (ch && todo > 0 && iovused < IOV_MAX-1) {
                        int skip = 0;
                        if (!ch->used) {
                            ch = ch->next;
                            continue;
                        }
                        // Skip parts we've already sent.
                        if (done >= ch->used) {
                            done -= ch->used;
                            ch = ch->next;
                            continue;
                        } else if (done) {
                            skip = done;
                            done = 0;
                        }
                        iovs[iovused].iov_base = ch->data + skip;
                        // Stupid binary protocol makes this go negative.
                        iovs[iovused].iov_len = ch->used - skip > todo ? todo : ch->used - skip;
                        iovused++;
                        todo -= ch->used - skip;
                        ch = ch->next;
                    }
                } else {
                    iovs[iovused].iov_base = resp->iov[x].iov_base;
                    iovs[iovused].iov_len = resp->iov[x].iov_len;
                    iovused++;
                }
                if (iovused >= IOV_MAX-1)
                    break;
            }
        } else {
            memcpy(&iovs[iovused], resp->iov, sizeof(struct iovec)*resp->iovcnt);
            iovused += resp->iovcnt;
        }

        // done looking at first response, walk down the chain.
        resp = resp->next;
        // used for UDP mode: UDP cannot send multiple responses per packet.
        if (one_resp)
            break;
    }
    return iovused;
}

/*
 * Decrements and completes responses based on how much data was transmitted.
 * Takes the connection and current result bytes.
 */
static void _transmit_post(conn *c, ssize_t res) {
    // We've written some of the data. Remove the completed
    // responses from the list of pending writes.
    mc_resp *resp = c->resp_head;
    while (resp) {
        int x;
        if (resp->skip) {
            resp = resp_finish(c, resp);
            continue;
        }

        // fastpath check. all small responses should cut here.
        if (res >= resp->tosend) {
            res -= resp->tosend;
            resp = resp_finish(c, resp);
            continue;
        }

        // it's fine to re-check iov's that were zeroed out before.
        for (x = 0; x < resp->iovcnt; x++) {
            struct iovec *iov = &resp->iov[x];
            if (res >= iov->iov_len) {
                resp->tosend -= iov->iov_len;
                res -= iov->iov_len;
                iov->iov_len = 0;
            } else {
                // Dumb special case for chunked items. Currently tracking
                // where to inject the chunked item via iov_base.
                // Extra not-great since chunked items can't be the first
                // index, so we have to check for non-zero c_d_iov first.
                if (!resp->chunked_data_iov || x != resp->chunked_data_iov) {
                    iov->iov_base = (char *)iov->iov_base + res;
                }
                iov->iov_len -= res;
                resp->tosend -= res;
                res = 0;
                break;
            }
        }

        // are we done with this response object?
        if (resp->tosend == 0) {
            resp = resp_finish(c, resp);
        } else {
            // Jammed up here. This is the new head.
            break;
        }
    }
}

/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static enum transmit_result transmit(conn *c) {
    assert(c != NULL);
    struct iovec iovs[IOV_MAX];
    struct msghdr msg;
    int iovused = 0;

    // init the msg.
    memset(&msg, 0, sizeof(struct msghdr));
    msg.msg_iov = iovs;

    iovused = _transmit_pre(c, iovs, iovused, TRANSMIT_ALL_RESP);

    // Alright, send.
    ssize_t res;
    msg.msg_iovlen = iovused;
    res = c->sendmsg(c, &msg, 0);
    if (res >= 0) {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.bytes_written += res;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        // Decrement any partial IOV's and complete any finished resp's.
        _transmit_post(c, res);

        if (c->resp_head) {
            return TRANSMIT_INCOMPLETE;
        } else {
            return TRANSMIT_COMPLETE;
        }
    }

    if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!update_event(c, EV_WRITE | EV_PERSIST)) {
            if (settings.verbose > 0)
                fprintf(stderr, "Couldn't update event\n");
            conn_set_state(c, conn_closing);
            return TRANSMIT_HARD_ERROR;
        }
        return TRANSMIT_SOFT_ERROR;
    }
    /* if res == -1 and error is not EAGAIN or EWOULDBLOCK,
       we have a real error, on which we close the connection */
    if (settings.verbose > 0)
        perror("Failed to write, and not due to blocking");

    conn_set_state(c, conn_closing);
    return TRANSMIT_HARD_ERROR;
}

static void build_udp_header(unsigned char *hdr, mc_resp *resp) {
    // We need to communicate the total number of packets
    // If this isn't set, it's the first time this response is building a udp
    // header, so "tosend" must be static.
    if (!resp->udp_total) {
        uint32_t total;
        total = resp->tosend / UDP_MAX_PAYLOAD_SIZE;
        if (resp->tosend % UDP_MAX_PAYLOAD_SIZE)
            total++;
        // The spec doesn't really say what we should do here. It's _probably_
        // better to bail out?
        if (total > USHRT_MAX) {
            total = USHRT_MAX;
        }
        resp->udp_total = total;
    }

    // TODO: why wasn't this hto*'s and casts?
    // this ends up sending UDP hdr data specifically in host byte order.
    *hdr++ = resp->request_id / 256;
    *hdr++ = resp->request_id % 256;
    *hdr++ = resp->udp_sequence / 256;
    *hdr++ = resp->udp_sequence % 256;
    *hdr++ = resp->udp_total / 256;
    *hdr++ = resp->udp_total % 256;
    *hdr++ = 0;
    *hdr++ = 0;
    resp->udp_sequence++;
}

/*
 * UDP specific transmit function. Uses its own function rather than check
 * IS_UDP() five times. If we ever implement sendmmsg or similar support they
 * will diverge even more.
 * Does not use TLS.
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static enum transmit_result transmit_udp(conn *c) {
    assert(c != NULL);
    struct iovec iovs[IOV_MAX];
    struct msghdr msg;
    mc_resp *resp;
    int iovused = 0;
    unsigned char udp_hdr[UDP_HEADER_SIZE];

    // We only send one UDP packet per call (ugh), so we can only operate on a
    // single response at a time.
    resp = c->resp_head;

    if (!resp) {
        return TRANSMIT_COMPLETE;
    }

    if (resp->skip) {
        resp = resp_finish(c, resp);
        return TRANSMIT_INCOMPLETE;
    }

    // clear the message and initialize it.
    memset(&msg, 0, sizeof(struct msghdr));
    msg.msg_iov = iovs;

    // the UDP source to return to.
    msg.msg_name = &resp->request_addr;
    msg.msg_namelen = resp->request_addr_size;

    // First IOV is the custom UDP header.
    iovs[0].iov_base = udp_hdr;
    iovs[0].iov_len = UDP_HEADER_SIZE;
    build_udp_header(udp_hdr, resp);
    iovused++;

    // Fill the IOV's the standard way.
    // TODO: might get a small speedup if we let it break early with a length
    // limit.
    iovused = _transmit_pre(c, iovs, iovused, TRANSMIT_ONE_RESP);

    // Clip the IOV's to the max UDP packet size.
    // If we add support for send_mmsg, this can be where we split msg's.
    {
        int x = 0;
        int len = 0;
        for (x = 0; x < iovused; x++) {
            if (len + iovs[x].iov_len >= UDP_MAX_PAYLOAD_SIZE) {
                iovs[x].iov_len = UDP_MAX_PAYLOAD_SIZE - len;
                x++;
                break;
            } else {
                len += iovs[x].iov_len;
            }
        }
        iovused = x;
    }

    ssize_t res;
    msg.msg_iovlen = iovused;
    // NOTE: uses system sendmsg since we have no support for indirect UDP.
    res = sendmsg(c->sfd, &msg, 0);
    if (res >= 0) {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.bytes_written += res;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        // Ignore the header size from forwarding the IOV's
        res -= UDP_HEADER_SIZE;

        // Decrement any partial IOV's and complete any finished resp's.
        _transmit_post(c, res);

        if (c->resp_head) {
            return TRANSMIT_INCOMPLETE;
        } else {
            return TRANSMIT_COMPLETE;
        }
    }

    if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (!update_event(c, EV_WRITE | EV_PERSIST)) {
            if (settings.verbose > 0)
                fprintf(stderr, "Couldn't update event\n");
            conn_set_state(c, conn_closing);
            return TRANSMIT_HARD_ERROR;
        }
        return TRANSMIT_SOFT_ERROR;
    }
    /* if res == -1 and error is not EAGAIN or EWOULDBLOCK,
       we have a real error, on which we close the connection */
    if (settings.verbose > 0)
        perror("Failed to write, and not due to blocking");

    conn_set_state(c, conn_read);
    return TRANSMIT_HARD_ERROR;
}


/* Does a looped read to fill data chunks */
/* TODO: restrict number of times this can loop.
 * Also, benchmark using readv's.
 */
static int read_into_chunked_item(conn *c) {
    int total = 0;
    int res;
    assert(c->rcurr != c->ritem);

    while (c->rlbytes > 0) {
        item_chunk *ch = (item_chunk *)c->ritem;
        if (ch->size == ch->used) {
            // FIXME: ch->next is currently always 0. remove this?
            if (ch->next) {
                c->ritem = (char *) ch->next;
            } else {
                /* Allocate next chunk. Binary protocol needs 2b for \r\n */
                c->ritem = (char *) do_item_alloc_chunk(ch, c->rlbytes +
                       ((c->protocol == binary_prot) ? 2 : 0));
                if (!c->ritem) {
                    // We failed an allocation. Let caller handle cleanup.
                    total = -2;
                    break;
                }
                // ritem has new chunk, restart the loop.
                continue;
                //assert(c->rlbytes == 0);
            }
        }

        int unused = ch->size - ch->used;
        /* first check if we have leftovers in the conn_read buffer */
        if (c->rbytes > 0) {
            total = 0;
            int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
            tocopy = tocopy > unused ? unused : tocopy;
            if (c->ritem != c->rcurr) {
                memmove(ch->data + ch->used, c->rcurr, tocopy);
            }
            total += tocopy;
            c->rlbytes -= tocopy;
            c->rcurr += tocopy;
            c->rbytes -= tocopy;
            ch->used += tocopy;
            if (c->rlbytes == 0) {
                break;
            }
        } else {
            /*  now try reading from the socket */
            res = c->read(c, ch->data + ch->used,
                    (unused > c->rlbytes ? c->rlbytes : unused));
            if (res > 0) {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.bytes_read += res;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                ch->used += res;
                total += res;
                c->rlbytes -= res;
            } else {
                /* Reset total to the latest result so caller can handle it */
                total = res;
                break;
            }
        }
    }

    /* At some point I will be able to ditch the \r\n from item storage and
       remove all of these kludges.
       The above binprot check ensures inline space for \r\n, but if we do
       exactly enough allocs there will be no additional chunk for \r\n.
     */
    if (c->rlbytes == 0 && c->protocol == binary_prot && total >= 0) {
        item_chunk *ch = (item_chunk *)c->ritem;
        if (ch->size - ch->used < 2) {
            c->ritem = (char *) do_item_alloc_chunk(ch, 2);
            if (!c->ritem) {
                total = -2;
            }
        }
    }
    return total;
}

static void drive_machine(conn *c) {
    bool stop = false;
    int sfd;
    socklen_t addrlen;
    struct sockaddr_storage addr;
    int nreqs = settings.reqs_per_event;
    int res;
    const char *str;
#ifdef HAVE_ACCEPT4
    static int  use_accept4 = 1;
#else
    static int  use_accept4 = 0;
#endif

    assert(c != NULL);

    while (!stop) {

        switch(c->state) {
        case conn_listening:
            addrlen = sizeof(addr);
#ifdef HAVE_ACCEPT4
            if (use_accept4) {
                sfd = accept4(c->sfd, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK);
            } else {
                sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
            }
#else
            sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
#endif
            if (sfd == -1) {
                if (use_accept4 && errno == ENOSYS) {
                    use_accept4 = 0;
                    continue;
                }
                perror(use_accept4 ? "accept4()" : "accept()");
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* these are transient, so don't log anything */
                    stop = true;
                } else if (errno == EMFILE) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Too many open connections\n");
                    accept_new_conns(false);
                    stop = true;
                } else {
                    perror("accept()");
                    stop = true;
                }
                break;
            }
            if (!use_accept4) {
#ifndef _WIN32
                if (fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL) | O_NONBLOCK) < 0) {
#else
                u_long flags = 1;
                if (ioctlsocket(sfd, FIONBIO, &flags) < 0) {
#endif /* #ifndef _WIN32 */
                    perror("setting O_NONBLOCK");
                    sock_close(sfd);
                    break;
                }
            }

            bool reject;
            if (settings.maxconns_fast) {
                STATS_LOCK();
                reject = stats_state.curr_conns + stats_state.reserved_fds >= settings.maxconns - 1;
                if (reject) {
                    stats.rejected_conns++;
                }
                STATS_UNLOCK();
            } else {
                reject = false;
            }

            if (reject) {
                str = "ERROR Too many open connections\r\n";
                res = sock_write(sfd, str, strlen(str));
                sock_close(sfd);
            } else {
                void *ssl_v = NULL;
#ifdef TLS
                SSL *ssl = NULL;
                if (c->ssl_enabled) {
                    assert(IS_TCP(c->transport) && settings.ssl_enabled);

                    if (settings.ssl_ctx == NULL) {
                        if (settings.verbose) {
                            fprintf(stderr, "SSL context is not initialized\n");
                        }
                        sock_close(sfd);
                        break;
                    }
                    SSL_LOCK();
                    ssl = SSL_new(settings.ssl_ctx);
                    SSL_UNLOCK();
                    if (ssl == NULL) {
                        if (settings.verbose) {
                            fprintf(stderr, "Failed to created the SSL object\n");
                        }
                        sock_close(sfd);
                        break;
                    }
                    SSL_set_fd(ssl, sfd);
#ifndef _WIN32
                    int ret = SSL_accept(ssl);
#else
                    int ret = WinSSL_accept(ssl);
#endif /* #ifndef _WIN32 */
                    if (ret <= 0) {
                        int err = SSL_get_error(ssl, ret);
                        if (err == SSL_ERROR_SYSCALL || err == SSL_ERROR_SSL) {
                            if (settings.verbose) {
                                fprintf(stderr, "SSL connection failed with error code : %d : %s\n", err, strerror(errno));
                            }
                            SSL_free(ssl);
                            sock_close(sfd);
                            STATS_LOCK();
                            stats.ssl_handshake_errors++;
                            STATS_UNLOCK();
                            break;
                        }
                    }
                }
                ssl_v = (void*) ssl;
#endif

                dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST,
                                     READ_BUFFER_CACHED, c->transport, ssl_v);
            }

            stop = true;
            break;

        case conn_waiting:
            rbuf_release(c);
            if (!update_event(c, EV_READ | EV_PERSIST)) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                break;
            }

            conn_set_state(c, conn_read);
            stop = true;
            break;

        case conn_read:
            if (!IS_UDP(c->transport)) {
                // Assign a read buffer if necessary.
                if (!rbuf_alloc(c)) {
                    // TODO: Some way to allow for temporary failures.
                    conn_set_state(c, conn_closing);
                    break;
                }
                res = try_read_network(c);
            } else {
                // UDP connections always have a static buffer.
                res = try_read_udp(c);
            }

            switch (res) {
            case READ_NO_DATA_RECEIVED:
                conn_set_state(c, conn_waiting);
                break;
            case READ_DATA_RECEIVED:
                conn_set_state(c, conn_parse_cmd);
                break;
            case READ_ERROR:
                conn_set_state(c, conn_closing);
                break;
            case READ_MEMORY_ERROR: /* Failed to allocate more memory */
                /* State already set by try_read_network */
                break;
            }
            break;

        case conn_parse_cmd:
            c->noreply = false;
            if (c->try_read_command(c) == 0) {
                /* wee need more data! */
                if (c->resp_head) {
                    // Buffered responses waiting, flush in the meantime.
                    conn_set_state(c, conn_mwrite);
                } else {
                    conn_set_state(c, conn_waiting);
                }
            }

            break;

        case conn_new_cmd:
            /* Only process nreqs at a time to avoid starving other
               connections */

            --nreqs;
            if (nreqs >= 0) {
                reset_cmd_handler(c);
            } else if (c->resp_head) {
                // flush response pipe on yield.
                conn_set_state(c, conn_mwrite);
            } else {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.conn_yields++;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                if (c->rbytes > 0) {
                    /* We have already read in data into the input buffer,
                       so libevent will most likely not signal read events
                       on the socket (unless more data is available. As a
                       hack we should just put in a request to write data,
                       because that should be possible ;-)
                    */
                    if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                        if (settings.verbose > 0)
                            fprintf(stderr, "Couldn't update event\n");
                        conn_set_state(c, conn_closing);
                        break;
                    }
                }
                stop = true;
            }
            break;

        case conn_nread:
            if (c->rlbytes == 0) {
                complete_nread(c);
                break;
            }

            /* Check if rbytes < 0, to prevent crash */
            if (c->rlbytes < 0) {
                if (settings.verbose) {
                    fprintf(stderr, "Invalid rlbytes to read: len %d\n", c->rlbytes);
                }
                conn_set_state(c, conn_closing);
                break;
            }

            if ((((item *)c->item)->it_flags & ITEM_CHUNKED) == 0) {
                /* first check if we have leftovers in the conn_read buffer */
                if (c->rbytes > 0) {
                    int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
                    memmove(c->ritem, c->rcurr, tocopy);
                    c->ritem += tocopy;
                    c->rlbytes -= tocopy;
                    c->rcurr += tocopy;
                    c->rbytes -= tocopy;
                    if (c->rlbytes == 0) {
                        break;
                    }
                }

                /*  now try reading from the socket */
                res = c->read(c, c->ritem, c->rlbytes);
                if (res > 0) {
                    pthread_mutex_lock(&c->thread->stats.mutex);
                    c->thread->stats.bytes_read += res;
                    pthread_mutex_unlock(&c->thread->stats.mutex);
                    if (c->rcurr == c->ritem) {
                        c->rcurr += res;
                    }
                    c->ritem += res;
                    c->rlbytes -= res;
                    break;
                }
            } else {
                res = read_into_chunked_item(c);
                if (res > 0)
                    break;
            }

            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }

            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }

            /* Memory allocation failure */
            if (res == -2) {
                out_of_memory(c, "SERVER_ERROR Out of memory during read");
                c->sbytes = c->rlbytes;
                conn_set_state(c, conn_swallow);
                // Ensure this flag gets cleared. It gets killed on conn_new()
                // so any conn_closing is fine, calling complete_nread is
                // fine. This swallow semms to be the only other case.
                c->set_stale = false;
                c->mset_res = false;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0) {
                fprintf(stderr, "Failed to read, and not due to blocking:\n"
                        "errno: %d %s \n"
                        "rcurr=%p ritem=%p rbuf=%p rlbytes=%d rsize=%d\n",
                        errno, strerror(errno),
                        (void *)c->rcurr, (void *)c->ritem, (void *)c->rbuf,
                        (int)c->rlbytes, (int)c->rsize);
            }
            conn_set_state(c, conn_closing);
            break;

        case conn_swallow:
            /* we are reading sbytes and throwing them away */
            if (c->sbytes <= 0) {
                conn_set_state(c, conn_new_cmd);
                break;
            }

            /* first check if we have leftovers in the conn_read buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
                c->sbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                break;
            }

            /*  now try reading from the socket */
            res = c->read(c, c->rbuf, c->rsize > c->sbytes ? c->sbytes : c->rsize);
            if (res > 0) {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.bytes_read += res;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                c->sbytes -= res;
                break;
            }
            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0)
                fprintf(stderr, "Failed to read, and not due to blocking\n");
            conn_set_state(c, conn_closing);
            break;

        case conn_write:
        case conn_mwrite:
#ifdef EXTSTORE
            /* have side IO's that must process before transmit() can run.
             * remove the connection from the worker thread and dispatch the
             * IO queue
             */
            if (c->io_wrapleft) {
                assert(c->io_queued == false);
                assert(c->io_wraplist != NULL);
                // TODO: create proper state for this condition
                conn_set_state(c, conn_watch);
                event_del(&c->event);
                c->io_queued = true;
                extstore_submit(c->thread->storage, &c->io_wraplist->io);
                stop = true;
                break;
            }
#endif
            switch (!IS_UDP(c->transport) ? transmit(c) : transmit_udp(c)) {
            case TRANSMIT_COMPLETE:
                if (c->state == conn_mwrite) {
                    // Free up IO wraps and any half-uploaded items.
                    conn_release_items(c);
                    conn_set_state(c, conn_new_cmd);
                    if (c->close_after_write) {
                        conn_set_state(c, conn_closing);
                    }
                } else {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Unexpected state %d\n", c->state);
                    conn_set_state(c, conn_closing);
                }
                break;

            case TRANSMIT_INCOMPLETE:
            case TRANSMIT_HARD_ERROR:
                break;                   /* Continue in state machine. */

            case TRANSMIT_SOFT_ERROR:
                stop = true;
                break;
            }
            break;

        case conn_closing:
            if (IS_UDP(c->transport))
                conn_cleanup(c);
            else
                conn_close(c);
            stop = true;
            break;

        case conn_closed:
            /* This only happens if dormando is an idiot. */
            abort();
            break;

        case conn_watch:
            /* We handed off our connection to the logger thread. */
            stop = true;
            break;
        case conn_max_state:
            assert(false);
            break;
        }
    }

    return;
}

void event_handler(const evutil_socket_t fd, const short which, void *arg) {
    conn *c;

    c = (conn *)arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd) {
        if (settings.verbose > 0)
            fprintf(stderr, "Catastrophic: event fd doesn't match conn fd!\n");
        conn_close(c);
        return;
    }

    drive_machine(c);

    /* wait for next event */
    return;
}

static int new_socket(struct addrinfo *ai) {
    int sfd;
    int flags = 1;

    if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
        return -1;
    }

#ifndef _WIN32
    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
#else
    if (ioctlsocket(sfd, FIONBIO, (u_long *)&flags) < 0) {
#endif /* #ifndef _WIN32 */
        perror("setting O_NONBLOCK");
        sock_close(sfd);
        return -1;
    }
    return sfd;
}


/*
 * Sets a socket's send buffer size to the maximum allowed by the system.
 */
static void maximize_sndbuf(const int sfd) {
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;

    /* Start with the default size. */
#ifdef _WIN32
    if (getsockopt((SOCKET)sfd, SOL_SOCKET, SO_SNDBUF, (char *)&old_size, &intsize) != 0) {
#else
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &old_size, &intsize) != 0) {
#endif /* #ifdef _WIN32 */
        if (settings.verbose > 0)
            perror("getsockopt(SO_SNDBUF)");
        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size;
    max = MAX_SENDBUF_SIZE;

    while (min <= max) {
        avg = ((unsigned int)(min + max)) / 2;
        if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0) {
            last_good = avg;
            min = avg + 1;
        } else {
            max = avg - 1;
        }
    }

    if (settings.verbose > 1)
        fprintf(stderr, "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
}

/**
 * Create a socket and bind it to a specific port number
 * @param iface the interface to bind to
 * @param port the port number to bind to
 * @param transport the transport protocol (TCP / UDP)
 * @param portnumber_file A filepointer to write the port numbers to
 *        when they are successfully added to the list of ports we
 *        listen on.
 */
static int server_socket(const char *iface,
                         int port,
                         enum network_transport transport,
                         FILE *portnumber_file, bool ssl_enabled) {
    int sfd;
    struct linger ling = {0, 0};
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints = { .ai_flags = AI_PASSIVE,
                              .ai_family = AF_UNSPEC };
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;
    int flags =1;

    hints.ai_socktype = IS_UDP(transport) ? SOCK_DGRAM : SOCK_STREAM;

    if (port == -1) {
        port = 0;
    }
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    error= getaddrinfo(iface, port_buf, &hints, &ai);
    if (error != 0) {
        if (error != EAI_SYSTEM)
          fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
        else
          perror("getaddrinfo()");
        return 1;
    }

    for (next= ai; next; next= next->ai_next) {
        conn *listen_conn_add;
        if ((sfd = new_socket(next)) == -1) {
            /* getaddrinfo can return "junk" addresses,
             * we make sure at least one works before erroring.
             */
            if (errno == EMFILE) {
                /* ...unless we're out of fds */
                perror("server_socket");
                exit(EX_OSERR);
            }
            continue;
        }

#ifdef IPV6_V6ONLY
        if (next->ai_family == AF_INET6) {
            error = setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &flags, sizeof(flags));
            if (error != 0) {
                perror("setsockopt");
                sock_close(sfd);
                continue;
            }
        }
#endif

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
        if (IS_UDP(transport)) {
            maximize_sndbuf(sfd);
        } else {
            error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
            if (error != 0)
                perror("setsockopt");

            error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
            if (error != 0)
                perror("setsockopt");

            error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
            if (error != 0)
                perror("setsockopt");
        }

        if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1) {
            if (errno != EADDRINUSE) {
                perror("bind()");
                sock_close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            sock_close(sfd);
            continue;
        } else {
            success++;
            if (!IS_UDP(transport) && listen(sfd, settings.backlog) == -1) {
                perror("listen()");
                sock_close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            if (portnumber_file != NULL &&
                (next->ai_addr->sa_family == AF_INET ||
                 next->ai_addr->sa_family == AF_INET6)) {
                union {
                    struct sockaddr_in in;
                    struct sockaddr_in6 in6;
                } my_sockaddr;
                socklen_t len = sizeof(my_sockaddr);
                if (getsockname(sfd, (struct sockaddr*)&my_sockaddr, &len)==0) {
                    if (next->ai_addr->sa_family == AF_INET) {
                        fprintf(portnumber_file, "%s INET: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in.sin_port));
                    } else {
                        fprintf(portnumber_file, "%s INET6: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in6.sin6_port));
                    }
                }
            }
        }

        if (IS_UDP(transport)) {
            int c;

            for (c = 0; c < settings.num_threads_per_udp; c++) {
                /* Allocate one UDP file descriptor per worker thread;
                 * this allows "stats conns" to separately list multiple
                 * parallel UDP requests in progress.
                 *
                 * The dispatch code round-robins new connection requests
                 * among threads, so this is guaranteed to assign one
                 * FD to each thread.
                 */
                int per_thread_fd;
                if (c == 0) {
                    per_thread_fd = sfd;
                } else {
#ifndef _WIN32
                    per_thread_fd = dup(sfd);
#else
                    per_thread_fd = sock_dup(sfd);  /* dup(socket) returns EBADF */
#endif /* #ifndef _WIN32 */
                    if (per_thread_fd < 0) {
                        perror("Failed to duplicate file descriptor");
                        exit(EXIT_FAILURE);
                    }
                }
                dispatch_conn_new(per_thread_fd, conn_read,
                                  EV_READ | EV_PERSIST,
                                  UDP_READ_BUFFER_SIZE, transport, NULL);
            }
        } else {
            if (!(listen_conn_add = conn_new(sfd, conn_listening,
                                             EV_READ | EV_PERSIST, 1,
                                             transport, main_base, NULL))) {
                fprintf(stderr, "failed to create listening connection\n");
                exit(EXIT_FAILURE);
            }
#ifdef TLS
            listen_conn_add->ssl_enabled = ssl_enabled;
#else
            assert(ssl_enabled == false);
#endif
            listen_conn_add->next = listen_conn;
            listen_conn = listen_conn_add;
        }
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

static int server_sockets(int port, enum network_transport transport,
                          FILE *portnumber_file) {
    bool ssl_enabled = false;

#ifdef TLS
    const char *notls = "notls";
    ssl_enabled = settings.ssl_enabled;
#endif

    if (settings.inter == NULL) {
        return server_socket(settings.inter, port, transport, portnumber_file, ssl_enabled);
    } else {
        // tokenize them and bind to each one of them..
        char *b;
        int ret = 0;
        char *list = strdup(settings.inter);

        if (list == NULL) {
            fprintf(stderr, "Failed to allocate memory for parsing server interface string\n");
            return 1;
        }
        for (char *p = strtok_r(list, ";,", &b);
            p != NULL;
            p = strtok_r(NULL, ";,", &b)) {
            int the_port = port;
#ifdef TLS
            ssl_enabled = settings.ssl_enabled;
            // "notls" option is valid only when memcached is run with SSL enabled.
            if (strncmp(p, notls, strlen(notls)) == 0) {
                if (!settings.ssl_enabled) {
                    fprintf(stderr, "'notls' option is valid only when SSL is enabled\n");
                    free(list);
                    return 1;
                }
                ssl_enabled = false;
                p += strlen(notls) + 1;
            }
#endif

            char *h = NULL;
            if (*p == '[') {
                // expecting it to be an IPv6 address enclosed in []
                // i.e. RFC3986 style recommended by RFC5952
                char *e = strchr(p, ']');
                if (e == NULL) {
                    fprintf(stderr, "Invalid IPV6 address: \"%s\"", p);
                    free(list);
                    return 1;
                }
                h = ++p; // skip the opening '['
                *e = '\0';
                p = ++e; // skip the closing ']'
            }

            char *s = strchr(p, ':');
            if (s != NULL) {
                // If no more semicolons - attempt to treat as port number.
                // Otherwise the only valid option is an unenclosed IPv6 without port, until
                // of course there was an RFC3986 IPv6 address previously specified -
                // in such a case there is no good option, will just send it to fail as port number.
                if (strchr(s + 1, ':') == NULL || h != NULL) {
                    *s = '\0';
                    ++s;
                    if (!safe_strtol(s, &the_port)) {
                        fprintf(stderr, "Invalid port number: \"%s\"", s);
                        free(list);
                        return 1;
                    }
                }
            }

            if (h != NULL)
                p = h;

            if (strcmp(p, "*") == 0) {
                p = NULL;
            }
            ret |= server_socket(p, the_port, transport, portnumber_file, ssl_enabled);
        }
        free(list);
        return ret;
    }
}

#ifndef DISABLE_UNIX_SOCKET
static int new_socket_unix(void) {
    int sfd;
    int flags = 1;

    if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket()");
        return -1;
    }

#ifndef _WIN32
    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
#else
    if (ioctlsocket(sfd, FIONBIO, (u_long *)&flags) < 0) {
#endif /* #ifndef _WIN32 */
        perror("setting O_NONBLOCK");
        sock_close(sfd);
        return -1;
    }
    return sfd;
}

static int server_socket_unix(const char *path, int access_mask) {
    int sfd;
    struct linger ling = {0, 0};
    struct sockaddr_un addr;
    int flags =1;
    int old_umask;

    if (!path) {
        return 1;
    }

    if ((sfd = new_socket_unix()) == -1) {
        return 1;
    }

    /*
     * Clean up a previous socket file if we left it around
     */
#ifndef _WIN32
    struct stat tstat;
    if (lstat(path, &tstat) == 0) {
        if (S_ISSOCK(tstat.st_mode))
            unlink(path);
    }
#else
    /* Windows implements unix socket file as NTFS reparse point. It looks
     * like a regular file for users. Delete file regardless to avoid
     * WSAEADDRINUSE error. */
    DeleteFile(path);
#endif /* #ifndef _WIN32 */

    /* Setting SO_REUSEADDR in Windows' unix socket causes the bind to fail with WSAEOPNOTSUPP */
#ifndef _WIN32
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
#endif /* #ifndef _WIN32 */
    setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

    /*
     * the memset call clears nonstandard fields in some implementations
     * that otherwise mess things up.
     */
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    assert(strcmp(addr.sun_path, path) == 0);
    old_umask = umask( ~(access_mask&0777));
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind()");
        sock_close(sfd);
        umask(old_umask);
        return 1;
    }
    umask(old_umask);
    if (listen(sfd, settings.backlog) == -1) {
        perror("listen()");
        sock_close(sfd);
        return 1;
    }
    if (!(listen_conn = conn_new(sfd, conn_listening,
                                 EV_READ | EV_PERSIST, 1,
                                 local_transport, main_base, NULL))) {
        fprintf(stderr, "failed to create listening connection\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}
#else
#define server_socket_unix(path, access_mask)   -1
#endif /* #ifndef DISABLE_UNIX_SOCKET */

/*
 * We keep the current time of day in a global variable that's updated by a
 * timer event. This saves us a bunch of time() system calls (we really only
 * need to get the time once a second, whereas there can be tens of thousands
 * of requests a second) and allows us to use server-start-relative timestamps
 * rather than absolute UNIX timestamps, a space savings on systems where
 * sizeof(time_t) > sizeof(unsigned int).
 */
volatile rel_time_t current_time;
static struct event clockevent;
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
static bool monotonic = false;
static int64_t monotonic_start;
#endif

/* libevent uses a monotonic clock when available for event scheduling. Aside
 * from jitter, simply ticking our internal timer here is accurate enough.
 * Note that users who are setting explicit dates for expiration times *must*
 * ensure their clocks are correct before starting memcached. */
static void clock_handler(const evutil_socket_t fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 1, .tv_usec = 0};
    static bool initialized = false;

    if (initialized) {
        /* only delete the event if it's actually there. */
        evtimer_del(&clockevent);
    } else {
        initialized = true;
    }

    // While we're here, check for hash table expansion.
    // This function should be quick to avoid delaying the timer.
    assoc_start_expand(stats_state.curr_items);
    // also, if HUP'ed we need to do some maintenance.
    // for now that's just the authfile reload.
    if (settings.sig_hup) {
        settings.sig_hup = false;

        authfile_load(settings.auth_file);
    }

    evtimer_set(&clockevent, clock_handler, 0);
    event_base_set(main_base, &clockevent);
    evtimer_add(&clockevent, &t);

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
    if (monotonic) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
            return;
        current_time = (rel_time_t) (ts.tv_sec - monotonic_start);
        return;
    }
#endif
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        current_time = (rel_time_t) (tv.tv_sec - process_started);
    }
}

static const char* flag_enabled_disabled(bool flag) {
    return (flag ? "enabled" : "disabled");
}

static void verify_default(const char* param, bool condition) {
    if (!condition) {
        printf("Default value of [%s] has changed."
            " Modify the help text and default value check.\n", param);
        exit(EXIT_FAILURE);
    }
}

static void usage(void) {
    printf(PACKAGE " " VERSION "\n");
    printf("-p, --port=<num>          TCP port to listen on (default: %d)\n"
           "-U, --udp-port=<num>      UDP port to listen on (default: %d, off)\n",
           settings.port, settings.udpport);
#ifndef DISABLE_UNIX_SOCKET
    printf("-s, --unix-socket=<file>  UNIX socket to listen on (disables network support)\n");
    printf("-a, --unix-mask=<mask>    access mask for UNIX socket, in octal (default: %o)\n",
            settings.access);
#endif /* #ifndef DISABLE_UNIX_SOCKET */
    printf("-A, --enable-shutdown     enable ascii \"shutdown\" command\n");
    printf("-l, --listen=<addr>       interface to listen on (default: INADDR_ANY)\n");
#ifdef TLS
    printf("                          if TLS/SSL is enabled, 'notls' prefix can be used to\n"
           "                          disable for specific listeners (-l notls:<ip>:<port>) \n");
#endif
    printf("-d, --daemon              run as a daemon\n"
#ifndef DISABLE_COREDUMP
           "-r, --enable-coredumps    maximize core file limit\n"
#endif /* #ifndef DISABLE_COREDUMP */
#ifndef DISABLE_RUNAS_USER
           "-u, --user=<user>         assume identity of <username> (only when run as root)\n"
#endif /* #ifndef DISABLE_RUNAS_USER */
           "-m, --memory-limit=<num>  item memory in megabytes (default: %lu)\n"
           "-M, --disable-evictions   return error on memory exhausted instead of evicting\n"
           "-c, --conn-limit=<num>    max simultaneous connections (default: %d)\n"
#ifndef DISABLE_MLOCKALL
           "-k, --lock-memory         lock down all paged memory\n"
#endif /* #ifndef DISABLE_MLOCKALL */
           "-v, --verbose             verbose (print errors/warnings while in event loop)\n"
           "-vv                       very verbose (also print client commands/responses)\n"
           "-vvv                      extremely verbose (internal state transitions)\n"
           "-h, --help                print this help and exit\n"
           "-i, --license             print memcached and libevent license\n"
           "-V, --version             print version and exit\n"
           "-P, --pidfile=<file>      save PID in <file>, only used with -d option\n"
           "-f, --slab-growth-factor=<num> chunk size growth factor (default: %2.2f)\n"
           "-n, --slab-min-size=<bytes> min space used for key+value+flags (default: %d)\n",
           (unsigned long) settings.maxbytes / (1 << 20),
           settings.maxconns, settings.factor, settings.chunk_size);
    verify_default("udp-port",settings.udpport == 0);
    printf("-L, --enable-largepages  try to use large memory pages (if available)\n");
    printf("-D <char>     Use <char> as the delimiter between key prefixes and IDs.\n"
           "              This is used for per-prefix stats reporting. The default is\n"
           "              \"%c\" (colon). If this option is specified, stats collection\n"
           "              is turned on automatically; if not, then it may be turned on\n"
           "              by sending the \"stats detail on\" command to the server.\n",
           settings.prefix_delimiter);
    printf("-t, --threads=<num>       number of threads to use (default: %d)\n", settings.num_threads);
    printf("-R, --max-reqs-per-event  maximum number of requests per event, limits the\n"
           "                          requests processed per connection to prevent \n"
           "                          starvation (default: %d)\n", settings.reqs_per_event);
    printf("-C, --disable-cas         disable use of CAS\n");
    printf("-b, --listen-backlog=<num> set the backlog queue limit (default: %d)\n", settings.backlog);
    printf("-B, --protocol=<name>     protocol - one of ascii, binary, or auto (default: %s)\n",
           prot_text(settings.binding_protocol));
    printf("-I, --max-item-size=<num> adjusts max item size\n"
           "                          (default: %dm, min: %dk, max: %dm)\n",
           settings.item_size_max/ (1 << 20), ITEM_SIZE_MAX_LOWER_LIMIT / (1 << 10),  ITEM_SIZE_MAX_UPPER_LIMIT / (1 << 20));
#ifdef ENABLE_SASL
    printf("-S, --enable-sasl         turn on Sasl authentication\n");
#endif
    printf("-F, --disable-flush-all   disable flush_all command\n");
    printf("-X, --disable-dumping     disable stats cachedump and lru_crawler metadump\n");
    printf("-W  --disable-watch       disable watch commands (live logging)\n");
    printf("-Y, --auth-file=<file>    (EXPERIMENTAL) enable ASCII protocol authentication. format:\n"
           "                          user:pass\\nuser2:pass2\\n\n");
    printf("-e, --memory-file=<file>  (EXPERIMENTAL) mmap a file for item memory.\n"
           "                          use only in ram disks or persistent memory mounts!\n"
           "                          enables restartable cache (stop with SIGUSR1)\n");
#ifdef TLS
    printf("-Z, --enable-ssl          enable TLS/SSL\n");
#endif
    printf("-o, --extended            comma separated list of extended options\n"
           "                          most options have a 'no_' prefix to disable\n"
           "   - maxconns_fast:       immediately close new connections after limit (default: %s)\n"
           "   - hashpower:           an integer multiplier for how large the hash\n"
           "                          table should be. normally grows at runtime. (default starts at: %d)\n"
           "                          set based on \"STAT hash_power_level\"\n"
           "   - tail_repair_time:    time in seconds for how long to wait before\n"
           "                          forcefully killing LRU tail item.\n"
           "                          disabled by default; very dangerous option.\n"
           "   - hash_algorithm:      the hash table algorithm\n"
           "                          default is murmur3 hash. options: jenkins, murmur3\n"
           "   - no_lru_crawler:      disable LRU Crawler background thread.\n"
           "   - lru_crawler_sleep:   microseconds to sleep between items\n"
           "                          default is %d.\n"
           "   - lru_crawler_tocrawl: max items to crawl per slab per run\n"
           "                          default is %u (unlimited)\n",
           flag_enabled_disabled(settings.maxconns_fast), settings.hashpower_init,
           settings.lru_crawler_sleep, settings.lru_crawler_tocrawl);
    printf("   - resp_obj_mem_limit:  limit in megabytes for connection response objects.\n"
           "                          do not adjust unless you have high (100k+) conn. limits.\n"
           "                          0 means unlimited (default: %u)\n"
           "   - read_buf_mem_limit:  limit in megabytes for connection read buffers.\n"
           "                          do not adjust unless you have high (100k+) conn. limits.\n"
           "                          0 means unlimited (default: %u)\n",
           settings.resp_obj_mem_limit,
           settings.read_buf_mem_limit);
    verify_default("resp_obj_mem_limit", settings.resp_obj_mem_limit == 0);
    verify_default("read_buf_mem_limit", settings.read_buf_mem_limit == 0);
    printf("   - no_lru_maintainer:   disable new LRU system + background thread.\n"
           "   - hot_lru_pct:         pct of slab memory to reserve for hot lru.\n"
           "                          (requires lru_maintainer, default pct: %d)\n"
           "   - warm_lru_pct:        pct of slab memory to reserve for warm lru.\n"
           "                          (requires lru_maintainer, default pct: %d)\n"
           "   - hot_max_factor:      items idle > cold lru age * drop from hot lru. (default: %.2f)\n"
           "   - warm_max_factor:     items idle > cold lru age * this drop from warm. (default: %.2f)\n"
           "   - temporary_ttl:       TTL's below get separate LRU, can't be evicted.\n"
           "                          (requires lru_maintainer, default: %d)\n"
           "   - idle_timeout:        timeout for idle connections. (default: %d, no timeout)\n",
           settings.hot_lru_pct, settings.warm_lru_pct, settings.hot_max_factor, settings.warm_max_factor,
           settings.temporary_ttl, settings.idle_timeout);
    printf("   - slab_chunk_max:      (EXPERIMENTAL) maximum slab size in kilobytes. use extreme care. (default: %d)\n"
           "   - watcher_logbuf_size: size in kilobytes of per-watcher write buffer. (default: %u)\n"
           "   - worker_logbuf_size:  size in kilobytes of per-worker-thread buffer\n"
           "                          read by background thread, then written to watchers. (default: %u)\n"
           "   - track_sizes:         enable dynamic reports for 'stats sizes' command.\n"
           "   - no_hashexpand:       disables hash table expansion (dangerous)\n"
           "   - modern:              enables options which will be default in future.\n"
           "                          currently: nothing\n"
           "   - no_modern:           uses defaults of previous major version (1.4.x)\n",
           settings.slab_chunk_size_max / (1 << 10), settings.logger_watcher_buf_size / (1 << 10),
           settings.logger_buf_size / (1 << 10));
    verify_default("tail_repair_time", settings.tail_repair_time == TAIL_REPAIR_TIME_DEFAULT);
    verify_default("lru_crawler_tocrawl", settings.lru_crawler_tocrawl == 0);
    verify_default("idle_timeout", settings.idle_timeout == 0);
#ifdef HAVE_DROP_PRIVILEGES
    printf("   - drop_privileges:     enable dropping extra syscall privileges\n"
           "   - no_drop_privileges:  disable drop_privileges in case it causes issues with\n"
           "                          some customisation.\n"
           "                          (default is no_drop_privileges)\n");
    verify_default("drop_privileges", !settings.drop_privileges);
#ifdef MEMCACHED_DEBUG
    printf("   - relaxed_privileges:  running tests requires extra privileges. (default: %s)\n",
           flag_enabled_disabled(settings.relaxed_privileges));
#endif
#endif
#ifdef EXTSTORE
    printf("\n   - External storage (ext_*) related options (see: https://memcached.org/extstore)\n");
    printf("   - ext_path:            file to write to for external storage.\n"
           "                          ie: ext_path=/mnt/d1/extstore:1G\n"
           "   - ext_page_size:       size in megabytes of storage pages. (default: %u)\n"
           "   - ext_wbuf_size:       size in megabytes of page write buffers. (default: %u)\n"
           "   - ext_threads:         number of IO threads to run. (default: %u)\n"
           "   - ext_item_size:       store items larger than this (bytes, default %u)\n"
           "   - ext_item_age:        store items idle at least this long (seconds, default: no age limit)\n"
           "   - ext_low_ttl:         consider TTLs lower than this specially (default: %u)\n"
           "   - ext_drop_unread:     don't re-write unread values during compaction (default: %s)\n"
           "   - ext_recache_rate:    recache an item every N accesses (default: %u)\n"
           "   - ext_compact_under:   compact when fewer than this many free pages\n"
           "                          (default: 1/4th of the assigned storage)\n"
           "   - ext_drop_under:      drop COLD items when fewer than this many free pages\n"
           "                          (default: 1/4th of the assigned storage)\n"
           "   - ext_max_frag:        max page fragmentation to tolerate (default: %.2f)\n"
           "   - slab_automove_freeratio: ratio of memory to hold free as buffer.\n"
           "                          (see doc/storage.txt for more info, default: %.3f)\n",
           settings.ext_page_size / (1 << 20), settings.ext_wbuf_size / (1 << 20), settings.ext_io_threadcount,
           settings.ext_item_size, settings.ext_low_ttl,
           flag_enabled_disabled(settings.ext_drop_unread), settings.ext_recache_rate,
           settings.ext_max_frag, settings.slab_automove_freeratio);
    verify_default("ext_item_age", settings.ext_item_age == UINT_MAX);
#endif
#ifdef TLS
    printf("   - ssl_chain_cert:      certificate chain file in PEM format\n"
           "   - ssl_key:             private key, if not part of the -ssl_chain_cert\n"
           "   - ssl_keyformat:       private key format (PEM, DER or ENGINE) (default: PEM)\n");
    printf("   - ssl_verify_mode:     peer certificate verification mode, default is 0(None).\n"
           "                          valid values are 0(None), 1(Request), 2(Require)\n"
           "                          or 3(Once)\n");
    printf("   - ssl_ciphers:         specify cipher list to be used\n"
           "   - ssl_ca_cert:         PEM format file of acceptable client CA's\n"
           "   - ssl_wbuf_size:       size in kilobytes of per-connection SSL output buffer\n"
           "                          (default: %u)\n", settings.ssl_wbuf_size / (1 << 10));
    printf("   - ssl_session_cache:   enable server-side SSL session cache, to support session\n"
           "                          resumption\n");
    verify_default("ssl_keyformat", settings.ssl_keyformat == SSL_FILETYPE_PEM);
    verify_default("ssl_verify_mode", settings.ssl_verify_mode == SSL_VERIFY_NONE);
#endif
    return;
}

static void usage_license(void) {
    printf(PACKAGE " " VERSION "\n\n");
    printf(
    "Copyright (c) 2003, Danga Interactive, Inc. <http://www.danga.com/>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are\n"
    "met:\n"
    "\n"
    "    * Redistributions of source code must retain the above copyright\n"
    "notice, this list of conditions and the following disclaimer.\n"
    "\n"
    "    * Redistributions in binary form must reproduce the above\n"
    "copyright notice, this list of conditions and the following disclaimer\n"
    "in the documentation and/or other materials provided with the\n"
    "distribution.\n"
    "\n"
    "    * Neither the name of the Danga Interactive nor the names of its\n"
    "contributors may be used to endorse or promote products derived from\n"
    "this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
    "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
    "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\n"
    "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
    "OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n"
    "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
    "LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
    "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    "\n"
    "\n"
    "This product includes software developed by Niels Provos.\n"
    "\n"
    "[ libevent ]\n"
    "\n"
    "Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions\n"
    "are met:\n"
    "1. Redistributions of source code must retain the above copyright\n"
    "   notice, this list of conditions and the following disclaimer.\n"
    "2. Redistributions in binary form must reproduce the above copyright\n"
    "   notice, this list of conditions and the following disclaimer in the\n"
    "   documentation and/or other materials provided with the distribution.\n"
    "3. All advertising materials mentioning features or use of this software\n"
    "   must display the following acknowledgement:\n"
    "      This product includes software developed by Niels Provos.\n"
    "4. The name of the author may not be used to endorse or promote products\n"
    "   derived from this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR\n"
    "IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES\n"
    "OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.\n"
    "IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,\n"
    "INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT\n"
    "NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF\n"
    "THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    );

    return;
}

static void save_pid(const char *pid_file) {
    FILE *fp;
    if (access(pid_file, F_OK) == 0) {
        if ((fp = fopen(pid_file, "r")) != NULL) {
            char buffer[1024];
            if (fgets(buffer, sizeof(buffer), fp) != NULL) {
                unsigned int pid;
                if (safe_strtoul(buffer, &pid) && kill((pid_t)pid, 0) == 0) {
                    fprintf(stderr, "WARNING: The pid file contained the following (running) pid: %u\n", pid);
                }
            }
            fclose(fp);
        }
    }

    /* Create the pid file first with a temporary name, then
     * atomically move the file to the real name to avoid a race with
     * another process opening the file to read the pid, but finding
     * it empty.
     */
    char tmp_pid_file[1024];
    snprintf(tmp_pid_file, sizeof(tmp_pid_file), "%s.tmp", pid_file);

    if ((fp = fopen(tmp_pid_file, "w")) == NULL) {
        vperror("Could not open the pid file %s for writing", tmp_pid_file);
        return;
    }

    fprintf(fp,"%ld\n", (long)getpid());
    if (fclose(fp) == -1) {
        vperror("Could not close the pid file %s", tmp_pid_file);
    }

#ifdef _WIN32
    /* In Windows, rename will have errors if the target exists.
     * As a workaround, delete first the file
     */
    unlink(pid_file);
#endif /* #ifdef _WIN32 */

    if (rename(tmp_pid_file, pid_file) != 0) {
        vperror("Could not rename the pid file from %s to %s",
                tmp_pid_file, pid_file);
    }
}

static void remove_pidfile(const char *pid_file) {
  if (pid_file == NULL)
      return;

  if (unlink(pid_file) != 0) {
      vperror("Could not remove the pid file %s", pid_file);
  }

}

static void sig_handler(const int sig) {
    printf("Signal handled: %s(%d).\n", strsignal(sig), sig);
    exit(EXIT_SUCCESS);
}

static void sig_usrhandler(const int sig) {
    printf("Graceful shutdown signal handled: %s(%d).\n", strsignal(sig), sig);
    stop_main_loop = true;
}

#ifndef WINDOWS_ONLY_SIGNALS
static void sighup_handler(const int sig) {
    settings.sig_hup = true;
}

#ifndef HAVE_SIGIGNORE
static int sigignore(int sig) {
    struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = 0 };

    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(sig, &sa, 0) == -1) {
        return -1;
    }
    return 0;
}
#endif
#endif /* #ifndef WINDOWS_ONLY_SIGNALS */

/*
 * On systems that supports multiple page sizes we may reduce the
 * number of TLB-misses by using the biggest available page size
 */
static int enable_large_pages(void) {
#if defined(HAVE_GETPAGESIZES) && defined(HAVE_MEMCNTL)
    int ret = -1;
    size_t sizes[32];
    int avail = getpagesizes(sizes, 32);
    if (avail != -1) {
        size_t max = sizes[0];
        struct memcntl_mha arg = {0};
        int ii;

        for (ii = 1; ii < avail; ++ii) {
            if (max < sizes[ii]) {
                max = sizes[ii];
            }
        }

        arg.mha_flags   = 0;
        arg.mha_pagesize = max;
        arg.mha_cmd = MHA_MAPSIZE_BSSBRK;

        if (memcntl(0, 0, MC_HAT_ADVISE, (caddr_t)&arg, 0, 0) == -1) {
            fprintf(stderr, "Failed to set large pages: %s\n",
                    strerror(errno));
            fprintf(stderr, "Will use default page size\n");
        } else {
            ret = 0;
        }
    } else {
        fprintf(stderr, "Failed to get supported pagesizes: %s\n",
                strerror(errno));
        fprintf(stderr, "Will use default page size\n");
    }

    return ret;
#elif defined(__linux__) && defined(MADV_HUGEPAGE)
    /* check if transparent hugepages is compiled into the kernel */
    struct stat st;
    int ret = stat("/sys/kernel/mm/transparent_hugepage/enabled", &st);
    if (ret || !(st.st_mode & S_IFREG)) {
        fprintf(stderr, "Transparent huge pages support not detected.\n");
        fprintf(stderr, "Will use default page size.\n");
        return -1;
    }
    return 0;
#elif defined(__FreeBSD__)
    int spages;
    size_t spagesl = sizeof(spages);

    if (sysctlbyname("vm.pmap.pg_ps_enabled", &spages,
    &spagesl, NULL, 0) != 0) {
        fprintf(stderr, "Could not evaluate the presence of superpages features.");
        return -1;
    }
    if (spages != 1) {
        fprintf(stderr, "Superpages support not detected.\n");
        fprintf(stderr, "Will use default page size.\n");
        return -1;
    }
    return 0;
#elif defined(_WIN32)
    return enable_large_pages_win();
#else
    return -1;
#endif
}

/**
 * Do basic sanity check of the runtime environment
 * @return true if no errors found, false if we can't use this env
 */
static bool sanitycheck(void) {
    /* One of our biggest problems is old and bogus libevents */
    const char *ever = event_get_version();
    if (ever != NULL) {
        if (strncmp(ever, "1.", 2) == 0) {
            /* Require at least 1.3 (that's still a couple of years old) */
            if (('0' <= ever[2] && ever[2] < '3') && !isdigit(ever[3])) {
                fprintf(stderr, "You are using libevent %s.\nPlease upgrade to"
                        " a more recent version (1.3 or newer)\n",
                        event_get_version());
                return false;
            }
        }
    }

    return true;
}

static bool _parse_slab_sizes(char *s, uint32_t *slab_sizes) {
    char *b = NULL;
    uint32_t size = 0;
    int i = 0;
    uint32_t last_size = 0;

    if (strlen(s) < 1)
        return false;

    for (char *p = strtok_r(s, "-", &b);
         p != NULL;
         p = strtok_r(NULL, "-", &b)) {
        if (!safe_strtoul(p, &size) || size < settings.chunk_size
             || size > settings.slab_chunk_size_max) {
            fprintf(stderr, "slab size %u is out of valid range\n", size);
            return false;
        }
        if (last_size >= size) {
            fprintf(stderr, "slab size %u cannot be lower than or equal to a previous class size\n", size);
            return false;
        }
        if (size <= last_size + CHUNK_ALIGN_BYTES) {
            fprintf(stderr, "slab size %u must be at least %d bytes larger than previous class\n",
                    size, CHUNK_ALIGN_BYTES);
            return false;
        }
        slab_sizes[i++] = size;
        last_size = size;
        if (i >= MAX_NUMBER_OF_SLAB_CLASSES-1) {
            fprintf(stderr, "too many slab classes specified\n");
            return false;
        }
    }

    slab_sizes[i] = 0;
    return true;
}

struct _mc_meta_data {
    void *mmap_base;
    uint64_t old_base;
    char *slab_config; // string containing either factor or custom slab list.
    int64_t time_delta;
    uint64_t process_started;
    uint32_t current_time;
};

// We need to remember a combination of configuration settings and global
// state for restart viability and resumption of internal services.
// Compared to the number of tunables and state values, relatively little
// does need to be remembered.
// Time is the hardest; we have to assume the sys clock is correct and re-sync for
// the lost time after restart.
static int _mc_meta_save_cb(const char *tag, void *ctx, void *data) {
    struct _mc_meta_data *meta = (struct _mc_meta_data *)data;

    // Settings to remember.
    // TODO: should get a version of version which is numeric, else
    // comparisons for compat reasons are difficult.
    // it may be possible to punt on this for now; since we can test for the
    // absense of another key... such as the new numeric version.
    //restart_set_kv(ctx, "version", "%s", VERSION);
    // We hold the original factor or subopts _string_
    // it can be directly compared without roundtripping through floats or
    // serializing/deserializing the long options list.
    restart_set_kv(ctx, "slab_config", "%s", meta->slab_config);
    restart_set_kv(ctx, "maxbytes", "%llu", (unsigned long long) settings.maxbytes);
    restart_set_kv(ctx, "chunk_size", "%d", settings.chunk_size);
    restart_set_kv(ctx, "item_size_max", "%d", settings.item_size_max);
    restart_set_kv(ctx, "slab_chunk_size_max", "%d", settings.slab_chunk_size_max);
    restart_set_kv(ctx, "slab_page_size", "%d", settings.slab_page_size);
    restart_set_kv(ctx, "use_cas", "%s", settings.use_cas ? "true" : "false");
    restart_set_kv(ctx, "slab_reassign", "%s", settings.slab_reassign ? "true" : "false");

    // Online state to remember.

    // current time is tough. we need to rely on the clock being correct to
    // pull the delta between stop and start times. we also need to know the
    // delta between start time and now to restore monotonic clocks.
    // for non-monotonic clocks (some OS?), process_started is the only
    // important one.
    restart_set_kv(ctx, "current_time", "%u", current_time);
    // types are great until... this. some systems time_t could be big, but
    // I'm assuming never negative.
    restart_set_kv(ctx, "process_started", "%llu", (unsigned long long) process_started);
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        restart_set_kv(ctx, "stop_time", "%lu", tv.tv_sec);
    }

    // Might as well just fetch the next CAS value to use than tightly
    // coupling the internal variable into the restart system.
    restart_set_kv(ctx, "current_cas", "%llu", (unsigned long long) get_cas_id());
    restart_set_kv(ctx, "oldest_cas", "%llu", (unsigned long long) settings.oldest_cas);
    restart_set_kv(ctx, "logger_gid", "%llu", logger_get_gid());
    restart_set_kv(ctx, "hashpower", "%u", stats_state.hash_power_level);
    // NOTE: oldest_live is a rel_time_t, which aliases for unsigned int.
    // should future proof this with a 64bit upcast, or fetch value from a
    // converter function/macro?
    restart_set_kv(ctx, "oldest_live", "%u", settings.oldest_live);
    // TODO: use uintptr_t etc? is it portable enough?
    restart_set_kv(ctx, "mmap_oldbase", "%p", meta->mmap_base);

    return 0;
}

// We must see at least this number of checked lines. Else empty/missing lines
// could cause a false-positive.
// TODO: Once crc32'ing of the metadata file is done this could be ensured better by
// the restart module itself (crc32 + count of lines must match on the
// backend)
#define RESTART_REQUIRED_META 17

// With this callback we make a decision on if the current configuration
// matches up enough to allow reusing the cache.
// We also re-load important runtime information.
static int _mc_meta_load_cb(const char *tag, void *ctx, void *data) {
    struct _mc_meta_data *meta = (struct _mc_meta_data *)data;
    char *key;
    char *val;
    int reuse_mmap = 0;
    meta->process_started = 0;
    meta->time_delta = 0;
    meta->current_time = 0;
    int lines_seen = 0;

    // TODO: not sure this is any better than just doing an if/else tree with
    // strcmp's...
    enum {
        R_MMAP_OLDBASE = 0,
        R_MAXBYTES,
        R_CHUNK_SIZE,
        R_ITEM_SIZE_MAX,
        R_SLAB_CHUNK_SIZE_MAX,
        R_SLAB_PAGE_SIZE,
        R_SLAB_CONFIG,
        R_USE_CAS,
        R_SLAB_REASSIGN,
        R_CURRENT_CAS,
        R_OLDEST_CAS,
        R_OLDEST_LIVE,
        R_LOGGER_GID,
        R_CURRENT_TIME,
        R_STOP_TIME,
        R_PROCESS_STARTED,
        R_HASHPOWER,
    };

    const char *opts[] = {
        [R_MMAP_OLDBASE] = "mmap_oldbase",
        [R_MAXBYTES] = "maxbytes",
        [R_CHUNK_SIZE] = "chunk_size",
        [R_ITEM_SIZE_MAX] = "item_size_max",
        [R_SLAB_CHUNK_SIZE_MAX] = "slab_chunk_size_max",
        [R_SLAB_PAGE_SIZE] = "slab_page_size",
        [R_SLAB_CONFIG] = "slab_config",
        [R_USE_CAS] = "use_cas",
        [R_SLAB_REASSIGN] = "slab_reassign",
        [R_CURRENT_CAS] = "current_cas",
        [R_OLDEST_CAS] = "oldest_cas",
        [R_OLDEST_LIVE] = "oldest_live",
        [R_LOGGER_GID] = "logger_gid",
        [R_CURRENT_TIME] = "current_time",
        [R_STOP_TIME] = "stop_time",
        [R_PROCESS_STARTED] = "process_started",
        [R_HASHPOWER] = "hashpower",
        NULL
    };

    while (restart_get_kv(ctx, &key, &val) == RESTART_OK) {
        int type = 0;
        int32_t val_int = 0;
        uint32_t val_uint = 0;
        int64_t bigval_int = 0;
        uint64_t bigval_uint = 0;

        while (opts[type] != NULL && strcmp(key, opts[type]) != 0) {
            type++;
        }
        if (opts[type] == NULL) {
            fprintf(stderr, "[restart] unknown/unhandled key: %s\n", key);
            continue;
        }
        lines_seen++;

        // helper for any boolean checkers.
        bool val_bool = false;
        bool is_bool = true;
        if (strcmp(val, "false") == 0) {
            val_bool = false;
        } else if (strcmp(val, "true") == 0) {
            val_bool = true;
        } else {
            is_bool = false;
        }

        switch (type) {
        case R_MMAP_OLDBASE:
            if (!safe_strtoull_hex(val, &meta->old_base)) {
                fprintf(stderr, "[restart] failed to parse %s: %s\n", key, val);
                reuse_mmap = -1;
            }
            break;
        case R_MAXBYTES:
            if (!safe_strtoll(val, &bigval_int) || settings.maxbytes != bigval_int) {
                reuse_mmap = -1;
            }
            break;
        case R_CHUNK_SIZE:
            if (!safe_strtol(val, &val_int) || settings.chunk_size != val_int) {
                reuse_mmap = -1;
            }
            break;
        case R_ITEM_SIZE_MAX:
            if (!safe_strtol(val, &val_int) || settings.item_size_max != val_int) {
                reuse_mmap = -1;
            }
            break;
        case R_SLAB_CHUNK_SIZE_MAX:
            if (!safe_strtol(val, &val_int) || settings.slab_chunk_size_max != val_int) {
                reuse_mmap = -1;
            }
            break;
        case R_SLAB_PAGE_SIZE:
            if (!safe_strtol(val, &val_int) || settings.slab_page_size != val_int) {
                reuse_mmap = -1;
            }
            break;
        case R_SLAB_CONFIG:
            if (strcmp(val, meta->slab_config) != 0) {
                reuse_mmap = -1;
            }
            break;
        case R_USE_CAS:
            if (!is_bool || settings.use_cas != val_bool) {
                reuse_mmap = -1;
            }
            break;
        case R_SLAB_REASSIGN:
            if (!is_bool || settings.slab_reassign != val_bool) {
                reuse_mmap = -1;
            }
            break;
        case R_CURRENT_CAS:
            // FIXME: do we need to fail if these values _aren't_ found?
            if (!safe_strtoull(val, &bigval_uint)) {
                reuse_mmap = -1;
            } else {
                set_cas_id(bigval_uint);
            }
            break;
        case R_OLDEST_CAS:
            if (!safe_strtoull(val, &bigval_uint)) {
                reuse_mmap = -1;
            } else {
                settings.oldest_cas = bigval_uint;
            }
            break;
        case R_OLDEST_LIVE:
            if (!safe_strtoul(val, &val_uint)) {
                reuse_mmap = -1;
            } else {
                settings.oldest_live = val_uint;
            }
            break;
        case R_LOGGER_GID:
            if (!safe_strtoull(val, &bigval_uint)) {
                reuse_mmap = -1;
            } else {
                logger_set_gid(bigval_uint);
            }
            break;
        case R_PROCESS_STARTED:
            if (!safe_strtoull(val, &bigval_uint)) {
                reuse_mmap = -1;
            } else {
                meta->process_started = bigval_uint;
            }
            break;
        case R_CURRENT_TIME:
            if (!safe_strtoul(val, &val_uint)) {
                reuse_mmap = -1;
            } else {
                meta->current_time = val_uint;
            }
            break;
        case R_STOP_TIME:
            if (!safe_strtoll(val, &bigval_int)) {
                reuse_mmap = -1;
            } else {
                struct timeval t;
                gettimeofday(&t, NULL);
                meta->time_delta = t.tv_sec - bigval_int;
                // clock has done something crazy.
                // there are _lots_ of ways the clock can go wrong here, but
                // this is a safe sanity check since there's nothing else we
                // can realistically do.
                if (meta->time_delta <= 0) {
                    reuse_mmap = -1;
                }
            }
            break;
        case R_HASHPOWER:
            if (!safe_strtoul(val, &val_uint)) {
                reuse_mmap = -1;
            } else {
                settings.hashpower_init = val_uint;
            }
            break;
        default:
            fprintf(stderr, "[restart] unhandled key: %s\n", key);
        }

        if (reuse_mmap != 0) {
            fprintf(stderr, "[restart] restart incompatible due to setting for [%s] [old value: %s]\n", key, val);
            break;
        }
    }

    if (lines_seen < RESTART_REQUIRED_META) {
        fprintf(stderr, "[restart] missing some metadata lines\n");
        reuse_mmap = -1;
    }

    return reuse_mmap;
}

int main (int argc, char **argv) {
    int c;
#ifndef DISABLE_MLOCKALL
    bool lock_memory = false;
#endif /* #ifndef DISABLE_MLOCKALL */
    bool do_daemonize = false;
    bool preallocate = false;
#ifndef DISABLE_COREDUMP
    int maxcore = 0;
    struct rlimit rlim;
#endif /* #ifndef DISABLE_COREDUMP */
#ifndef DISABLE_RUNAS_USER
    char *username = NULL;
#endif /* #ifndef DISABLE_RUNAS_USER */
    char *pid_file = NULL;
    char *memory_file = NULL;
#ifndef DISABLE_RUNAS_USER
    struct passwd *pw;
#endif /* #ifndef DISABLE_RUNAS_USER */
    char *buf;
    char unit = '\0';
    int size_max = 0;
    int retval = EXIT_SUCCESS;
    bool protocol_specified = false;
    bool tcp_specified = false;
    bool udp_specified = false;
    bool start_lru_maintainer = true;
    bool start_lru_crawler = true;
    bool start_assoc_maint = true;
    enum hashfunc_type hash_type = MURMUR3_HASH;
    uint32_t tocrawl;
    uint32_t slab_sizes[MAX_NUMBER_OF_SLAB_CLASSES];
    bool use_slab_sizes = false;
    char *slab_sizes_unparsed = NULL;
    bool slab_chunk_size_changed = false;
    // struct for restart code. Initialized up here so we can curry
    // important settings to save or validate.
    struct _mc_meta_data *meta = malloc(sizeof(struct _mc_meta_data));
    meta->slab_config = NULL;
#ifdef EXTSTORE
    void *storage = NULL;
    struct extstore_conf_file *storage_file = NULL;
    struct extstore_conf ext_cf;
#endif
    char *subopts, *subopts_orig;
    char *subopts_value;
    enum {
        MAXCONNS_FAST = 0,
        HASHPOWER_INIT,
        NO_HASHEXPAND,
        SLAB_REASSIGN,
        SLAB_AUTOMOVE,
        SLAB_AUTOMOVE_RATIO,
        SLAB_AUTOMOVE_WINDOW,
        TAIL_REPAIR_TIME,
        HASH_ALGORITHM,
        LRU_CRAWLER,
        LRU_CRAWLER_SLEEP,
        LRU_CRAWLER_TOCRAWL,
        LRU_MAINTAINER,
        HOT_LRU_PCT,
        WARM_LRU_PCT,
        HOT_MAX_FACTOR,
        WARM_MAX_FACTOR,
        TEMPORARY_TTL,
        IDLE_TIMEOUT,
        WATCHER_LOGBUF_SIZE,
        WORKER_LOGBUF_SIZE,
        SLAB_SIZES,
        SLAB_CHUNK_MAX,
        TRACK_SIZES,
        NO_INLINE_ASCII_RESP,
        MODERN,
        NO_MODERN,
        NO_CHUNKED_ITEMS,
        NO_SLAB_REASSIGN,
        NO_SLAB_AUTOMOVE,
        NO_MAXCONNS_FAST,
        INLINE_ASCII_RESP,
        NO_LRU_CRAWLER,
        NO_LRU_MAINTAINER,
        NO_DROP_PRIVILEGES,
        DROP_PRIVILEGES,
        RESP_OBJ_MEM_LIMIT,
        READ_BUF_MEM_LIMIT,
#ifdef TLS
        SSL_CERT,
        SSL_KEY,
        SSL_VERIFY_MODE,
        SSL_KEYFORM,
        SSL_CIPHERS,
        SSL_CA_CERT,
        SSL_WBUF_SIZE,
        SSL_SESSION_CACHE,
#endif
#ifdef MEMCACHED_DEBUG
        RELAXED_PRIVILEGES,
#endif
#ifdef EXTSTORE
        EXT_PAGE_SIZE,
        EXT_WBUF_SIZE,
        EXT_THREADS,
        EXT_IO_DEPTH,
        EXT_PATH,
        EXT_ITEM_SIZE,
        EXT_ITEM_AGE,
        EXT_LOW_TTL,
        EXT_RECACHE_RATE,
        EXT_COMPACT_UNDER,
        EXT_DROP_UNDER,
        EXT_MAX_FRAG,
        EXT_DROP_UNREAD,
        SLAB_AUTOMOVE_FREERATIO,
#endif
    };
    char *const subopts_tokens[] = {
        [MAXCONNS_FAST] = "maxconns_fast",
        [HASHPOWER_INIT] = "hashpower",
        [NO_HASHEXPAND] = "no_hashexpand",
        [SLAB_REASSIGN] = "slab_reassign",
        [SLAB_AUTOMOVE] = "slab_automove",
        [SLAB_AUTOMOVE_RATIO] = "slab_automove_ratio",
        [SLAB_AUTOMOVE_WINDOW] = "slab_automove_window",
        [TAIL_REPAIR_TIME] = "tail_repair_time",
        [HASH_ALGORITHM] = "hash_algorithm",
        [LRU_CRAWLER] = "lru_crawler",
        [LRU_CRAWLER_SLEEP] = "lru_crawler_sleep",
        [LRU_CRAWLER_TOCRAWL] = "lru_crawler_tocrawl",
        [LRU_MAINTAINER] = "lru_maintainer",
        [HOT_LRU_PCT] = "hot_lru_pct",
        [WARM_LRU_PCT] = "warm_lru_pct",
        [HOT_MAX_FACTOR] = "hot_max_factor",
        [WARM_MAX_FACTOR] = "warm_max_factor",
        [TEMPORARY_TTL] = "temporary_ttl",
        [IDLE_TIMEOUT] = "idle_timeout",
        [WATCHER_LOGBUF_SIZE] = "watcher_logbuf_size",
        [WORKER_LOGBUF_SIZE] = "worker_logbuf_size",
        [SLAB_SIZES] = "slab_sizes",
        [SLAB_CHUNK_MAX] = "slab_chunk_max",
        [TRACK_SIZES] = "track_sizes",
        [NO_INLINE_ASCII_RESP] = "no_inline_ascii_resp",
        [MODERN] = "modern",
        [NO_MODERN] = "no_modern",
        [NO_CHUNKED_ITEMS] = "no_chunked_items",
        [NO_SLAB_REASSIGN] = "no_slab_reassign",
        [NO_SLAB_AUTOMOVE] = "no_slab_automove",
        [NO_MAXCONNS_FAST] = "no_maxconns_fast",
        [INLINE_ASCII_RESP] = "inline_ascii_resp",
        [NO_LRU_CRAWLER] = "no_lru_crawler",
        [NO_LRU_MAINTAINER] = "no_lru_maintainer",
        [NO_DROP_PRIVILEGES] = "no_drop_privileges",
        [DROP_PRIVILEGES] = "drop_privileges",
        [RESP_OBJ_MEM_LIMIT] = "resp_obj_mem_limit",
        [READ_BUF_MEM_LIMIT] = "read_buf_mem_limit",
#ifdef TLS
        [SSL_CERT] = "ssl_chain_cert",
        [SSL_KEY] = "ssl_key",
        [SSL_VERIFY_MODE] = "ssl_verify_mode",
        [SSL_KEYFORM] = "ssl_keyformat",
        [SSL_CIPHERS] = "ssl_ciphers",
        [SSL_CA_CERT] = "ssl_ca_cert",
        [SSL_WBUF_SIZE] = "ssl_wbuf_size",
        [SSL_SESSION_CACHE] = "ssl_session_cache",
#endif
#ifdef MEMCACHED_DEBUG
        [RELAXED_PRIVILEGES] = "relaxed_privileges",
#endif
#ifdef EXTSTORE
        [EXT_PAGE_SIZE] = "ext_page_size",
        [EXT_WBUF_SIZE] = "ext_wbuf_size",
        [EXT_THREADS] = "ext_threads",
        [EXT_IO_DEPTH] = "ext_io_depth",
        [EXT_PATH] = "ext_path",
        [EXT_ITEM_SIZE] = "ext_item_size",
        [EXT_ITEM_AGE] = "ext_item_age",
        [EXT_LOW_TTL] = "ext_low_ttl",
        [EXT_RECACHE_RATE] = "ext_recache_rate",
        [EXT_COMPACT_UNDER] = "ext_compact_under",
        [EXT_DROP_UNDER] = "ext_drop_under",
        [EXT_MAX_FRAG] = "ext_max_frag",
        [EXT_DROP_UNREAD] = "ext_drop_unread",
        [SLAB_AUTOMOVE_FREERATIO] = "slab_automove_freeratio",
#endif
        NULL
    };

#ifdef _WIN32
    // Initialize Winsock
    if (sock_startup()) {
        fprintf(stderr, "Winsock initialization failed!\n");
        free(meta);
        return EX_OSERR;
    }
#endif /* #ifdef _WIN32 */

    if (!sanitycheck()) {
        free(meta);
        return EX_OSERR;
    }

    /* handle SIGINT, SIGTERM */
    signal(SIGINT, sig_handler);
#ifndef WINDOWS_ONLY_SIGNALS
    signal(SIGTERM, sig_handler);
    signal(SIGHUP, sighup_handler);
    signal(SIGUSR1, sig_usrhandler);
#else
    /* Windows does not support/generate SIGUSR1 or some standard signals.
     * Just use '-A' option and shutdown command to just internally
     * raise SIGTERM via @ref process_shutdown_command and handle it as
     * graceful exit. This also means that behavior is different with upstream
     * for shutdown. This is better than adding Windows message handling IPC (e.g. WM_QUIT)
     * just to handle graceful exit. The user can just use any Windows
     * tools/commands to kill the process immediately if desired.
     * Summary:
     * SIGINT (e.g. Ctrl+C)   : Signalled but exit immediately (same with upstream)
     * SIGTERM (via shutdown) : Signalled and exit gracefully (not same with  upstream)
     * Others (e.g. taskkill) : Not signalled and exit immediately (same with upstream)
     */
    signal(SIGTERM, sig_usrhandler);
#endif /* #ifndef WINDOWS_ONLY_SIGNALS */

    /* init settings */
    settings_init();
    verify_default("hash_algorithm", hash_type == MURMUR3_HASH);
#ifdef EXTSTORE
    settings.ext_item_size = 512;
    settings.ext_item_age = UINT_MAX;
    settings.ext_low_ttl = 0;
    settings.ext_recache_rate = 2000;
    settings.ext_max_frag = 0.8;
    settings.ext_drop_unread = false;
    settings.ext_wbuf_size = 1024 * 1024 * 4;
    settings.ext_compact_under = 0;
    settings.ext_drop_under = 0;
    settings.slab_automove_freeratio = 0.01;
    settings.ext_page_size = 1024 * 1024 * 64;
    settings.ext_io_threadcount = 1;
    ext_cf.page_size = settings.ext_page_size;
    ext_cf.wbuf_size = settings.ext_wbuf_size;
    ext_cf.io_threadcount = settings.ext_io_threadcount;
    ext_cf.io_depth = 1;
    ext_cf.page_buckets = 4;
    ext_cf.wbuf_count = ext_cf.page_buckets;
#endif

    /* Run regardless of initializing it later */
    init_lru_maintainer();

    /* set stderr non-buffering (for running under, say, daemontools) */
    setbuf(stderr, NULL);

    char *shortopts =
          "a:"  /* access mask for unix socket */
          "A"   /* enable admin shutdown command */
          "Z"   /* enable SSL */
          "p:"  /* TCP port number to listen on */
          "s:"  /* unix socket path to listen on */
          "U:"  /* UDP port number to listen on */
          "m:"  /* max memory to use for items in megabytes */
          "M"   /* return error on memory exhausted */
          "c:"  /* max simultaneous connections */
#ifndef DISABLE_MLOCKALL
          "k"   /* lock down all paged memory */
#endif /* #ifndef DISABLE_MLOCKALL */
          "hiV" /* help, licence info, version */
          "r"   /* maximize core file limit */
          "v"   /* verbose */
          "d"   /* daemon mode */
          "l:"  /* interface to listen on */
#ifndef DISABLE_RUNAS_USER
          "u:"  /* user identity to run as */
#endif /* #ifndef DISABLE_RUNAS_USER */
          "P:"  /* save PID in file */
          "f:"  /* factor? */
          "n:"  /* minimum space allocated for key+value+flags */
          "t:"  /* threads */
          "D:"  /* prefix delimiter? */
          "L"   /* Large memory pages */
          "R:"  /* max requests per event */
          "C"   /* Disable use of CAS */
          "b:"  /* backlog queue limit */
          "B:"  /* Binding protocol */
          "I:"  /* Max item size */
          "S"   /* Sasl ON */
          "F"   /* Disable flush_all */
          "X"   /* Disable dump commands */
          "W"   /* Disable watch commands */
          "Y:"   /* Enable token auth */
          "e:"  /* mmap path for external item memory */
          "o:"  /* Extended generic options */
          ;

    /* process arguments */
#ifdef HAVE_GETOPT_LONG
    const struct option longopts[] = {
        {"unix-mask", required_argument, 0, 'a'},
        {"enable-shutdown", no_argument, 0, 'A'},
        {"enable-ssl", no_argument, 0, 'Z'},
        {"port", required_argument, 0, 'p'},
        {"unix-socket", required_argument, 0, 's'},
        {"udp-port", required_argument, 0, 'U'},
        {"memory-limit", required_argument, 0, 'm'},
        {"disable-evictions", no_argument, 0, 'M'},
        {"conn-limit", required_argument, 0, 'c'},
#ifndef DISABLE_MLOCKALL
        {"lock-memory", no_argument, 0, 'k'},
#endif /* #ifndef DISABLE_MLOCKALL */
        {"help", no_argument, 0, 'h'},
        {"license", no_argument, 0, 'i'},
        {"version", no_argument, 0, 'V'},
#ifndef DISABLE_COREDUMP
        {"enable-coredumps", no_argument, 0, 'r'},
#endif /* #ifndef DISABLE_COREDUMP */
        {"verbose", optional_argument, 0, 'v'},
        {"daemon", no_argument, 0, 'd'},
        {"listen", required_argument, 0, 'l'},
#ifndef DISABLE_RUNAS_USER
        {"user", required_argument, 0, 'u'},
#endif /* #ifndef DISABLE_RUNAS_USER */
        {"pidfile", required_argument, 0, 'P'},
        {"slab-growth-factor", required_argument, 0, 'f'},
        {"slab-min-size", required_argument, 0, 'n'},
        {"threads", required_argument, 0, 't'},
        {"enable-largepages", no_argument, 0, 'L'},
        {"max-reqs-per-event", required_argument, 0, 'R'},
        {"disable-cas", no_argument, 0, 'C'},
        {"listen-backlog", required_argument, 0, 'b'},
        {"protocol", required_argument, 0, 'B'},
        {"max-item-size", required_argument, 0, 'I'},
        {"enable-sasl", no_argument, 0, 'S'},
        {"disable-flush-all", no_argument, 0, 'F'},
        {"disable-dumping", no_argument, 0, 'X'},
        {"disable-watch", no_argument, 0, 'W'},
        {"auth-file", required_argument, 0, 'Y'},
        {"memory-file", required_argument, 0, 'e'},
        {"extended", required_argument, 0, 'o'},
        {0, 0, 0, 0}
    };
    int optindex;
    while (-1 != (c = getopt_long(argc, argv, shortopts,
                    longopts, &optindex))) {
#else
    while (-1 != (c = getopt(argc, argv, shortopts))) {
#endif
        switch (c) {
        case 'A':
            /* enables "shutdown" command */
            settings.shutdown_command = true;
            break;
        case 'Z':
            /* enable secure communication*/
#ifdef TLS
            settings.ssl_enabled = true;
#else
            fprintf(stderr, "This server is not built with TLS support.\n");
            exit(EX_USAGE);
#endif
            break;
        case 'a':
#ifndef DISABLE_UNIX_SOCKET
            /* access for unix domain socket, as octal mask (like chmod)*/
            settings.access= strtol(optarg,NULL,8);
#else
            fprintf(stderr, "This server is not built with unix socket support.\n");
            exit(EX_USAGE);
#endif /* #ifndef DISABLE_UNIX_SOCKET */
            break;
        case 'U':
            settings.udpport = atoi(optarg);
            udp_specified = true;
            break;
        case 'p':
            settings.port = atoi(optarg);
            tcp_specified = true;
            break;
        case 's':
#ifndef DISABLE_UNIX_SOCKET
            settings.socketpath = optarg;
#else
            fprintf(stderr, "This server is not built with unix socket support.\n");
            exit(EX_USAGE);
#endif /* #ifndef DISABLE_UNIX_SOCKET */
            break;
        case 'm':
            settings.maxbytes = ((size_t)atoi(optarg)) * 1024 * 1024;
            break;
        case 'M':
            settings.evict_to_free = 0;
            break;
        case 'c':
            settings.maxconns = atoi(optarg);
            if (settings.maxconns <= 0) {
                fprintf(stderr, "Maximum connections must be greater than 0\n");
                return 1;
            }
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 'i':
            usage_license();
            exit(EXIT_SUCCESS);
        case 'V':
            printf(PACKAGE " " VERSION "\n");
            exit(EXIT_SUCCESS);
#ifndef DISABLE_MLOCKALL
        case 'k':
            lock_memory = true;
            break;
#endif /* #ifndef DISABLE_MLOCKALL */
        case 'v':
            settings.verbose++;
            break;
        case 'l':
            if (settings.inter != NULL) {
                if (strstr(settings.inter, optarg) != NULL) {
                    break;
                }
                size_t len = strlen(settings.inter) + strlen(optarg) + 2;
                char *p = malloc(len);
                if (p == NULL) {
                    fprintf(stderr, "Failed to allocate memory\n");
                    return 1;
                }
                snprintf(p, len, "%s,%s", settings.inter, optarg);
                free(settings.inter);
                settings.inter = p;
            } else {
                settings.inter= strdup(optarg);
            }
            break;
        case 'd':
#ifdef _WIN32
            /* Daemon mode in Windows will just run the same command line in background
             * and exits immediately the calling process. It's using an env key for
             * the child process not to exit and proceed as normal process.
             */
            daemonize_cmd(argv, "MEMCACHE_DAEMON_ENV");
#endif /* #ifdef _WIN32 */
            do_daemonize = true;
            break;
#ifndef DISABLE_COREDUMP
        case 'r':
            maxcore = 1;
            break;
#endif /* #ifndef DISABLE_COREDUMP */
        case 'R':
            settings.reqs_per_event = atoi(optarg);
            if (settings.reqs_per_event == 0) {
                fprintf(stderr, "Number of requests per event must be greater than 0\n");
                return 1;
            }
            break;
#ifndef DISABLE_RUNAS_USER
        case 'u':
            username = optarg;
            break;
#endif /* #ifndef DISABLE_RUNAS_USER */
        case 'P':
            pid_file = optarg;
            break;
        case 'e':
            memory_file = optarg;
            break;
        case 'f':
            settings.factor = atof(optarg);
            if (settings.factor <= 1.0) {
                fprintf(stderr, "Factor must be greater than 1\n");
                return 1;
            }
            meta->slab_config = strdup(optarg);
            break;
        case 'n':
            settings.chunk_size = atoi(optarg);
            if (settings.chunk_size == 0) {
                fprintf(stderr, "Chunk size must be greater than 0\n");
                return 1;
            }
            break;
        case 't':
            settings.num_threads = atoi(optarg);
            if (settings.num_threads <= 0) {
                fprintf(stderr, "Number of threads must be greater than 0\n");
                return 1;
            }
            /* There're other problems when you get above 64 threads.
             * In the future we should portably detect # of cores for the
             * default.
             */
            if (settings.num_threads > 64) {
                fprintf(stderr, "WARNING: Setting a high number of worker"
                                "threads is not recommended.\n"
                                " Set this value to the number of cores in"
                                " your machine or less.\n");
            }
            break;
        case 'D':
            if (! optarg || ! optarg[0]) {
                fprintf(stderr, "No delimiter specified\n");
                return 1;
            }
            settings.prefix_delimiter = optarg[0];
            settings.detail_enabled = 1;
            break;
        case 'L' :
            if (enable_large_pages() == 0) {
                preallocate = true;
            } else {
                fprintf(stderr, "Cannot enable large pages on this system\n"
                    "(There is no support as of this version)\n");
                return 1;
            }
            break;
        case 'C' :
            settings.use_cas = false;
            break;
        case 'b' :
            settings.backlog = atoi(optarg);
            break;
        case 'B':
            protocol_specified = true;
            if (strcmp(optarg, "auto") == 0) {
                settings.binding_protocol = negotiating_prot;
            } else if (strcmp(optarg, "binary") == 0) {
                settings.binding_protocol = binary_prot;
            } else if (strcmp(optarg, "ascii") == 0) {
                settings.binding_protocol = ascii_prot;
            } else {
                fprintf(stderr, "Invalid value for binding protocol: %s\n"
                        " -- should be one of auto, binary, or ascii\n", optarg);
                exit(EX_USAGE);
            }
            break;
        case 'I':
            buf = strdup(optarg);
            unit = buf[strlen(buf)-1];
            if (unit == 'k' || unit == 'm' ||
                unit == 'K' || unit == 'M') {
                buf[strlen(buf)-1] = '\0';
                size_max = atoi(buf);
                if (unit == 'k' || unit == 'K')
                    size_max *= 1024;
                if (unit == 'm' || unit == 'M')
                    size_max *= 1024 * 1024;
                settings.item_size_max = size_max;
            } else {
                settings.item_size_max = atoi(buf);
            }
            free(buf);
            break;
        case 'S': /* set Sasl authentication to true. Default is false */
#ifndef ENABLE_SASL
            fprintf(stderr, "This server is not built with SASL support.\n");
            exit(EX_USAGE);
#endif
            settings.sasl = true;
            break;
       case 'F' :
            settings.flush_enabled = false;
            break;
       case 'X' :
            settings.dump_enabled = false;
            break;
       case 'W' :
            settings.watch_enabled = false;
            break;
       case 'Y' :
            // dupe the file path now just in case the options get mangled.
            settings.auth_file = strdup(optarg);
            break;
        case 'o': /* It's sub-opts time! */
            subopts_orig = subopts = strdup(optarg); /* getsubopt() changes the original args */

            while (*subopts != '\0') {

            switch (getsubopt(&subopts, subopts_tokens, &subopts_value)) {
            case MAXCONNS_FAST:
                settings.maxconns_fast = true;
                break;
            case HASHPOWER_INIT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing numeric argument for hashpower\n");
                    return 1;
                }
                settings.hashpower_init = atoi(subopts_value);
                if (settings.hashpower_init < 12) {
                    fprintf(stderr, "Initial hashtable multiplier of %d is too low\n",
                        settings.hashpower_init);
                    return 1;
                } else if (settings.hashpower_init > 32) {
                    fprintf(stderr, "Initial hashtable multiplier of %d is too high\n"
                        "Choose a value based on \"STAT hash_power_level\" from a running instance\n",
                        settings.hashpower_init);
                    return 1;
                }
                break;
            case NO_HASHEXPAND:
                start_assoc_maint = false;
                break;
            case SLAB_REASSIGN:
                settings.slab_reassign = true;
                break;
            case SLAB_AUTOMOVE:
                if (subopts_value == NULL) {
                    settings.slab_automove = 1;
                    break;
                }
                settings.slab_automove = atoi(subopts_value);
                if (settings.slab_automove < 0 || settings.slab_automove > 2) {
                    fprintf(stderr, "slab_automove must be between 0 and 2\n");
                    return 1;
                }
                break;
            case SLAB_AUTOMOVE_RATIO:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing slab_automove_ratio argument\n");
                    return 1;
                }
                settings.slab_automove_ratio = atof(subopts_value);
                if (settings.slab_automove_ratio <= 0 || settings.slab_automove_ratio > 1) {
                    fprintf(stderr, "slab_automove_ratio must be > 0 and < 1\n");
                    return 1;
                }
                break;
            case SLAB_AUTOMOVE_WINDOW:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing slab_automove_window argument\n");
                    return 1;
                }
                settings.slab_automove_window = atoi(subopts_value);
                if (settings.slab_automove_window < 3) {
                    fprintf(stderr, "slab_automove_window must be > 2\n");
                    return 1;
                }
                break;
            case TAIL_REPAIR_TIME:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing numeric argument for tail_repair_time\n");
                    return 1;
                }
                settings.tail_repair_time = atoi(subopts_value);
                if (settings.tail_repair_time < 10) {
                    fprintf(stderr, "Cannot set tail_repair_time to less than 10 seconds\n");
                    return 1;
                }
                break;
            case HASH_ALGORITHM:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing hash_algorithm argument\n");
                    return 1;
                };
                if (strcmp(subopts_value, "jenkins") == 0) {
                    hash_type = JENKINS_HASH;
                } else if (strcmp(subopts_value, "murmur3") == 0) {
                    hash_type = MURMUR3_HASH;
                } else {
                    fprintf(stderr, "Unknown hash_algorithm option (jenkins, murmur3)\n");
                    return 1;
                }
                break;
            case LRU_CRAWLER:
                start_lru_crawler = true;
                break;
            case LRU_CRAWLER_SLEEP:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing lru_crawler_sleep value\n");
                    return 1;
                }
                settings.lru_crawler_sleep = atoi(subopts_value);
                if (settings.lru_crawler_sleep > 1000000 || settings.lru_crawler_sleep < 0) {
                    fprintf(stderr, "LRU crawler sleep must be between 0 and 1 second\n");
                    return 1;
                }
                break;
            case LRU_CRAWLER_TOCRAWL:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing lru_crawler_tocrawl value\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &tocrawl)) {
                    fprintf(stderr, "lru_crawler_tocrawl takes a numeric 32bit value\n");
                    return 1;
                }
                settings.lru_crawler_tocrawl = tocrawl;
                break;
            case LRU_MAINTAINER:
                start_lru_maintainer = true;
                settings.lru_segmented = true;
                break;
            case HOT_LRU_PCT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing hot_lru_pct argument\n");
                    return 1;
                }
                settings.hot_lru_pct = atoi(subopts_value);
                if (settings.hot_lru_pct < 1 || settings.hot_lru_pct >= 80) {
                    fprintf(stderr, "hot_lru_pct must be > 1 and < 80\n");
                    return 1;
                }
                break;
            case WARM_LRU_PCT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing warm_lru_pct argument\n");
                    return 1;
                }
                settings.warm_lru_pct = atoi(subopts_value);
                if (settings.warm_lru_pct < 1 || settings.warm_lru_pct >= 80) {
                    fprintf(stderr, "warm_lru_pct must be > 1 and < 80\n");
                    return 1;
                }
                break;
            case HOT_MAX_FACTOR:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing hot_max_factor argument\n");
                    return 1;
                }
                settings.hot_max_factor = atof(subopts_value);
                if (settings.hot_max_factor <= 0) {
                    fprintf(stderr, "hot_max_factor must be > 0\n");
                    return 1;
                }
                break;
            case WARM_MAX_FACTOR:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing warm_max_factor argument\n");
                    return 1;
                }
                settings.warm_max_factor = atof(subopts_value);
                if (settings.warm_max_factor <= 0) {
                    fprintf(stderr, "warm_max_factor must be > 0\n");
                    return 1;
                }
                break;
            case TEMPORARY_TTL:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing temporary_ttl argument\n");
                    return 1;
                }
                settings.temp_lru = true;
                settings.temporary_ttl = atoi(subopts_value);
                break;
            case IDLE_TIMEOUT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing numeric argument for idle_timeout\n");
                    return 1;
                }
                settings.idle_timeout = atoi(subopts_value);
                break;
            case WATCHER_LOGBUF_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing watcher_logbuf_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.logger_watcher_buf_size)) {
                    fprintf(stderr, "could not parse argument to watcher_logbuf_size\n");
                    return 1;
                }
                settings.logger_watcher_buf_size *= 1024; /* kilobytes */
                break;
            case WORKER_LOGBUF_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing worker_logbuf_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.logger_buf_size)) {
                    fprintf(stderr, "could not parse argument to worker_logbuf_size\n");
                    return 1;
                }
                settings.logger_buf_size *= 1024; /* kilobytes */
            case SLAB_SIZES:
                slab_sizes_unparsed = strdup(subopts_value);
                break;
            case SLAB_CHUNK_MAX:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing slab_chunk_max argument\n");
                }
                if (!safe_strtol(subopts_value, &settings.slab_chunk_size_max)) {
                    fprintf(stderr, "could not parse argument to slab_chunk_max\n");
                }
                slab_chunk_size_changed = true;
                break;
            case TRACK_SIZES:
                item_stats_sizes_init();
                break;
            case NO_INLINE_ASCII_RESP:
                break;
            case INLINE_ASCII_RESP:
                break;
            case NO_CHUNKED_ITEMS:
                settings.slab_chunk_size_max = settings.slab_page_size;
                break;
            case NO_SLAB_REASSIGN:
                settings.slab_reassign = false;
                break;
            case NO_SLAB_AUTOMOVE:
                settings.slab_automove = 0;
                break;
            case NO_MAXCONNS_FAST:
                settings.maxconns_fast = false;
                break;
            case NO_LRU_CRAWLER:
                settings.lru_crawler = false;
                start_lru_crawler = false;
                break;
            case NO_LRU_MAINTAINER:
                start_lru_maintainer = false;
                settings.lru_segmented = false;
                break;
#ifdef TLS
            case SSL_CERT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ssl_chain_cert argument\n");
                    return 1;
                }
                settings.ssl_chain_cert = strdup(subopts_value);
                break;
            case SSL_KEY:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ssl_key argument\n");
                    return 1;
                }
                settings.ssl_key = strdup(subopts_value);
                break;
            case SSL_VERIFY_MODE:
            {
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ssl_verify_mode argument\n");
                    return 1;
                }
                int verify  = 0;
                if (!safe_strtol(subopts_value, &verify)) {
                    fprintf(stderr, "could not parse argument to ssl_verify_mode\n");
                    return 1;
                }
                switch(verify) {
                    case 0:
                        settings.ssl_verify_mode = SSL_VERIFY_NONE;
                        break;
                    case 1:
                        settings.ssl_verify_mode = SSL_VERIFY_PEER;
                        break;
                    case 2:
                        settings.ssl_verify_mode = SSL_VERIFY_PEER |
                                                    SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
                        break;
                    case 3:
                        settings.ssl_verify_mode = SSL_VERIFY_PEER |
                                                    SSL_VERIFY_FAIL_IF_NO_PEER_CERT |
                                                    SSL_VERIFY_CLIENT_ONCE;
                        break;
                    default:
                        fprintf(stderr, "Invalid ssl_verify_mode. Use help to see valid options.\n");
                        return 1;
                }
                break;
            }
            case SSL_KEYFORM:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ssl_keyformat argument\n");
                    return 1;
                }
                if (!safe_strtol(subopts_value, &settings.ssl_keyformat)) {
                    fprintf(stderr, "could not parse argument to ssl_keyformat\n");
                    return 1;
                }
                break;
            case SSL_CIPHERS:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ssl_ciphers argument\n");
                    return 1;
                }
                settings.ssl_ciphers = strdup(subopts_value);
                break;
            case SSL_CA_CERT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ssl_ca_cert argument\n");
                    return 1;
                }
                settings.ssl_ca_cert = strdup(subopts_value);
                break;
            case SSL_WBUF_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ssl_wbuf_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ssl_wbuf_size)) {
                    fprintf(stderr, "could not parse argument to ssl_wbuf_size\n");
                    return 1;
                }
                settings.ssl_wbuf_size *= 1024; /* kilobytes */
                break;
            case SSL_SESSION_CACHE:
                settings.ssl_session_cache = true;
                break;
#endif
#ifdef EXTSTORE
            case EXT_PAGE_SIZE:
                if (storage_file) {
                    fprintf(stderr, "Must specify ext_page_size before any ext_path arguments\n");
                    return 1;
                }
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_page_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &ext_cf.page_size)) {
                    fprintf(stderr, "could not parse argument to ext_page_size\n");
                    return 1;
                }
                ext_cf.page_size *= 1024 * 1024; /* megabytes */
                break;
            case EXT_WBUF_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_wbuf_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &ext_cf.wbuf_size)) {
                    fprintf(stderr, "could not parse argument to ext_wbuf_size\n");
                    return 1;
                }
                ext_cf.wbuf_size *= 1024 * 1024; /* megabytes */
                settings.ext_wbuf_size = ext_cf.wbuf_size;
                break;
            case EXT_THREADS:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_threads argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &ext_cf.io_threadcount)) {
                    fprintf(stderr, "could not parse argument to ext_threads\n");
                    return 1;
                }
                break;
            case EXT_IO_DEPTH:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_io_depth argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &ext_cf.io_depth)) {
                    fprintf(stderr, "could not parse argument to ext_io_depth\n");
                    return 1;
                }
                break;
            case EXT_ITEM_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_item_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_item_size)) {
                    fprintf(stderr, "could not parse argument to ext_item_size\n");
                    return 1;
                }
                break;
            case EXT_ITEM_AGE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_item_age argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_item_age)) {
                    fprintf(stderr, "could not parse argument to ext_item_age\n");
                    return 1;
                }
                break;
            case EXT_LOW_TTL:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_low_ttl argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_low_ttl)) {
                    fprintf(stderr, "could not parse argument to ext_low_ttl\n");
                    return 1;
                }
                break;
            case EXT_RECACHE_RATE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_recache_rate argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_recache_rate)) {
                    fprintf(stderr, "could not parse argument to ext_recache_rate\n");
                    return 1;
                }
                break;
            case EXT_COMPACT_UNDER:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_compact_under argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_compact_under)) {
                    fprintf(stderr, "could not parse argument to ext_compact_under\n");
                    return 1;
                }
                break;
            case EXT_DROP_UNDER:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_drop_under argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_drop_under)) {
                    fprintf(stderr, "could not parse argument to ext_drop_under\n");
                    return 1;
                }
                break;
            case EXT_MAX_FRAG:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_max_frag argument\n");
                    return 1;
                }
                if (!safe_strtod(subopts_value, &settings.ext_max_frag)) {
                    fprintf(stderr, "could not parse argument to ext_max_frag\n");
                    return 1;
                }
                break;
            case SLAB_AUTOMOVE_FREERATIO:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing slab_automove_freeratio argument\n");
                    return 1;
                }
                if (!safe_strtod(subopts_value, &settings.slab_automove_freeratio)) {
                    fprintf(stderr, "could not parse argument to slab_automove_freeratio\n");
                    return 1;
                }
                break;
            case EXT_DROP_UNREAD:
                settings.ext_drop_unread = true;
                break;
            case EXT_PATH:
                if (subopts_value) {
                    struct extstore_conf_file *tmp = storage_conf_parse(subopts_value, ext_cf.page_size);
                    if (tmp == NULL) {
                        fprintf(stderr, "failed to parse ext_path argument\n");
                        return 1;
                    }
                    if (storage_file != NULL) {
                        tmp->next = storage_file;
                    }
                    storage_file = tmp;
                } else {
                    fprintf(stderr, "missing argument to ext_path, ie: ext_path=/d/file:5G\n");
                    return 1;
                }
                break;
#endif
            case MODERN:
                /* currently no new defaults */
                break;
            case NO_MODERN:
                if (!slab_chunk_size_changed) {
                    settings.slab_chunk_size_max = settings.slab_page_size;
                }
                settings.slab_reassign = false;
                settings.slab_automove = 0;
                settings.maxconns_fast = false;
                settings.lru_segmented = false;
                hash_type = JENKINS_HASH;
                start_lru_crawler = false;
                start_lru_maintainer = false;
                break;
            case NO_DROP_PRIVILEGES:
                settings.drop_privileges = false;
                break;
            case DROP_PRIVILEGES:
                settings.drop_privileges = true;
                break;
            case RESP_OBJ_MEM_LIMIT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing resp_obj_mem_limit argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.resp_obj_mem_limit)) {
                    fprintf(stderr, "could not parse argument to resp_obj_mem_limit\n");
                    return 1;
                }
                settings.resp_obj_mem_limit *= 1024 * 1024; /* megabytes */
                break;
            case READ_BUF_MEM_LIMIT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing read_buf_mem_limit argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.read_buf_mem_limit)) {
                    fprintf(stderr, "could not parse argument to read_buf_mem_limit\n");
                    return 1;
                }
                settings.read_buf_mem_limit *= 1024 * 1024; /* megabytes */
                break;
#ifdef MEMCACHED_DEBUG
            case RELAXED_PRIVILEGES:
                settings.relaxed_privileges = true;
                break;
#endif
            default:
                printf("Illegal suboption \"%s\"\n", subopts_value);
                return 1;
            }

            }
            free(subopts_orig);
            break;
        default:
            fprintf(stderr, "Illegal argument \"%c\"\n", c);
            return 1;
        }
    }

    if (settings.item_size_max < ITEM_SIZE_MAX_LOWER_LIMIT) {
        fprintf(stderr, "Item max size cannot be less than 1024 bytes.\n");
        exit(EX_USAGE);
    }
    if (settings.item_size_max > (settings.maxbytes / 2)) {
        fprintf(stderr, "Cannot set item size limit higher than 1/2 of memory max.\n");
        exit(EX_USAGE);
    }
    if (settings.item_size_max > (ITEM_SIZE_MAX_UPPER_LIMIT)) {
        fprintf(stderr, "Cannot set item size limit higher than a gigabyte.\n");
        exit(EX_USAGE);
    }
    if (settings.item_size_max > 1024 * 1024) {
        if (!slab_chunk_size_changed) {
            // Ideal new default is 16k, but needs stitching.
            settings.slab_chunk_size_max = settings.slab_page_size / 2;
        }
    }

    if (settings.slab_chunk_size_max > settings.item_size_max) {
        fprintf(stderr, "slab_chunk_max (bytes: %d) cannot be larger than -I (item_size_max %d)\n",
                settings.slab_chunk_size_max, settings.item_size_max);
        exit(EX_USAGE);
    }

    if (settings.item_size_max % settings.slab_chunk_size_max != 0) {
        fprintf(stderr, "-I (item_size_max: %d) must be evenly divisible by slab_chunk_max (bytes: %d)\n",
                settings.item_size_max, settings.slab_chunk_size_max);
        exit(EX_USAGE);
    }

    if (settings.slab_page_size % settings.slab_chunk_size_max != 0) {
        fprintf(stderr, "slab_chunk_max (bytes: %d) must divide evenly into %d (slab_page_size)\n",
                settings.slab_chunk_size_max, settings.slab_page_size);
        exit(EX_USAGE);
    }
#ifdef EXTSTORE
    if (storage_file) {
        if (settings.item_size_max > ext_cf.wbuf_size) {
            fprintf(stderr, "-I (item_size_max: %d) cannot be larger than ext_wbuf_size: %d\n",
                settings.item_size_max, ext_cf.wbuf_size);
            exit(EX_USAGE);
        }

        if (settings.udpport) {
            fprintf(stderr, "Cannot use UDP with extstore enabled (-U 0 to disable)\n");
            exit(EX_USAGE);
        }
    }
#endif
    // Reserve this for the new default. If factor size hasn't changed, use
    // new default.
    /*if (settings.slab_chunk_size_max == 16384 && settings.factor == 1.25) {
        settings.factor = 1.08;
    }*/

    if (slab_sizes_unparsed != NULL) {
        // want the unedited string for restart code.
        char *temp = strdup(slab_sizes_unparsed);
        if (_parse_slab_sizes(slab_sizes_unparsed, slab_sizes)) {
            use_slab_sizes = true;
            if (meta->slab_config) {
                free(meta->slab_config);
            }
            meta->slab_config = temp;
        } else {
            exit(EX_USAGE);
        }
    } else if (!meta->slab_config) {
        // using the default factor.
        meta->slab_config = "1.25";
    }

    if (settings.hot_lru_pct + settings.warm_lru_pct > 80) {
        fprintf(stderr, "hot_lru_pct + warm_lru_pct cannot be more than 80%% combined\n");
        exit(EX_USAGE);
    }

    if (settings.temp_lru && !start_lru_maintainer) {
        fprintf(stderr, "temporary_ttl requires lru_maintainer to be enabled\n");
        exit(EX_USAGE);
    }

    if (hash_init(hash_type) != 0) {
        fprintf(stderr, "Failed to initialize hash_algorithm!\n");
        exit(EX_USAGE);
    }

    /*
     * Use one workerthread to serve each UDP port if the user specified
     * multiple ports
     */
    if (settings.inter != NULL && strchr(settings.inter, ',')) {
        settings.num_threads_per_udp = 1;
    } else {
        settings.num_threads_per_udp = settings.num_threads;
    }

    if (settings.sasl) {
        if (!protocol_specified) {
            settings.binding_protocol = binary_prot;
        } else {
            if (settings.binding_protocol != binary_prot) {
                fprintf(stderr, "ERROR: You cannot allow the ASCII protocol while using SASL.\n");
                exit(EX_USAGE);
            }
        }

        if (settings.udpport) {
            fprintf(stderr, "ERROR: Cannot enable UDP while using binary SASL authentication.\n");
            exit(EX_USAGE);
        }
    }

    if (settings.auth_file) {
        if (!protocol_specified) {
            settings.binding_protocol = ascii_prot;
        } else {
            if (settings.binding_protocol != ascii_prot) {
                fprintf(stderr, "ERROR: You cannot allow the BINARY protocol while using ascii authentication tokens.\n");
                exit(EX_USAGE);
            }
        }
    }

    if (udp_specified && settings.udpport != 0 && !tcp_specified) {
        settings.port = settings.udpport;
    }


#ifdef TLS
    /*
     * Setup SSL if enabled
     */
    if (settings.ssl_enabled) {
        if (!settings.port) {
            fprintf(stderr, "ERROR: You cannot enable SSL without a TCP port.\n");
            exit(EX_USAGE);
        }
        // openssl init methods.
        SSL_load_error_strings();
        SSLeay_add_ssl_algorithms();
        // Initiate the SSL context.
        ssl_init();
    }
#endif

#ifndef DISABLE_COREDUMP
    if (maxcore != 0) {
        struct rlimit rlim_new;
        /*
         * First try raising to infinity; if that fails, try bringing
         * the soft limit to the hard.
         */
        if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
            rlim_new.rlim_cur = rlim_new.rlim_max = RLIM_INFINITY;
            if (setrlimit(RLIMIT_CORE, &rlim_new)!= 0) {
                /* failed. try raising just to the old max */
                rlim_new.rlim_cur = rlim_new.rlim_max = rlim.rlim_max;
                (void)setrlimit(RLIMIT_CORE, &rlim_new);
            }
        }
        /*
         * getrlimit again to see what we ended up with. Only fail if
         * the soft limit ends up 0, because then no core files will be
         * created at all.
         */

        if ((getrlimit(RLIMIT_CORE, &rlim) != 0) || rlim.rlim_cur == 0) {
            fprintf(stderr, "failed to ensure corefile creation\n");
            exit(EX_OSERR);
        }
    }
#endif /* #ifndef DISABLE_COREDUMP */

    /*
     * If needed, increase rlimits to allow as many connections
     * as needed.
     */
#ifndef DISABLE_RLIMIT_NOFILE
    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fprintf(stderr, "failed to getrlimit number of files\n");
        exit(EX_OSERR);
    } else {
        rlim.rlim_cur = settings.maxconns;
        rlim.rlim_max = settings.maxconns;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            fprintf(stderr, "failed to set rlimit for open files. Try starting as root or requesting smaller maxconns value.\n");
            exit(EX_OSERR);
        }
    }
#endif /* #ifndef DISABLE_RLIMIT_NOFILE */

#ifndef DISABLE_RUNAS_USER
    /* lose root privileges if we have them */
    if (getuid() == 0 || geteuid() == 0) {
        if (username == 0 || *username == '\0') {
            fprintf(stderr, "can't run as root without the -u switch\n");
            exit(EX_USAGE);
        }
        if ((pw = getpwnam(username)) == 0) {
            fprintf(stderr, "can't find the user %s to switch to\n", username);
            exit(EX_NOUSER);
        }
        if (setgroups(0, NULL) < 0) {
            /* setgroups may fail with EPERM, indicating we are already in a
             * minimally-privileged state. In that case we continue. For all
             * other failure codes we exit.
             *
             * Note that errno is stored here because fprintf may change it.
             */
            bool should_exit = errno != EPERM;
            fprintf(stderr, "failed to drop supplementary groups: %s\n",
                    strerror(errno));
            if (should_exit) {
                exit(EX_OSERR);
            }
        }
        if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            fprintf(stderr, "failed to assume identity of user %s\n", username);
            exit(EX_OSERR);
        }
    }
#endif /* #ifndef DISABLE_RUNAS_USER */

    /* Initialize Sasl if -S was specified */
    if (settings.sasl) {
        init_sasl();
    }

#ifndef _WIN32
    /* daemonize if requested */
    /* if we want to ensure our ability to dump core, don't chdir to / */
    if (do_daemonize) {
        if (sigignore(SIGHUP) == -1) {
            perror("Failed to ignore SIGHUP");
        }
        if (daemonize(maxcore, settings.verbose) == -1) {
            fprintf(stderr, "failed to daemon() in order to daemonize\n");
            exit(EXIT_FAILURE);
        }
    }
#endif /* #ifndef _WIN32 */

#ifndef DISABLE_MLOCKALL
    /* lock paged memory if needed */
    if (lock_memory) {
#ifdef HAVE_MLOCKALL
        int res = mlockall(MCL_CURRENT | MCL_FUTURE);
        if (res != 0) {
            fprintf(stderr, "warning: -k invalid, mlockall() failed: %s\n",
                    strerror(errno));
        }
#else
        fprintf(stderr, "warning: -k invalid, mlockall() not supported on this platform.  proceeding without.\n");
#endif
    }
#endif /* #ifndef DISABLE_MLOCKALL */

    /* initialize main thread libevent instance */
#if defined(LIBEVENT_VERSION_NUMBER) && LIBEVENT_VERSION_NUMBER >= 0x02000101
    /* If libevent version is larger/equal to 2.0.2-alpha, use newer version */
    struct event_config *ev_config;
    ev_config = event_config_new();
    event_config_set_flag(ev_config, EVENT_BASE_FLAG_NOLOCK);
    main_base = event_base_new_with_config(ev_config);
    event_config_free(ev_config);
#else
    /* Otherwise, use older API */
    main_base = event_init();
#endif

    /* Load initial auth file if required */
    if (settings.auth_file) {
        if (settings.udpport) {
            fprintf(stderr, "Cannot use UDP with ascii authentication enabled (-U 0 to disable)\n");
            exit(EX_USAGE);
        }

        switch (authfile_load(settings.auth_file)) {
            case AUTHFILE_MISSING: // fall through.
            case AUTHFILE_OPENFAIL:
                vperror("Could not open authfile [%s] for reading", settings.auth_file);
                exit(EXIT_FAILURE);
                break;
            case AUTHFILE_OOM:
                fprintf(stderr, "Out of memory reading password file: %s", settings.auth_file);
                exit(EXIT_FAILURE);
                break;
            case AUTHFILE_MALFORMED:
                fprintf(stderr, "Authfile [%s] has a malformed entry. Should be 'user:password'", settings.auth_file);
                exit(EXIT_FAILURE);
                break;
            case AUTHFILE_OK:
                break;
        }
    }

    /* initialize other stuff */
    stats_init();
    logger_init();
    conn_init();
    bool reuse_mem = false;
    void *mem_base = NULL;
    bool prefill = false;
    if (memory_file != NULL) {
        preallocate = true;
        // Easier to manage memory if we prefill the global pool when reusing.
        prefill = true;
        restart_register("main", _mc_meta_load_cb, _mc_meta_save_cb, meta);
        reuse_mem = restart_mmap_open(settings.maxbytes,
                        memory_file,
                        &mem_base);
        // The "save" callback gets called when we're closing out the mmap,
        // but we don't know what the mmap_base is until after we call open.
        // So we pass the struct above but have to fill it in here so the
        // data's available during the save routine.
        meta->mmap_base = mem_base;
        // Also, the callbacks for load() run before _open returns, so we
        // should have the old base in 'meta' as of here.
    }
    // Initialize the hash table _after_ checking restart metadata.
    // We override the hash table start argument with what was live
    // previously, to avoid filling a huge set of items into a tiny hash
    // table.
    assoc_init(settings.hashpower_init);
#ifdef EXTSTORE
    if (storage_file && reuse_mem) {
        fprintf(stderr, "[restart] memory restart with extstore not presently supported.\n");
        reuse_mem = false;
    }
#endif
    slabs_init(settings.maxbytes, settings.factor, preallocate,
            use_slab_sizes ? slab_sizes : NULL, mem_base, reuse_mem);
#ifdef EXTSTORE
    if (storage_file) {
        enum extstore_res eres;
        if (settings.ext_compact_under == 0) {
            // If changing the default fraction (4), change the help text as well.
            settings.ext_compact_under = storage_file->page_count / 4;
            /* Only rescues non-COLD items if below this threshold */
            settings.ext_drop_under = storage_file->page_count / 4;
        }
        crc32c_init();
        /* Init free chunks to zero. */
        for (int x = 0; x < MAX_NUMBER_OF_SLAB_CLASSES; x++) {
            settings.ext_free_memchunks[x] = 0;
        }
        storage = extstore_init(storage_file, &ext_cf, &eres);
        if (storage == NULL) {
            fprintf(stderr, "Failed to initialize external storage: %s\n",
                    extstore_err(eres));
            if (eres == EXTSTORE_INIT_OPEN_FAIL) {
                perror("extstore open");
            }
            exit(EXIT_FAILURE);
        }
        ext_storage = storage;
        /* page mover algorithm for extstore needs memory prefilled */
        prefill = true;
    }
#endif

    if (settings.drop_privileges) {
        setup_privilege_violations_handler();
    }

    if (prefill)
        slabs_prefill_global();
    /* In restartable mode and we've decided to issue a fixup on memory */
    if (memory_file != NULL && reuse_mem) {
        mc_ptr_t old_base = meta->old_base;
        assert(old_base == meta->old_base);

        // should've pulled in process_started from meta file.
        process_started = meta->process_started;
        // TODO: must be a more canonical way of serializing/deserializing
        // pointers? passing through uint64_t should work, and we're not
        // annotating the pointer with anything, but it's still slightly
        // insane.
        restart_fixup((void *)old_base);
    }
#ifndef WINDOWS_ONLY_SIGNALS
    /*
     * ignore SIGPIPE signals; we can use errno == EPIPE if we
     * need that information
     */
    if (sigignore(SIGPIPE) == -1) {
        perror("failed to ignore SIGPIPE; sigaction");
        exit(EX_OSERR);
    }
#endif /* #ifndef WINDOWS_ONLY_SIGNALS */
    /* start up worker threads if MT mode */
#ifdef EXTSTORE
    slabs_set_storage(storage);
    memcached_thread_init(settings.num_threads, storage);
    init_lru_crawler(storage);
#else
    memcached_thread_init(settings.num_threads, NULL);
    init_lru_crawler(NULL);
#endif

    if (start_assoc_maint && start_assoc_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }
    if (start_lru_crawler && start_item_crawler_thread() != 0) {
        fprintf(stderr, "Failed to enable LRU crawler thread\n");
        exit(EXIT_FAILURE);
    }
#ifdef EXTSTORE
    if (storage && start_storage_compact_thread(storage) != 0) {
        fprintf(stderr, "Failed to start storage compaction thread\n");
        exit(EXIT_FAILURE);
    }
    if (storage && start_storage_write_thread(storage) != 0) {
        fprintf(stderr, "Failed to start storage writer thread\n");
        exit(EXIT_FAILURE);
    }

    if (start_lru_maintainer && start_lru_maintainer_thread(storage) != 0) {
#else
    if (start_lru_maintainer && start_lru_maintainer_thread(NULL) != 0) {
#endif
        fprintf(stderr, "Failed to enable LRU maintainer thread\n");
        free(meta);
        return 1;
    }

    if (settings.slab_reassign &&
        start_slab_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    if (settings.idle_timeout && start_conn_timeout_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    /* initialise clock event */
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            monotonic = true;
            monotonic_start = ts.tv_sec;
            // Monotonic clock needs special handling for restarts.
            // We get a start time at an arbitrary place, so we need to
            // restore the original time delta, which is always "now" - _start
            if (reuse_mem) {
                // the running timespan at stop time + the time we think we
                // were stopped.
                monotonic_start -= meta->current_time + meta->time_delta;
            } else {
                monotonic_start -= ITEM_UPDATE_INTERVAL + 2;
            }
        }
    }
#endif
    clock_handler(0, 0, 0);

    /* create unix mode sockets after dropping privileges */
    if (settings.socketpath != NULL) {
        errno = 0;
        if (server_socket_unix(settings.socketpath,settings.access)) {
            vperror("failed to listen on UNIX socket: %s", settings.socketpath);
            exit(EX_OSERR);
        }
    }

    /* create the listening socket, bind it, and init */
    if (settings.socketpath == NULL) {
        const char *portnumber_filename = getenv("MEMCACHED_PORT_FILENAME");
        char *temp_portnumber_filename = NULL;
        size_t len;
        FILE *portnumber_file = NULL;

        if (portnumber_filename != NULL) {
            len = strlen(portnumber_filename)+4+1;
            temp_portnumber_filename = malloc(len);
            snprintf(temp_portnumber_filename,
                     len,
                     "%s.lck", portnumber_filename);

            portnumber_file = fopen(temp_portnumber_filename, "a");
            if (portnumber_file == NULL) {
                fprintf(stderr, "Failed to open \"%s\": %s\n",
                        temp_portnumber_filename, strerror(errno));
            }
        }

        errno = 0;
        if (settings.port && server_sockets(settings.port, tcp_transport,
                                           portnumber_file)) {
            vperror("failed to listen on TCP port %d", settings.port);
            exit(EX_OSERR);
        }

        /*
         * initialization order: first create the listening sockets
         * (may need root on low ports), then drop root if needed,
         * then daemonize if needed, then init libevent (in some cases
         * descriptors created by libevent wouldn't survive forking).
         */

        /* create the UDP listening socket and bind it */
        errno = 0;
        if (settings.udpport && server_sockets(settings.udpport, udp_transport,
                                              portnumber_file)) {
            vperror("failed to listen on UDP port %d", settings.udpport);
            exit(EX_OSERR);
        }

        if (portnumber_file) {
            fclose(portnumber_file);
            rename(temp_portnumber_filename, portnumber_filename);
        }
        if (temp_portnumber_filename)
            free(temp_portnumber_filename);
    }

    /* Give the sockets a moment to open. I know this is dumb, but the error
     * is only an advisory.
     */
    usleep(1000);
    if (stats_state.curr_conns + stats_state.reserved_fds >= settings.maxconns - 1) {
        fprintf(stderr, "Maxconns setting is too low, use -c to increase.\n");
        exit(EXIT_FAILURE);
    }

    if (pid_file != NULL) {
        save_pid(pid_file);
    }

    /* Drop privileges no longer needed */
    if (settings.drop_privileges) {
        drop_privileges();
    }

    /* Initialize the uriencode lookup table. */
    uriencode_init();

    /* enter the event loop */
    while (!stop_main_loop) {
        if (event_base_loop(main_base, EVLOOP_ONCE) != 0) {
            retval = EXIT_FAILURE;
            break;
        }
    }

    fprintf(stderr, "Gracefully stopping\n");
    stop_threads();
    if (memory_file != NULL) {
        restart_mmap_close();
    }

    /* remove the PID file if we're a daemon */
    if (do_daemonize)
        remove_pidfile(pid_file);

    /* Clean up strdup() call for bind() address */
    if (settings.inter)
      free(settings.inter);

    /* cleanup base */
    event_base_free(main_base);

    free(meta);

#ifdef _WIN32
    sock_cleanup();
#endif /* #ifdef _WIN32 */

    return retval;
}
