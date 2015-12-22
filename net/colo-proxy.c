/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO., LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * Author: Zhang Chen <zhangchen.fnst@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "net/filter.h"
#include "net/queue.h"
#include "qemu-common.h"
#include "qemu/iov.h"
#include "qapi/qmp/qerror.h"
#include "qapi-visit.h"
#include "qom/object.h"
#include "qemu/sockets.h"
#include "qemu/main-loop.h"
#include "qemu/jhash.h"
#include "qemu/coroutine.h"
#include "net/eth.h"
#include "slirp/slirp.h"
#include "slirp/slirp_config.h"
#include "slirp/ip.h"
#include "net/net.h"
#include "qemu/error-report.h"
#include "net/colo-proxy.h"
#include "trace.h"
#include <sys/sysinfo.h>

#define FILTER_COLO_PROXY(obj) \
    OBJECT_CHECK(COLOProxyState, (obj), TYPE_FILTER_COLO_PROXY)

#define TYPE_FILTER_COLO_PROXY "colo-proxy"
#define PRIMARY_MODE "primary"
#define SECONDARY_MODE "secondary"

/*

  |COLOProxyState++
  |               |
  +---------------+   +---------------+         +---------------+
  |conn list      +--->conn           +--------->conn           |
  +---------------+   +---------------+         +---------------+
  |               |     |           |             |           |
  +---------------+ +---v----+  +---v----+    +---v----+  +---v----+
                    |primary |  |secondary    |primary |  |secondary
                    |packet  |  |packet  +    |packet  |  |packet  +
                    +--------+  +--------+    +--------+  +--------+
                        |           |             |           |
                    +---v----+  +---v----+    +---v----+  +---v----+
                    |primary |  |secondary    |primary |  |secondary
                    |packet  |  |packet  +    |packet  |  |packet  +
                    +--------+  +--------+    +--------+  +--------+
                        |           |             |           |
                    +---v----+  +---v----+    +---v----+  +---v----+
                    |primary |  |secondary    |primary |  |secondary
                    |packet  |  |packet  +    |packet  |  |packet  +
                    +--------+  +--------+    +--------+  +--------+


*/

typedef struct COLOProxyState {
    NetFilterState parent_obj;
    NetQueue *incoming_queue;/* guest normal net queue */
    NetFilterDirection direction; /* packet direction */
    /* colo mode (primary or secondary) */
    int colo_mode;
    /* primary colo connect address(192.168.0.100:12345)
     * or secondary listening address(:12345)
     */
    char *addr;
    int sockfd;

     /* connection list: the packet belonged to this NIC
     * could be found in this list.
     * element type: Connection
     */
    GQueue conn_list;
    int status; /* proxy is running or not */
    ssize_t hashtable_size; /* proxy current hash size */
    QemuEvent need_compare_ev;  /* notify compare thread */
    QemuThread thread; /* compare thread, a thread for each NIC */

} COLOProxyState;

typedef struct Packet {
    void *data;
    union {
        uint8_t *network_layer;
        struct ip *ip;
    };
    uint8_t *transport_layer;
    int size;
    COLOProxyState *s;
    NetClientState *sender;
} Packet;

typedef struct ConnectionKey {
    /* (src, dst) must be grouped, in the same way than in IP header */
    struct in_addr src;
    struct in_addr dst;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t ip_proto;
} QEMU_PACKED ConnectionKey;

/* define one connection */
typedef struct Connection {
    /* connection primary send queue: element type: Packet */
    GQueue primary_list;
    /* connection secondary send queue: element type: Packet */
    GQueue secondary_list;
     /* flag to enqueue unprocessed_connections */
    bool processing;
    int ip_proto;

    void *proto; /* tcp only now */
} Connection;

enum {
    COLO_PROXY_NONE,     /* colo proxy is not started */
    COLO_PROXY_RUNNING,  /* colo proxy is running */
    COLO_PROXY_DONE,     /* colo proxyis done(failover) */
};

/* save all the connections of a vm instance in this table */
GHashTable *colo_conn_hash;
static bool colo_do_checkpoint;
static ssize_t hashtable_max_size;

static inline void colo_proxy_dump_packet(Packet *pkt)
{
    int i;
    for (i = 0; i < pkt->size; i++) {
        printf("%02x ", ((uint8_t *)pkt->data)[i]);
    }
    printf("\n");
}

static uint32_t connection_key_hash(const void *opaque)
{
    const ConnectionKey *key = opaque;
    uint32_t a, b, c;

    /* Jenkins hash */
    a = b = c = JHASH_INITVAL + sizeof(*key);
    a += key->src.s_addr;
    b += key->dst.s_addr;
    c += (key->src_port | key->dst_port << 16);
    __jhash_mix(a, b, c);

    a += key->ip_proto;
    __jhash_final(a, b, c);

    return c;
}

static int connection_key_equal(const void *opaque1, const void *opaque2)
{
    return memcmp(opaque1, opaque2, sizeof(ConnectionKey)) == 0;
}

bool colo_proxy_query_checkpoint(void)
{
    return colo_do_checkpoint;
}

/*
 * send a packet to peer
 * >=0: success
 * <0: fail
 */
static ssize_t colo_proxy_sock_send(NetFilterState *nf,
                                         const struct iovec *iov,
                                         int iovcnt)
{
    COLOProxyState *s = FILTER_COLO_PROXY(nf);
    ssize_t ret = 0;
    ssize_t size = 0;
    struct iovec sizeiov = {
        .iov_base = &size,
        .iov_len = sizeof(size)
    };
    size = iov_size(iov, iovcnt);
    if (!size) {
        return 0;
    }

    ret = iov_send(s->sockfd, &sizeiov, 1, 0, sizeof(size));
    if (ret < 0) {
        return ret;
    }
    ret = iov_send(s->sockfd, iov, iovcnt, 0, size);
    return ret;
}

/*
 * receive a packet from peer
 * in primary: enqueue packet to secondary_list
 * in secondary: pass packet to next
 */
static void colo_proxy_sock_receive(void *opaque)
{
    NetFilterState *nf = opaque;
    COLOProxyState *s = FILTER_COLO_PROXY(nf);
    ssize_t len = 0;
    struct iovec sizeiov = {
        .iov_base = &len,
        .iov_len = sizeof(len)
    };

    iov_recv(s->sockfd, &sizeiov, 1, 0, sizeof(len));
    if (len > 0 && len < NET_BUFSIZE) {
        char *buf = g_malloc0(len);
        struct iovec iov = {
            .iov_base = buf,
            .iov_len = len
        };

        iov_recv(s->sockfd, &iov, 1, 0, len);
        if (s->colo_mode == COLO_MODE_PRIMARY) {
            colo_proxy_enqueue_secondary_packet(nf, buf, len);
            /* buf will be release when pakcet destroy */
        } else {
            qemu_net_queue_send(s->incoming_queue, nf->netdev,
                            0, (const uint8_t *)buf, len, NULL);
        }
    }
}

static ssize_t colo_proxy_receive_iov(NetFilterState *nf,
                                         NetClientState *sender,
                                         unsigned flags,
                                         const struct iovec *iov,
                                         int iovcnt,
                                         NetPacketSent *sent_cb)
{
    /*
     * We return size when buffer a packet, the sender will take it as
     * a already sent packet, so sent_cb should not be called later.
     *
     */
    COLOProxyState *s = FILTER_COLO_PROXY(nf);
    ssize_t ret = 0;

    if (s->status != COLO_PROXY_RUNNING) {
        /* proxy is not started or failovered */
        return 0;
    }

    if (s->colo_mode == COLO_MODE_PRIMARY) {
        /* colo_proxy_primary_handler */
    } else {
        /* colo_proxy_secondary_handler */
    }
    return iov_size(iov, iovcnt);
}

static void colo_proxy_cleanup(NetFilterState *nf)
{
    COLOProxyState *s = FILTER_COLO_PROXY(nf);
    close(s->sockfd);
    s->sockfd = -1;
    qemu_event_destroy(&s->need_compare_ev);
}

/* wait for peer connecting
 * NOTE: this function will block the caller
 * 0 on success, otherwise returns -1
 */
static int colo_wait_incoming(COLOProxyState *s)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int accept_sock, err;
    int fd = inet_listen(s->addr, NULL, 256, SOCK_STREAM, 0, NULL);

    if (fd < 0) {
        error_report("colo proxy listen failed");
        return -1;
    }

    do {
        accept_sock = qemu_accept(fd, (struct sockaddr *)&addr, &addrlen);
        err = socket_error();
    } while (accept_sock < 0 && err == EINTR);
    closesocket(fd);

    if (accept_sock < 0) {
        error_report("colo proxy accept failed(%s)", strerror(err));
        return -1;
    }
    s->sockfd = accept_sock;

    qemu_set_fd_handler(s->sockfd, colo_proxy_sock_receive, NULL, (void *)s);

    return 0;
}

/* try to connect listening server
 * 0 on success, otherwise something wrong
 */
static ssize_t colo_proxy_connect(COLOProxyState *s)
{
    int sock;
    sock = inet_connect(s->addr, NULL);

    if (sock < 0) {
        error_report("colo proxy inet_connect failed");
        return -1;
    }
    s->sockfd = sock;
    qemu_set_fd_handler(s->sockfd, colo_proxy_sock_receive, NULL, (void *)s);

    return 0;
}

static void colo_proxy_notify_checkpoint(void)
{
    trace_colo_proxy("colo_proxy_notify_checkpoint");
    colo_do_checkpoint = true;
}

static void colo_proxy_start_one(NetFilterState *nf,
                                      void *opaque, Error **errp)
{
    COLOProxyState *s;
    int mode, ret;

    if (strcmp(object_get_typename(OBJECT(nf)), TYPE_FILTER_COLO_PROXY)) {
        return;
    }

    mode = *(int *)opaque;
    s = FILTER_COLO_PROXY(nf);
    assert(s->colo_mode == mode);

    if (s->colo_mode == COLO_MODE_PRIMARY) {
        char thread_name[1024];

        ret = colo_proxy_connect(s);
        if (ret) {
            error_setg(errp, "colo proxy connect failed");
            return ;
        }

        s->status = COLO_PROXY_RUNNING;
        sprintf(thread_name, "proxy compare %s", nf->netdev_id);
        qemu_thread_create(&s->thread, thread_name,
                                colo_proxy_compare_thread, s,
                                QEMU_THREAD_JOINABLE);
    } else {
        ret = colo_wait_incoming(s);
        if (ret) {
            error_setg(errp, "colo proxy wait incoming failed");
            return ;
        }
        s->status = COLO_PROXY_RUNNING;
    }
}

int colo_proxy_start(int mode)
{
    Error *err = NULL;
    qemu_foreach_netfilter(colo_proxy_start_one, &mode, &err);
    if (err) {
        return -1;
    }
    return 0;
}

static void colo_proxy_stop_one(NetFilterState *nf,
                                      void *opaque, Error **errp)
{
    COLOProxyState *s;
    int mode;

    if (strcmp(object_get_typename(OBJECT(nf)), TYPE_FILTER_COLO_PROXY)) {
        return;
    }

    s = FILTER_COLO_PROXY(nf);
    mode = *(int *)opaque;
    assert(s->colo_mode == mode);

    s->status = COLO_PROXY_DONE;
    if (s->sockfd >= 0) {
        qemu_set_fd_handler(s->sockfd, NULL, NULL, NULL);
        closesocket(s->sockfd);
    }
    if (s->colo_mode == COLO_MODE_PRIMARY) {
        colo_proxy_primary_checkpoint(s);
        qemu_event_set(&s->need_compare_ev);
        qemu_thread_join(&s->thread);
    } else {
        colo_proxy_secondary_checkpoint(s);
    }
}

void colo_proxy_stop(int mode)
{
    Error *err = NULL;
    qemu_foreach_netfilter(colo_proxy_stop_one, &mode, &err);
}

static void colo_proxy_setup(NetFilterState *nf, Error **errp)
{
    COLOProxyState *s = FILTER_COLO_PROXY(nf);

    if (!s->addr) {
        error_setg(errp, "filter colo_proxy needs 'addr' property set!");
        return;
    }

    if (nf->direction != NET_FILTER_DIRECTION_ALL) {
        error_setg(errp, "colo need queue all packet,"
                        "please startup colo-proxy with queue=all\n");
        return;
    }

    s->sockfd = -1;
    s->hashtable_size = 0;
    colo_do_checkpoint = false;
    qemu_event_init(&s->need_compare_ev, false);

    s->incoming_queue = qemu_new_net_queue(qemu_netfilter_pass_to_next, nf);
    colo_conn_hash = g_hash_table_new_full(connection_key_hash,
                                           connection_key_equal,
                                           g_free,
                                           connection_destroy);
    g_queue_init(&s->conn_list);
}

static void colo_proxy_class_init(ObjectClass *oc, void *data)
{
    NetFilterClass *nfc = NETFILTER_CLASS(oc);

    nfc->setup = colo_proxy_setup;
    nfc->cleanup = colo_proxy_cleanup;
    nfc->receive_iov = colo_proxy_receive_iov;
}

static int colo_proxy_get_mode(Object *obj, Error **errp)
{
    COLOProxyState *s = FILTER_COLO_PROXY(obj);

    return s->colo_mode;
}

static void
colo_proxy_set_mode(Object *obj, int mode, Error **errp)
{
    COLOProxyState *s = FILTER_COLO_PROXY(obj);

    s->colo_mode = mode;
}

static char *colo_proxy_get_addr(Object *obj, Error **errp)
{
    COLOProxyState *s = FILTER_COLO_PROXY(obj);

    return g_strdup(s->addr);
}

static void
colo_proxy_set_addr(Object *obj, const char *value, Error **errp)
{
    COLOProxyState *s = FILTER_COLO_PROXY(obj);
    g_free(s->addr);
    s->addr = g_strdup(value);
    if (!s->addr) {
        error_setg(errp, "colo_proxy needs 'addr'"
                     "property set!");
        return;
    }
}

static void colo_proxy_init(Object *obj)
{
    object_property_add_enum(obj, "mode", "COLOMode", COLOMode_lookup,
                             colo_proxy_get_mode, colo_proxy_set_mode, NULL);
    object_property_add_str(obj, "addr", colo_proxy_get_addr,
                            colo_proxy_set_addr, NULL);
}

static void colo_proxy_fini(Object *obj)
{
    COLOProxyState *s = FILTER_COLO_PROXY(obj);
    g_free(s->addr);
}

static const TypeInfo colo_proxy_info = {
    .name = TYPE_FILTER_COLO_PROXY,
    .parent = TYPE_NETFILTER,
    .class_init = colo_proxy_class_init,
    .instance_init = colo_proxy_init,
    .instance_finalize = colo_proxy_fini,
    .instance_size = sizeof(COLOProxyState),
};

static void register_types(void)
{
    type_register_static(&colo_proxy_info);
}

type_init(register_types);
