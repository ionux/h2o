/*
 * Copyright (c) 2015 DeNA Co., Ltd., Kazuho Oku, Justin Zhu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>
#include <openssl/err.h>
#include "h2o/socket.h"
#include "h2o/timeout.h"

#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#ifndef IOV_MAX
#define IOV_MAX UIO_MAXIOV
#endif

/* kernel-headers bundled with Ubuntu 14.04 does not have the constant defined in netinet/tcp.h */
#if defined(__linux__) && !defined(TCP_NOTSENT_LOWAT)
#define TCP_NOTSENT_LOWAT 25
#endif

#define OPENSSL_HOSTNAME_VALIDATION_LINKAGE static
#include "../../deps/ssl-conservatory/openssl/openssl_hostname_validation.c"

struct st_h2o_socket_ssl_t {
    SSL *ssl;
    int *did_write_in_read; /* used for detecting and closing the connection upon renegotiation (FIXME implement renegotiation) */
    struct {
        h2o_socket_cb cb;
        union {
            struct {
                struct {
                    enum {
                        ASYNC_RESUMPTION_STATE_COMPLETE = 0, /* just pass thru */
                        ASYNC_RESUMPTION_STATE_RECORD,       /* record first input, restore SSL state if it changes to REQUEST_SENT
                                                                */
                        ASYNC_RESUMPTION_STATE_REQUEST_SENT  /* async request has been sent, and is waiting for response */
                    } state;
                    SSL_SESSION *session_data;
                } async_resumption;
            } server;
            struct {
                char *server_name;
            } client;
        };
    } handshake;
    struct {
        h2o_buffer_t *encrypted;
    } input;
    struct {
        H2O_VECTOR(h2o_iovec_t) bufs;
        h2o_mem_pool_t pool; /* placed at the last */
    } output;
};

struct st_h2o_ssl_context_t {
    SSL_CTX *ctx;
    const h2o_iovec_t *protocols;
    h2o_iovec_t _npn_list_of_protocols;
};

/* backend functions */
static void do_dispose_socket(h2o_socket_t *sock);
static void do_write(h2o_socket_t *sock, h2o_iovec_t *bufs, size_t bufcnt, h2o_socket_cb cb);
static void do_read_start(h2o_socket_t *sock);
static void do_read_stop(h2o_socket_t *sock);
static int do_export(h2o_socket_t *_sock, h2o_socket_export_t *info);
static h2o_socket_t *do_import(h2o_loop_t *loop, h2o_socket_export_t *info);
static socklen_t get_peername_uncached(h2o_socket_t *sock, struct sockaddr *sa);

/* internal functions called from the backend */
static const char *decode_ssl_input(h2o_socket_t *sock);
static void on_write_complete(h2o_socket_t *sock, const char *err);

#if H2O_USE_LIBUV
#include "socket/uv-binding.c.h"
#else
#include "socket/evloop.c.h"
#endif

h2o_buffer_mmap_settings_t h2o_socket_buffer_mmap_settings = {
    32 * 1024 * 1024, /* 32MB, should better be greater than max frame size of HTTP2 for performance reasons */
    "/tmp/h2o.b.XXXXXX"};

__thread h2o_buffer_prototype_t h2o_socket_buffer_prototype = {
    {16},                                       /* keep 16 recently used chunks */
    {H2O_SOCKET_INITIAL_INPUT_BUFFER_SIZE * 2}, /* minimum initial capacity */
    &h2o_socket_buffer_mmap_settings};

const char *h2o_socket_error_out_of_memory = "out of memory";
const char *h2o_socket_error_io = "I/O error";
const char *h2o_socket_error_closed = "socket closed by peer";
const char *h2o_socket_error_conn_fail = "connection failure";
const char *h2o_socket_error_ssl_no_cert = "no certificate";
const char *h2o_socket_error_ssl_cert_invalid = "invalid certificate";
const char *h2o_socket_error_ssl_cert_name_mismatch = "certificate name mismatch";
const char *h2o_socket_error_ssl_decode = "SSL decode error";

static void (*resumption_get_async)(h2o_socket_t *sock, h2o_iovec_t session_id);
static void (*resumption_new)(h2o_iovec_t session_id, h2o_iovec_t session_data);
static void (*resumption_remove)(h2o_iovec_t session_id);

static int read_bio(BIO *b, char *out, int len)
{
    h2o_socket_t *sock = b->ptr;

    if (len == 0)
        return 0;

    if (sock->ssl->input.encrypted->size == 0) {
        BIO_set_retry_read(b);
        return -1;
    }

    if (sock->ssl->input.encrypted->size < len) {
        len = (int)sock->ssl->input.encrypted->size;
    }
    memcpy(out, sock->ssl->input.encrypted->bytes, len);
    h2o_buffer_consume(&sock->ssl->input.encrypted, len);

    return len;
}

static int write_bio(BIO *b, const char *in, int len)
{
    h2o_socket_t *sock = b->ptr;
    void *bytes_alloced;

    /* FIXME no support for SSL renegotiation (yet) */
    if (sock->ssl->did_write_in_read != NULL) {
        *sock->ssl->did_write_in_read = 1;
        return -1;
    }

    if (len == 0)
        return 0;

    bytes_alloced = h2o_mem_alloc_pool(&sock->ssl->output.pool, len);
    memcpy(bytes_alloced, in, len);

    h2o_vector_reserve(&sock->ssl->output.pool, &sock->ssl->output.bufs, sock->ssl->output.bufs.size + 1);
    sock->ssl->output.bufs.entries[sock->ssl->output.bufs.size++] = h2o_iovec_init(bytes_alloced, len);

    return len;
}

static int puts_bio(BIO *b, const char *str)
{
    return write_bio(b, str, (int)strlen(str));
}

static long ctrl_bio(BIO *b, int cmd, long num, void *ptr)
{
    switch (cmd) {
    case BIO_CTRL_GET_CLOSE:
        return b->shutdown;
    case BIO_CTRL_SET_CLOSE:
        b->shutdown = (int)num;
        return 1;
    case BIO_CTRL_FLUSH:
        return 1;
    default:
        return 0;
    }
}

static int new_bio(BIO *b)
{
    b->init = 0;
    b->num = 0;
    b->ptr = NULL;
    b->flags = 0;
    return 1;
}

static int free_bio(BIO *b)
{
    return b != NULL;
}

static void setup_bio(h2o_socket_t *sock)
{
    static BIO_METHOD bio_methods = {BIO_TYPE_FD, "h2o_socket", write_bio, read_bio, puts_bio,
                                     NULL,        ctrl_bio,     new_bio,   free_bio, NULL};
    BIO *bio = BIO_new(&bio_methods);
    bio->ptr = sock;
    bio->init = 1;
    SSL_set_bio(sock->ssl->ssl, bio, bio);
}

const char *decode_ssl_input(h2o_socket_t *sock)
{
    assert(sock->ssl != NULL);
    assert(sock->ssl->handshake.cb == NULL);

    while (sock->ssl->input.encrypted->size != 0 || SSL_pending(sock->ssl->ssl)) {
        int rlen;
        h2o_iovec_t buf = h2o_buffer_reserve(&sock->input, 4096);
        if (buf.base == NULL)
            return h2o_socket_error_out_of_memory;
        { /* call SSL_read (while detecting SSL renegotiation and reporting it as error) */
            int did_write_in_read = 0;
            sock->ssl->did_write_in_read = &did_write_in_read;
            rlen = SSL_read(sock->ssl->ssl, buf.base, (int)buf.len);
            sock->ssl->did_write_in_read = NULL;
            if (did_write_in_read)
                return "ssl renegotiation not supported";
        }
        if (rlen == -1) {
            if (SSL_get_error(sock->ssl->ssl, rlen) != SSL_ERROR_WANT_READ) {
                return h2o_socket_error_ssl_decode;
            }
            break;
        } else if (rlen == 0) {
            break;
        } else {
            sock->input->size += rlen;
        }
    }

    return 0;
}

static void flush_pending_ssl(h2o_socket_t *sock, h2o_socket_cb cb)
{
    do_write(sock, sock->ssl->output.bufs.entries, sock->ssl->output.bufs.size, cb);
}

static void clear_output_buffer(struct st_h2o_socket_ssl_t *ssl)
{
    memset(&ssl->output.bufs, 0, sizeof(ssl->output.bufs));
    h2o_mem_clear_pool(&ssl->output.pool);
}

static void destroy_ssl(struct st_h2o_socket_ssl_t *ssl)
{
    if (!ssl->ssl->server)
        free(ssl->handshake.client.server_name);
    SSL_free(ssl->ssl);
    ssl->ssl = NULL;
    h2o_buffer_dispose(&ssl->input.encrypted);
    clear_output_buffer(ssl);
    free(ssl);
}

static void dispose_socket(h2o_socket_t *sock, const char *err)
{
    void (*close_cb)(void *data);
    void *close_cb_data;

    if (sock->ssl != NULL) {
        destroy_ssl(sock->ssl);
        sock->ssl = NULL;
    }
    h2o_buffer_dispose(&sock->input);
    if (sock->_peername != NULL) {
        free(sock->_peername);
        sock->_peername = NULL;
    }

    close_cb = sock->on_close.cb;
    close_cb_data = sock->on_close.data;

    do_dispose_socket(sock);

    if (close_cb != NULL)
        close_cb(close_cb_data);
}

static void shutdown_ssl(h2o_socket_t *sock, const char *err)
{
    int ret;

    if (err != NULL)
        goto Close;

    if (sock->_cb.write != NULL) {
        /* note: libuv calls the write callback after the socket is closed by uv_close (with status set to 0 if the write succeeded)
         */
        sock->_cb.write = NULL;
        goto Close;
    }

    if ((ret = SSL_shutdown(sock->ssl->ssl)) == -1) {
        goto Close;
    }

    if (sock->ssl->output.bufs.size != 0) {
        h2o_socket_read_stop(sock);
        flush_pending_ssl(sock, ret == 1 ? dispose_socket : shutdown_ssl);
    } else if (ret == 2 && SSL_get_error(sock->ssl->ssl, ret) == SSL_ERROR_WANT_READ) {
        h2o_socket_read_start(sock, shutdown_ssl);
    } else {
        goto Close;
    }

    return;
Close:
    dispose_socket(sock, err);
}

void h2o_socket_dispose_export(h2o_socket_export_t *info)
{
    assert(info->fd != -1);
    if (info->ssl != NULL) {
        destroy_ssl(info->ssl);
        info->ssl = NULL;
    }
    h2o_buffer_dispose(&info->input);
    close(info->fd);
    info->fd = -1;
}

int h2o_socket_export(h2o_socket_t *sock, h2o_socket_export_t *info)
{
    static h2o_buffer_prototype_t nonpooling_prototype = {};

    assert(!h2o_socket_is_writing(sock));

    if (do_export(sock, info) == -1)
        return -1;

    if ((info->ssl = sock->ssl) != NULL) {
        sock->ssl = NULL;
        h2o_buffer_set_prototype(&info->ssl->input.encrypted, &nonpooling_prototype);
    }
    info->input = sock->input;
    h2o_buffer_set_prototype(&info->input, &nonpooling_prototype);
    h2o_buffer_init(&sock->input, &h2o_socket_buffer_prototype);

    h2o_socket_close(sock);

    return 0;
}

h2o_socket_t *h2o_socket_import(h2o_loop_t *loop, h2o_socket_export_t *info)
{
    h2o_socket_t *sock;

    assert(info->fd != -1);

    sock = do_import(loop, info);
    info->fd = -1; /* just in case */
    if ((sock->ssl = info->ssl) != NULL) {
        setup_bio(sock);
        h2o_buffer_set_prototype(&sock->ssl->input.encrypted, &h2o_socket_buffer_prototype);
    }
    sock->input = info->input;
    h2o_buffer_set_prototype(&sock->input, &h2o_socket_buffer_prototype);
    return sock;
}

void h2o_socket_close(h2o_socket_t *sock)
{
    if (sock->ssl == NULL) {
        dispose_socket(sock, 0);
    } else {
        shutdown_ssl(sock, 0);
    }
}

#if defined(TCP_INFO) && defined(TCP_NOTSENT_LOWAT)
static int fetch_tcp_info(h2o_socket_t *sock, struct tcp_info *info)
{
    int fd = h2o_socket_get_fd(sock);
    socklen_t sz = sizeof(*info);
    return getsockopt(fd, IPPROTO_TCP, TCP_INFO, info, &sz);
}

size_t h2o_socket_do_prepare_for_latency_optimized_write(h2o_socket_t *sock, int minimum_rtt)
{
    struct tcp_info tcpi;

    switch (sock->_latency_optimization.mode) {

    case H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_TBD: {
        int16_t tls_overhead;
        if (fetch_tcp_info(sock, &tcpi) != 0)
            goto Disable;
        if (tcpi.tcpi_rtt < minimum_rtt)
            goto Disable;
        if (sock->ssl != NULL) {
            /* FIXME check the numbers! taken from http://d.hatena.ne.jp/jovi0608/20160404/1459748671 */
            const SSL_CIPHER *cipher = SSL_get_current_cipher(sock->ssl->ssl);
            switch (cipher->id) {
            case TLS1_CK_RSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_DHE_RSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_RSA_WITH_AES_256_GCM_SHA384:
            case TLS1_CK_DHE_RSA_WITH_AES_256_GCM_SHA384:
            case TLS1_CK_ECDHE_RSA_WITH_AES_256_GCM_SHA384:
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384:
                tls_overhead = 5 /* header */ + 8 /* IV */ + 12 /* tag */;
#if defined(TLS1_CK_DHE_RSA_CHACHA20_POLY1305)
            case TLS1_CK_DHE_RSA_CHACHA20_POLY1305:
            case TLS1_CK_ECDHE_RSA_CHACHA20_POLY1305:
            case TLS1_CK_ECDHE_ECDSA_CHACHA20_POLY1305:
                tls_overhead = 5 /* header */ + 16 /* tag */;
#endif
            default:
                goto Disable;
            }
        } else {
            tls_overhead = 0;
        }
        int notsent_lowat = 1; /* cannot be set to zero on Linux */
        if (setsockopt(h2o_socket_get_fd(sock), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &notsent_lowat, sizeof(notsent_lowat)) != 0)
            goto Disable;
        /* successfully set up.  Save the parameters */
        sock->_latency_optimization.tls_overhead = tls_overhead;
        sock->_latency_optimization.mss = tcpi.tcpi_snd_mss;
    } break;

    case H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_NEEDS_UPDATE:
        /* re-fetch TCP_INFO */
        if (fetch_tcp_info(sock, &tcpi) != 0)
            return SIZE_MAX;
        break;

    default:
        h2o_fatal("unexpected mode");
        break;
    }

    /* latency-optimization is enabled, and TCP_INFO is obtained */

    /* no need to:
     *   1) adjust the write size if single_write_size << cwnd_size
     *   2) align TLS record boundary to TCP packet boundary if packet loss-rate is low and BW isn't small (implied by cwnd size)
     */
    if (sock->_latency_optimization.mss * tcpi.tcpi_snd_cwnd >= 65536) {
        sock->_latency_optimization.mode = H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_USE_LARGE_TLS_RECORDS;
        return SIZE_MAX;
    }

    sock->_latency_optimization.mode = H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_USE_TINY_TLS_RECORDS;
    size_t packets_sendable = tcpi.tcpi_snd_cwnd > tcpi.tcpi_unacked ? tcpi.tcpi_snd_cwnd - tcpi.tcpi_unacked : 0;
    sock->_latency_optimization.suggested_write_size =
        (packets_sendable + 1) * (sock->_latency_optimization.mss - sock->_latency_optimization.tls_overhead);
    return sock->_latency_optimization.suggested_write_size;

Disable:
    sock->_latency_optimization.mode = H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_DISABLED;
    return SIZE_MAX;
}

#else

size_t h2o_socket_do_prepare_for_latency_optimized_write(h2o_socket_t *sock, int minimum_rtt)
{
    sock->_latency_optimization.mode = H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_DISABLED;
    return SIZE_MAX;
}

#endif

void h2o_socket_write(h2o_socket_t *sock, h2o_iovec_t *bufs, size_t bufcnt, h2o_socket_cb cb)
{
#if H2O_SOCKET_DUMP_WRITE
    {
        size_t i;
        for (i = 0; i != bufcnt; ++i) {
            fprintf(stderr, "writing %zu bytes to fd:%d\n", bufs[i].len,
#if H2O_USE_LIBUV
                    ((struct st_h2o_uv_socket_t *)sock)->uv.stream->io_watcher.fd
#else
                    ((struct st_h2o_evloop_socket_t *)sock)->fd
#endif
                    );
            h2o_dump_memory(stderr, bufs[i].base, bufs[i].len);
        }
    }
#endif
    if (sock->ssl == NULL) {
        do_write(sock, bufs, bufcnt, cb);
    } else {
        assert(sock->ssl->output.bufs.size == 0);
        /* fill in the data */
        size_t ssl_record_size;
        switch (sock->_latency_optimization.mode) {
        case H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_USE_TINY_TLS_RECORDS:
        case H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_NEEDS_UPDATE:
            ssl_record_size = sock->_latency_optimization.mss;
            sock->_latency_optimization.mode = H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_NEEDS_UPDATE;
            break;
        case H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_USE_LARGE_TLS_RECORDS:
            ssl_record_size = 16384 - sock->_latency_optimization.tls_overhead;
            sock->_latency_optimization.mode = H2O_SOCKET_LATENCY_OPTIMIZATION_MODE_NEEDS_UPDATE;
            break;
        default:
            ssl_record_size = 1400;
            break;
        }
        for (; bufcnt != 0; ++bufs, --bufcnt) {
            size_t off = 0;
            while (off != bufs[0].len) {
                int ret;
                size_t sz = bufs[0].len - off;
                if (sz > ssl_record_size)
                    sz = ssl_record_size;
                ret = SSL_write(sock->ssl->ssl, bufs[0].base + off, (int)sz);
                if (ret != sz) {
                    /* The error happens if SSL_write is called after SSL_read returns a fatal error (e.g. due to corrupt TCP packet
                     * being received). We need to take care of this since some protocol implementations send data after the read-
                     * side of the connection gets closed (note that protocol implementations are (yet) incapable of distinguishing
                     * a normal shutdown and close due to an error using the `status` value of the read callback).
                     */
                    clear_output_buffer(sock->ssl);
                    flush_pending_ssl(sock, cb);
#ifndef H2O_USE_LIBUV
                    ((struct st_h2o_evloop_socket_t *)sock)->_flags |= H2O_SOCKET_FLAG_IS_WRITE_ERROR;
#endif
                    return;
                }
                off += sz;
            }
        }
        flush_pending_ssl(sock, cb);
    }
}

void on_write_complete(h2o_socket_t *sock, const char *err)
{
    h2o_socket_cb cb;

    if (sock->ssl != NULL)
        clear_output_buffer(sock->ssl);

    cb = sock->_cb.write;
    sock->_cb.write = NULL;
    cb(sock, err);
}

void h2o_socket_read_start(h2o_socket_t *sock, h2o_socket_cb cb)
{
    sock->_cb.read = cb;
    do_read_start(sock);
}

void h2o_socket_read_stop(h2o_socket_t *sock)
{
    sock->_cb.read = NULL;
    do_read_stop(sock);
}

void h2o_socket_setpeername(h2o_socket_t *sock, struct sockaddr *sa, socklen_t len)
{
    if (sock->_peername != NULL)
        free(sock->_peername);
    sock->_peername = h2o_mem_alloc(offsetof(struct st_h2o_socket_peername_t, addr) + len);
    sock->_peername->len = len;
    memcpy(&sock->_peername->addr, sa, len);
}

socklen_t h2o_socket_getpeername(h2o_socket_t *sock, struct sockaddr *sa)
{
    /* return cached, if exists */
    if (sock->_peername != NULL) {
        memcpy(sa, &sock->_peername->addr, sock->_peername->len);
        return sock->_peername->len;
    }
    /* call, copy to cache, and return */
    socklen_t len = get_peername_uncached(sock, sa);
    h2o_socket_setpeername(sock, sa, len);
    return len;
}

const char *h2o_socket_get_ssl_protocol_version(h2o_socket_t *sock)
{
    return sock->ssl != NULL ? SSL_get_version(sock->ssl->ssl) : NULL;
}

int h2o_socket_get_ssl_session_reused(h2o_socket_t *sock)
{
    return sock->ssl != NULL ? (int)SSL_session_reused(sock->ssl->ssl) : -1;
}

const char *h2o_socket_get_ssl_cipher(h2o_socket_t *sock)
{
    return sock->ssl != NULL ? SSL_get_cipher_name(sock->ssl->ssl) : NULL;
}

int h2o_socket_get_ssl_cipher_bits(h2o_socket_t *sock)
{
    return sock->ssl != NULL ? SSL_get_cipher_bits(sock->ssl->ssl, NULL) : 0;
}

h2o_iovec_t h2o_socket_log_ssl_cipher_bits(h2o_socket_t *sock, h2o_mem_pool_t *pool)
{
    int bits = h2o_socket_get_ssl_cipher_bits(sock);
    if (bits != 0) {
        char *s = (char *)(pool != NULL ? h2o_mem_alloc_pool(pool, sizeof(H2O_INT16_LONGEST_STR))
                                        : h2o_mem_alloc(sizeof(H2O_INT16_LONGEST_STR)));
        size_t len = sprintf(s, "%" PRId16, (int16_t)bits);
        return h2o_iovec_init(s, len);
    } else {
        return h2o_iovec_init(H2O_STRLIT("-"));
    }
}

int h2o_socket_compare_address(struct sockaddr *x, struct sockaddr *y)
{
#define CMP(a, b)                                                                                                                  \
    if (a != b)                                                                                                                    \
    return a < b ? -1 : 1

    CMP(x->sa_family, y->sa_family);

    if (x->sa_family == AF_UNIX) {
        struct sockaddr_un *xun = (void *)x, *yun = (void *)y;
        int r = strcmp(xun->sun_path, yun->sun_path);
        if (r != 0)
            return r;
    } else if (x->sa_family == AF_INET) {
        struct sockaddr_in *xin = (void *)x, *yin = (void *)y;
        CMP(ntohl(xin->sin_addr.s_addr), ntohl(yin->sin_addr.s_addr));
        CMP(ntohs(xin->sin_port), ntohs(yin->sin_port));
    } else if (x->sa_family == AF_INET6) {
        struct sockaddr_in6 *xin6 = (void *)x, *yin6 = (void *)y;
        int r = memcmp(xin6->sin6_addr.s6_addr, yin6->sin6_addr.s6_addr, sizeof(xin6->sin6_addr.s6_addr));
        if (r != 0)
            return r;
        CMP(ntohs(xin6->sin6_port), ntohs(yin6->sin6_port));
        CMP(xin6->sin6_flowinfo, yin6->sin6_flowinfo);
        CMP(xin6->sin6_scope_id, yin6->sin6_scope_id);
    } else {
        assert(!"unknown sa_family");
    }

#undef CMP
    return 0;
}

size_t h2o_socket_getnumerichost(struct sockaddr *sa, socklen_t salen, char *buf)
{
    if (sa->sa_family == AF_INET) {
        /* fast path for IPv4 addresses */
        struct sockaddr_in *sin = (void *)sa;
        uint32_t addr;
        addr = htonl(sin->sin_addr.s_addr);
        return sprintf(buf, "%d.%d.%d.%d", addr >> 24, (addr >> 16) & 255, (addr >> 8) & 255, addr & 255);
    }

    if (getnameinfo(sa, salen, buf, NI_MAXHOST, NULL, 0, NI_NUMERICHOST) != 0)
        return SIZE_MAX;
    return strlen(buf);
}

int32_t h2o_socket_getport(struct sockaddr *sa)
{
    switch (sa->sa_family) {
    case AF_INET:
        return htons(((struct sockaddr_in *)sa)->sin_port);
    case AF_INET6:
        return htons(((struct sockaddr_in6 *)sa)->sin6_port);
    default:
        return -1;
    }
}

static void create_ssl(h2o_socket_t *sock, SSL_CTX *ssl_ctx)
{
    sock->ssl->ssl = SSL_new(ssl_ctx);
    setup_bio(sock);
}

static SSL_SESSION *on_async_resumption_get(SSL *ssl, unsigned char *data, int len, int *copy)
{
    h2o_socket_t *sock = SSL_get_rbio(ssl)->ptr;

    switch (sock->ssl->handshake.server.async_resumption.state) {
    case ASYNC_RESUMPTION_STATE_RECORD:
        sock->ssl->handshake.server.async_resumption.state = ASYNC_RESUMPTION_STATE_REQUEST_SENT;
        resumption_get_async(sock, h2o_iovec_init(data, len));
        return NULL;
    case ASYNC_RESUMPTION_STATE_COMPLETE:
        *copy = 1;
        return sock->ssl->handshake.server.async_resumption.session_data;
    default:
        assert(!"FIXME");
        return NULL;
    }
}

static int on_async_resumption_new(SSL *ssl, SSL_SESSION *session)
{
    h2o_iovec_t data;
    const unsigned char *id;
    unsigned id_len;
    unsigned char *p;

    /* build data */
    data.len = i2d_SSL_SESSION(session, NULL);
    data.base = alloca(data.len);
    p = (void *)data.base;
    i2d_SSL_SESSION(session, &p);

    id = SSL_SESSION_get_id(session, &id_len);
    resumption_new(h2o_iovec_init(id, id_len), data);
    return 0;
}

static void on_async_resumption_remove(SSL_CTX *ssl_ctx, SSL_SESSION *session)
{
    h2o_iovec_t session_id = h2o_iovec_init(session->session_id, session->session_id_length);
    resumption_remove(session_id);
}

static void on_handshake_complete(h2o_socket_t *sock, const char *err)
{
    h2o_socket_cb handshake_cb = sock->ssl->handshake.cb;
    sock->_cb.write = NULL;
    sock->ssl->handshake.cb = NULL;
    decode_ssl_input(sock);
    handshake_cb(sock, err);
}

static void proceed_handshake(h2o_socket_t *sock, const char *err)
{
    h2o_iovec_t first_input = {};
    int ret;

    sock->_cb.write = NULL;

    if (err != NULL) {
        goto Complete;
    }

    if (sock->ssl->handshake.server.async_resumption.state == ASYNC_RESUMPTION_STATE_RECORD) {
        if (sock->ssl->input.encrypted->size <= 1024) {
            /* retain a copy of input if performing async resumption */
            first_input = h2o_iovec_init(alloca(sock->ssl->input.encrypted->size), sock->ssl->input.encrypted->size);
            memcpy(first_input.base, sock->ssl->input.encrypted->bytes, first_input.len);
        } else {
            sock->ssl->handshake.server.async_resumption.state = ASYNC_RESUMPTION_STATE_COMPLETE;
        }
    }

Redo:
    if (sock->ssl->ssl->server) {
        ret = SSL_accept(sock->ssl->ssl);
    } else {
        ret = SSL_connect(sock->ssl->ssl);
    }

    switch (sock->ssl->handshake.server.async_resumption.state) {
    case ASYNC_RESUMPTION_STATE_RECORD:
        /* async resumption has not been triggered; proceed the state to complete */
        sock->ssl->handshake.server.async_resumption.state = ASYNC_RESUMPTION_STATE_COMPLETE;
        break;
    case ASYNC_RESUMPTION_STATE_REQUEST_SENT: {
        /* sent async request, reset the ssl state, and wait for async response */
        assert(ret < 0);
        SSL_CTX *ssl_ctx = SSL_get_SSL_CTX(sock->ssl->ssl);
        SSL_free(sock->ssl->ssl);
        create_ssl(sock, ssl_ctx);
        clear_output_buffer(sock->ssl);
        h2o_buffer_consume(&sock->ssl->input.encrypted, sock->ssl->input.encrypted->size);
        h2o_buffer_reserve(&sock->ssl->input.encrypted, first_input.len);
        memcpy(sock->ssl->input.encrypted->bytes, first_input.base, first_input.len);
        sock->ssl->input.encrypted->size = first_input.len;
        h2o_socket_read_stop(sock);
        return;
    }
    default:
        break;
    }

    if (ret == 0 || (ret < 0 && SSL_get_error(sock->ssl->ssl, ret) != SSL_ERROR_WANT_READ)) {
        /* failed */
        long verify_result = SSL_get_verify_result(sock->ssl->ssl);
        if (verify_result != X509_V_OK) {
            err = X509_verify_cert_error_string(verify_result);
        } else {
            err = "ssl handshake failure";
        }
        goto Complete;
    }

    if (sock->ssl->output.bufs.size != 0) {
        h2o_socket_read_stop(sock);
        flush_pending_ssl(sock, ret == 1 ? on_handshake_complete : proceed_handshake);
    } else {
        if (ret == 1) {
            if (!sock->ssl->ssl->server) {
                X509 *cert = SSL_get_peer_certificate(sock->ssl->ssl);
                if (cert != NULL) {
                    switch (validate_hostname(sock->ssl->handshake.client.server_name, cert)) {
                    case MatchFound:
                        /* ok */
                        break;
                    case MatchNotFound:
                        err = h2o_socket_error_ssl_cert_name_mismatch;
                        break;
                    default:
                        err = h2o_socket_error_ssl_cert_invalid;
                        break;
                    }
                    X509_free(cert);
                } else {
                    err = h2o_socket_error_ssl_no_cert;
                }
            }
            goto Complete;
        }
        if (sock->ssl->input.encrypted->size != 0)
            goto Redo;
        h2o_socket_read_start(sock, proceed_handshake);
    }
    return;

Complete:
    h2o_socket_read_stop(sock);
    on_handshake_complete(sock, err);
}

void h2o_socket_ssl_handshake(h2o_socket_t *sock, SSL_CTX *ssl_ctx, const char *server_name, h2o_socket_cb handshake_cb)
{
    sock->ssl = h2o_mem_alloc(sizeof(*sock->ssl));
    memset(sock->ssl, 0, offsetof(struct st_h2o_socket_ssl_t, output.pool));

    /* setup the buffers; sock->input should be empty, sock->ssl->input.encrypted should contain the initial input, if any */
    h2o_buffer_init(&sock->ssl->input.encrypted, &h2o_socket_buffer_prototype);
    if (sock->input->size != 0) {
        h2o_buffer_t *tmp = sock->input;
        sock->input = sock->ssl->input.encrypted;
        sock->ssl->input.encrypted = tmp;
    }

    h2o_mem_init_pool(&sock->ssl->output.pool);
    create_ssl(sock, ssl_ctx);

    sock->ssl->handshake.cb = handshake_cb;
    if (server_name == NULL) {
        /* is server */
        if (SSL_CTX_sess_get_get_cb(ssl_ctx) != NULL)
            sock->ssl->handshake.server.async_resumption.state = ASYNC_RESUMPTION_STATE_RECORD;
        if (sock->ssl->input.encrypted->size != 0)
            proceed_handshake(sock, 0);
        else
            h2o_socket_read_start(sock, proceed_handshake);
    } else {
        sock->ssl->handshake.client.server_name = h2o_strdup(NULL, server_name, SIZE_MAX).base;
        SSL_set_tlsext_host_name(sock->ssl->ssl, sock->ssl->handshake.client.server_name);
        proceed_handshake(sock, 0);
    }
}

void h2o_socket_ssl_resume_server_handshake(h2o_socket_t *sock, h2o_iovec_t session_data)
{
    if (session_data.len != 0) {
        const unsigned char *p = (void *)session_data.base;
        sock->ssl->handshake.server.async_resumption.session_data = d2i_SSL_SESSION(NULL, &p, (long)session_data.len);
        /* FIXME warn on failure */
    }

    sock->ssl->handshake.server.async_resumption.state = ASYNC_RESUMPTION_STATE_COMPLETE;
    proceed_handshake(sock, 0);

    if (sock->ssl->handshake.server.async_resumption.session_data != NULL) {
        SSL_SESSION_free(sock->ssl->handshake.server.async_resumption.session_data);
        sock->ssl->handshake.server.async_resumption.session_data = NULL;
    }
}

void h2o_socket_ssl_async_resumption_init(h2o_socket_ssl_resumption_get_async_cb get_async_cb,
                                          h2o_socket_ssl_resumption_new_cb new_cb, h2o_socket_ssl_resumption_remove_cb remove_cb)
{
    resumption_get_async = get_async_cb;
    resumption_new = new_cb;
    resumption_remove = remove_cb;
}

void h2o_socket_ssl_async_resumption_setup_ctx(SSL_CTX *ctx)
{
    SSL_CTX_sess_set_get_cb(ctx, on_async_resumption_get);
    SSL_CTX_sess_set_new_cb(ctx, on_async_resumption_new);
    SSL_CTX_sess_set_remove_cb(ctx, on_async_resumption_remove);
    /* if necessary, it is the responsibility of the caller to disable the internal cache */
}

h2o_iovec_t h2o_socket_ssl_get_selected_protocol(h2o_socket_t *sock)
{
    const unsigned char *data = NULL;
    unsigned len = 0;

    assert(sock->ssl != NULL);

#if H2O_USE_ALPN
    if (len == 0)
        SSL_get0_alpn_selected(sock->ssl->ssl, &data, &len);
#endif
#if H2O_USE_NPN
    if (len == 0)
        SSL_get0_next_proto_negotiated(sock->ssl->ssl, &data, &len);
#endif

    return h2o_iovec_init(data, len);
}

static int on_alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *_in, unsigned int inlen,
                          void *_protocols)
{
    const h2o_iovec_t *protocols = _protocols;
    size_t i;

    for (i = 0; protocols[i].len != 0; ++i) {
        const unsigned char *in = _in, *in_end = in + inlen;
        while (in != in_end) {
            size_t cand_len = *in++;
            if (in_end - in < cand_len) {
                /* broken request */
                return SSL_TLSEXT_ERR_NOACK;
            }
            if (cand_len == protocols[i].len && memcmp(in, protocols[i].base, cand_len) == 0) {
                goto Found;
            }
            in += cand_len;
        }
    }
    /* not found */
    return SSL_TLSEXT_ERR_NOACK;

Found:
    *out = (const unsigned char *)protocols[i].base;
    *outlen = (unsigned char)protocols[i].len;
    return SSL_TLSEXT_ERR_OK;
}

#if H2O_USE_ALPN

void h2o_ssl_register_alpn_protocols(SSL_CTX *ctx, const h2o_iovec_t *protocols)
{
    SSL_CTX_set_alpn_select_cb(ctx, on_alpn_select, (void *)protocols);
}

#endif

#if H2O_USE_NPN

static int on_npn_advertise(SSL *ssl, const unsigned char **out, unsigned *outlen, void *protocols)
{
    *out = protocols;
    *outlen = (unsigned)strlen(protocols);
    return SSL_TLSEXT_ERR_OK;
}

void h2o_ssl_register_npn_protocols(SSL_CTX *ctx, const char *protocols)
{
    SSL_CTX_set_next_protos_advertised_cb(ctx, on_npn_advertise, (void *)protocols);
}

#endif
