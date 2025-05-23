/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 38    Network Measurement Database */

/*
 * XXX XXX XXX
 *
 * This code may be slightly broken now. If you're getting consistent
 * (sometimes working) corrupt data exchanges, please contact adrian
 * (adrian@squid-cache.org) to sort them out.
 */

#include "squid.h"
#include "CachePeer.h"
#include "CachePeers.h"
#include "cbdata.h"
#include "event.h"
#include "fde.h"
#include "fs_io.h"
#include "FwdState.h"
#include "HttpReply.h"
#include "icmp/net_db.h"
#include "internal.h"
#include "ip/Address.h"
#include "log/File.h"
#include "MemObject.h"
#include "mgr/Registration.h"
#include "mime_header.h"
#include "neighbors.h"
#include "PeerSelectState.h"
#include "sbuf/SBuf.h"
#include "SquidConfig.h"
#include "Store.h"
#include "StoreClient.h"
#include "tools.h"
#include "wordlist.h"

#if HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#if USE_ICMP
#include "icmp/IcmpSquid.h"
#include "ipcache.h"
#include "StoreClient.h"

typedef enum {
    STATE_NONE,
    STATE_HEADER,
    STATE_BODY
} netdb_conn_state_t;

class netdbExchangeState
{
    CBDATA_CLASS(netdbExchangeState);

public:
    netdbExchangeState(CachePeer *aPeer, const HttpRequestPointer &theReq) :
        p(aPeer),
        r(theReq)
    {
        assert(r);
        // TODO: check if we actually need to do this. should be implicit
        r->http_ver = Http::ProtocolVersion();
    }

    ~netdbExchangeState() {
        debugs(38, 3, e->url());
        storeUnregister(sc, e, this);
        e->unlock("netdbExchangeDone");
    }

    CbcPointer<CachePeer> p;
    StoreEntry *e = nullptr;
    store_client *sc = nullptr;
    HttpRequestPointer r;

    /// for receiving a NetDB reply body from Store and interpreting it
    Store::ParsingBuffer parsingBuffer;

    netdb_conn_state_t connstate = STATE_HEADER;
};

CBDATA_CLASS_INIT(netdbExchangeState);

static hash_table *addr_table = nullptr;
static hash_table *host_table = nullptr;

static Ip::Address networkFromInaddr(const Ip::Address &a);
static void netdbRelease(netdbEntry * n);

static void netdbHashInsert(netdbEntry * n, Ip::Address &addr);
static void netdbHashDelete(const char *key);
static void netdbHostInsert(netdbEntry * n, const char *hostname);
static void netdbHostDelete(const net_db_name * x);
static void netdbPurgeLRU(void);
static netdbEntry *netdbLookupHost(const char *key);
static net_db_peer *netdbPeerByName(const netdbEntry * n, const char *);
static net_db_peer *netdbPeerAdd(netdbEntry * n, CachePeer * e);
static const char *netdbPeerName(const char *name);
static IPH netdbSendPing;
static STCB netdbExchangeHandleReply;

/* We have to keep a local list of CachePeer names.  The Peers structure
 * gets freed during a reconfigure.  We want this database to
 * remain persisitent, so _net_db_peer->peername points into this
 * linked list */
static wordlist *peer_names = nullptr;

static void
netdbHashInsert(netdbEntry * n, Ip::Address &addr)
{
    networkFromInaddr(addr).toStr(n->network, MAX_IPSTRLEN);
    n->key = n->network;
    assert(hash_lookup(addr_table, n->network) == nullptr);
    hash_join(addr_table, n);
}

static void
netdbHashDelete(const char *key)
{
    hash_link *hptr = (hash_link *)hash_lookup(addr_table, key);

    if (hptr == nullptr) {
        debug_trap("netdbHashDelete: key not found");
        return;
    }

    hash_remove_link(addr_table, hptr);
}

net_db_name::net_db_name(const char *hostname, netdbEntry *e) :
    next(e ? e->hosts : nullptr),
    net_db_entry(e)
{
    key = xstrdup(hostname);
    if (e) {
        e->hosts = this;
        ++ e->link_count;
    }
}

static void
netdbHostInsert(netdbEntry * n, const char *hostname)
{
    net_db_name *x = new net_db_name(hostname, n);
    assert(hash_lookup(host_table, hostname) == nullptr);
    hash_join(host_table, x);
}

static void
netdbHostDelete(const net_db_name * x)
{
    assert(x != nullptr);
    assert(x->net_db_entry != nullptr);

    netdbEntry *n = x->net_db_entry;
    -- n->link_count;

    for (auto **X = &n->hosts; *X; X = &(*X)->next) {
        if (*X == x) {
            *X = x->next;
            break;
        }
    }

    hash_remove_link(host_table, (hash_link *) x);
    delete x;
}

static netdbEntry *
netdbLookupHost(const char *key)
{
    net_db_name *x = (net_db_name *) hash_lookup(host_table, key);
    return x ? x->net_db_entry : nullptr;
}

static void
netdbRelease(netdbEntry * n)
{
    net_db_name *x;
    net_db_name *next;

    for (x = n->hosts; x; x = next) {
        next = x->next;
        netdbHostDelete(x);
    }

    n->hosts = nullptr;
    safe_free(n->peers);
    n->peers = nullptr;
    n->n_peers = 0;
    n->n_peers_alloc = 0;

    if (n->link_count == 0) {
        netdbHashDelete(n->network);
        delete n;
    }
}

static int
netdbLRU(const void *A, const void *B)
{
    const netdbEntry *const *n1 = (const netdbEntry *const *)A;
    const netdbEntry *const *n2 = (const netdbEntry *const *)B;

    if ((*n1)->last_use_time > (*n2)->last_use_time)
        return (1);

    if ((*n1)->last_use_time < (*n2)->last_use_time)
        return (-1);

    return (0);
}

static void
netdbPurgeLRU(void)
{
    netdbEntry *n;
    netdbEntry **list;
    int k = 0;
    int list_count = 0;
    list = (netdbEntry **)xcalloc(netdbEntry::UseCount(), sizeof(netdbEntry *));
    hash_first(addr_table);

    while ((n = (netdbEntry *) hash_next(addr_table))) {
        assert(list_count < netdbEntry::UseCount());
        *(list + list_count) = n;
        ++list_count;
    }

    qsort((char *) list,
          list_count,
          sizeof(netdbEntry *),
          netdbLRU);

    for (k = 0; k < list_count; ++k) {
        if (netdbEntry::UseCount() < Config.Netdb.low)
            break;

        netdbRelease(*(list + k));
    }

    xfree(list);
}

static netdbEntry *
netdbLookupAddr(const Ip::Address &addr)
{
    netdbEntry *n;
    char *key = new char[MAX_IPSTRLEN];
    networkFromInaddr(addr).toStr(key,MAX_IPSTRLEN);
    n = (netdbEntry *) hash_lookup(addr_table, key);
    delete[] key;
    return n;
}

static netdbEntry *
netdbAdd(Ip::Address &addr)
{
    netdbEntry *n;

    if (netdbEntry::UseCount() > Config.Netdb.high)
        netdbPurgeLRU();

    if ((n = netdbLookupAddr(addr)) == nullptr) {
        n = new netdbEntry;
        netdbHashInsert(n, addr);
    }

    return n;
}

static void
netdbSendPing(const ipcache_addrs *ia, const Dns::LookupDetails &, void *data)
{
    Ip::Address addr;
    char *hostname = nullptr;
    static_cast<generic_cbdata *>(data)->unwrap(&hostname);
    netdbEntry *n;
    netdbEntry *na;
    net_db_name *x;
    net_db_name **X;

    if (ia == nullptr) {
        xfree(hostname);
        return;
    }

    addr = ia->current();

    if ((n = netdbLookupHost(hostname)) == nullptr) {
        n = netdbAdd(addr);
        netdbHostInsert(n, hostname);
    } else if ((na = netdbLookupAddr(addr)) != n) {
        /*
         *hostname moved from 'network n' to 'network na'!
         */

        if (na == nullptr)
            na = netdbAdd(addr);

        debugs(38, 3, "netdbSendPing: " << hostname << " moved from " << n->network << " to " << na->network);

        x = (net_db_name *) hash_lookup(host_table, hostname);

        if (x == nullptr) {
            debugs(38, DBG_IMPORTANT, "ERROR: Squid BUG: net_db_name list bug: " << hostname << " not found");
            xfree(hostname);
            return;
        }

        /* remove net_db_name from 'network n' linked list */
        for (X = &n->hosts; *X; X = &(*X)->next) {
            if (*X == x) {
                *X = x->next;
                break;
            }
        }

        -- n->link_count;
        /* point to 'network na' from host entry */
        x->net_db_entry = na;
        /* link net_db_name to 'network na' */
        x->next = na->hosts;
        na->hosts = x;
        ++ na->link_count;
        n = na;
    }

    if (n->next_ping_time <= squid_curtime) {
        debugs(38, 3, "netdbSendPing: pinging " << hostname);
        icmpEngine.DomainPing(addr, hostname);
        ++ n->pings_sent;
        n->next_ping_time = squid_curtime + Config.Netdb.period;
        n->last_use_time = squid_curtime;
    }

    xfree(hostname);
}

static Ip::Address
networkFromInaddr(const Ip::Address &in)
{
    Ip::Address out;

    out = in;

    /* in IPv6 the 'network' should be the routing section. */
    if ( in.isIPv6() ) {
        out.applyMask(64, AF_INET6);
        debugs(14, 5, "networkFromInaddr : Masked IPv6 Address to " << in << "/64 routing part.");
        return out;
    }

    debugs(14, 5, "networkFromInaddr : Masked IPv4 Address to " << out << "/24.");

    /* use /24 for everything under IPv4 */
    out.applyMask(24, AF_INET);
    debugs(14, 5, "networkFromInaddr : Masked IPv4 Address to " << in << "/24.");

    return out;
}

static int
sortByRtt(const void *A, const void *B)
{
    const netdbEntry *const *n1 = (const netdbEntry *const *)A;
    const netdbEntry *const *n2 = (const netdbEntry *const *)B;

    if ((*n1)->rtt > (*n2)->rtt)
        return 1;
    else if ((*n1)->rtt < (*n2)->rtt)
        return -1;
    else
        return 0;
}

static net_db_peer *
netdbPeerByName(const netdbEntry * n, const char *peername)
{
    int i;
    net_db_peer *p = n->peers;

    for (i = 0; i < n->n_peers; ++i, ++p) {
        if (!strcmp(p->peername, peername))
            return p;
    }

    return nullptr;
}

static net_db_peer *
netdbPeerAdd(netdbEntry * n, CachePeer * e)
{
    net_db_peer *p;
    net_db_peer *o;
    int osize;
    int i;

    if (n->n_peers == n->n_peers_alloc) {
        o = n->peers;
        osize = n->n_peers_alloc;

        if (n->n_peers_alloc == 0)
            n->n_peers_alloc = 2;
        else
            n->n_peers_alloc <<= 1;

        debugs(38, 3, "netdbPeerAdd: Growing peer list for '" << n->network << "' to " << n->n_peers_alloc);

        n->peers = (net_db_peer *)xcalloc(n->n_peers_alloc, sizeof(net_db_peer));

        for (i = 0; i < osize; ++i)
            *(n->peers + i) = *(o + i);

        if (osize) {
            safe_free(o);
        }
    }

    p = n->peers + n->n_peers;
    p->peername = netdbPeerName(e->host);
    ++ n->n_peers;
    return p;
}

static int
sortPeerByRtt(const void *A, const void *B)
{
    const net_db_peer *p1 = (net_db_peer *)A;
    const net_db_peer *p2 = (net_db_peer *)B;

    if (p1->rtt > p2->rtt)
        return 1;
    else if (p1->rtt < p2->rtt)
        return -1;
    else
        return 0;
}

static void
netdbSaveState(void *)
{
    if (strcmp(Config.netdbFilename, "none") == 0)
        return;

    Logfile *lf;
    netdbEntry *n;
    net_db_name *x;

    struct timeval start = current_time;
    int count = 0;
    /*
     * This was nicer when we were using stdio, but thanks to
     * Solaris bugs, its a bad idea.  fopen can fail if more than
     * 256 FDs are open.
     */
    /*
     * unlink() is here because there is currently no way to make
     * logfileOpen() use O_TRUNC.
     */
    unlink(Config.netdbFilename);
    lf = logfileOpen(Config.netdbFilename, 4096, 0);

    if (!lf) {
        int xerrno = errno;
        debugs(50, DBG_IMPORTANT, MYNAME << Config.netdbFilename << ": " << xstrerr(xerrno));
        return;
    }

    hash_first(addr_table);

    while ((n = (netdbEntry *) hash_next(addr_table))) {
        if (n->pings_recv == 0)
            continue;

        logfilePrintf(lf, "%s %d %d %10.5f %10.5f %d %d",
                      n->network,
                      n->pings_sent,
                      n->pings_recv,
                      n->hops,
                      n->rtt,
                      (int) n->next_ping_time,
                      (int) n->last_use_time);

        for (x = n->hosts; x; x = x->next)
            logfilePrintf(lf, " %s", hashKeyStr(x));

        logfilePrintf(lf, "\n");

        ++count;

#undef RBUF_SZ

    }

    logfileClose(lf);
    getCurrentTime();
    debugs(38, DBG_IMPORTANT, "NETDB state saved; " <<
           count << " entries, " <<
           tvSubMsec(start, current_time) << " msec" );
    eventAddIsh("netdbSaveState", netdbSaveState, nullptr, 3600.0, 1);
}

static void
netdbReloadState(void)
{
    if (strcmp(Config.netdbFilename, "none") == 0)
        return;

    char *s;
    int fd;
    int l;

    struct stat sb;
    netdbEntry *n;
    netdbEntry N;

    Ip::Address addr;
    int count = 0;

    struct timeval start = current_time;
    /*
     * This was nicer when we were using stdio, but thanks to
     * Solaris bugs, its a bad idea.  fopen can fail if more than
     * 256 FDs are open.
     */
    fd = file_open(Config.netdbFilename, O_RDONLY | O_BINARY);

    if (fd < 0)
        return;

    if (fstat(fd, &sb) < 0) {
        file_close(fd);
        return;
    }

    char *t;
    char *buf = (char *)xcalloc(1, sb.st_size + 1);
    t = buf;
    l = FD_READ_METHOD(fd, buf, sb.st_size);
    file_close(fd);

    if (l <= 0) {
        safe_free (buf);
        return;
    };

    while ((s = strchr(t, '\n'))) {
        char *q;
        assert(s - buf < l);
        *s = '\0';
        N = netdbEntry();
        q = strtok(t, w_space);
        t = s + 1;

        if (nullptr == q)
            continue;

        if (! (addr = q) )
            continue;

        if (netdbLookupAddr(addr) != nullptr)  /* no dups! */
            continue;

        if ((q = strtok(nullptr, w_space)) == nullptr)
            continue;

        N.pings_sent = atoi(q);

        if ((q = strtok(nullptr, w_space)) == nullptr)
            continue;

        N.pings_recv = atoi(q);

        if (N.pings_recv == 0)
            continue;

        /* give this measurement low weight */
        N.pings_sent = 1;

        N.pings_recv = 1;

        if ((q = strtok(nullptr, w_space)) == nullptr)
            continue;

        N.hops = atof(q);

        if ((q = strtok(nullptr, w_space)) == nullptr)
            continue;

        N.rtt = atof(q);

        if ((q = strtok(nullptr, w_space)) == nullptr)
            continue;

        N.next_ping_time = (time_t) atoi(q);

        if ((q = strtok(nullptr, w_space)) == nullptr)
            continue;

        N.last_use_time = (time_t) atoi(q);

        n = new netdbEntry;

        memcpy(n, &N, sizeof(netdbEntry));

        netdbHashInsert(n, addr);

        while ((q = strtok(nullptr, w_space)) != nullptr) {
            if (netdbLookupHost(q) != nullptr) /* no dups! */
                continue;

            netdbHostInsert(n, q);
        }

        ++count;
    }

    xfree(buf);
    getCurrentTime();
    debugs(38, DBG_IMPORTANT, "NETDB state reloaded; " <<
           count << " entries, " <<
           tvSubMsec(start, current_time) << " msec" );
}

static const char *
netdbPeerName(const char *name)
{
    const wordlist *w;

    for (w = peer_names; w; w = w->next) {
        if (!strcmp(w->key, name))
            return w->key;
    }

    return wordlistAdd(&peer_names, name);
}

static void
netdbExchangeHandleReply(void *data, StoreIOBuffer receivedData)
{
    Ip::Address addr;

    netdbExchangeState *ex = (netdbExchangeState *)data;

    struct in_addr line_addr;
    double rtt;
    double hops;
    int j;
    int nused = 0;

    size_t rec_sz = 0; // received record size (TODO: make const)
    rec_sz += 1 + sizeof(struct in_addr);
    rec_sz += 1 + sizeof(int);
    rec_sz += 1 + sizeof(int);
    // to make progress without growing buffer space, we must parse at least one record per call
    Assure(rec_sz <= ex->parsingBuffer.capacity());
    debugs(38, 3, "netdbExchangeHandleReply: " << receivedData.length << " read bytes");

    if (!ex->p.valid()) {
        debugs(38, 3, "netdbExchangeHandleReply: Peer became invalid");
        delete ex;
        return;
    }

    debugs(38, 3, "for " << *ex->p);

    if (receivedData.flags.error) {
        delete ex;
        return;
    }

    if (ex->connstate == STATE_HEADER) {
        const auto scode = ex->e->mem().baseReply().sline.status();
        assert(scode != Http::scNone);
        debugs(38, 3, "reply status " << scode);
        if (scode != Http::scOkay) {
            delete ex;
            return;
        }
        ex->connstate = STATE_BODY;
    }

    assert(ex->connstate == STATE_BODY);

    ex->parsingBuffer.appended(receivedData.data, receivedData.length);
    auto p = ex->parsingBuffer.c_str(); // current parsing position
    auto size = ex->parsingBuffer.contentSize(); // bytes we still need to parse

    /* If we get here, we have some body to parse .. */
    debugs(38, 5, "netdbExchangeHandleReply: start parsing loop, size = " << size);

    while (size >= rec_sz) {
        debugs(38, 5, "netdbExchangeHandleReply: in parsing loop, size = " << size);
        addr.setAnyAddr();
        hops = rtt = 0.0;

        size_t o; // current record parsing offset
        for (o = 0; o < rec_sz;) {
            switch ((int) *(p + o)) {

            case NETDB_EX_NETWORK:
                ++o;
                // XXX: NetDB can still only send IPv4
                memcpy(&line_addr, p + o, sizeof(struct in_addr));
                addr = line_addr;
                o += sizeof(struct in_addr);
                break;

            case NETDB_EX_RTT:
                ++o;
                memcpy(&j, p + o, sizeof(int));
                o += sizeof(int);
                rtt = (double) ntohl(j) / 1000.0;
                break;

            case NETDB_EX_HOPS:
                ++o;
                memcpy(&j, p + o, sizeof(int));
                o += sizeof(int);
                hops = (double) ntohl(j) / 1000.0;
                break;

            default:
                debugs(38, DBG_IMPORTANT, "ERROR: netdbExchangeHandleReply: corrupt data, aborting");
                delete ex;
                return;
            }
        }

        if (!addr.isAnyAddr() && rtt > 0)
            netdbExchangeUpdatePeer(addr, ex->p.get(), rtt, hops);

        assert(o == rec_sz);

        size -= rec_sz;

        p += rec_sz;

        ++nused;
    }

    const auto parsedSize = ex->parsingBuffer.contentSize() - size;
    ex->parsingBuffer.consume(parsedSize);

    debugs(38, 3, "netdbExchangeHandleReply: size left over in this buffer: " << size << " bytes");

    debugs(38, 3, "netdbExchangeHandleReply: used " << nused <<
           " entries, (x " << rec_sz << " bytes) == " << nused * rec_sz <<
           " bytes total");

    if (EBIT_TEST(ex->e->flags, ENTRY_ABORTED)) {
        debugs(38, 3, "netdbExchangeHandleReply: ENTRY_ABORTED");
        delete ex;
        return;
    }

    if (ex->sc->atEof()) {
        if (const auto leftoverBytes = ex->parsingBuffer.contentSize())
            debugs(38, 2, "discarding a partially received record due to Store EOF: " << leftoverBytes);
        delete ex;
        return;
    }

    // TODO: To protect us from a broken peer sending an "infinite" stream of
    // new addresses, limit the cumulative number of received bytes or records?

    const auto remainingSpace = ex->parsingBuffer.space().positionAt(receivedData.offset + receivedData.length);
    // rec_sz is at most buffer capacity, and we consume all fully loaded records
    Assure(remainingSpace.length);
    storeClientCopy(ex->sc, ex->e, remainingSpace, netdbExchangeHandleReply, ex);
}

#endif /* USE_ICMP */

/* PUBLIC FUNCTIONS */

void
netdbInit(void)
{
#if USE_ICMP
    Mgr::RegisterAction("netdb", "Network Measurement Database", netdbDump, 0, 1);

    if (addr_table)
        return;

    int n = hashPrime(Config.Netdb.high / 4);

    addr_table = hash_create((HASHCMP *) strcmp, n, hash_string);

    n = hashPrime(3 * Config.Netdb.high / 4);

    host_table = hash_create((HASHCMP *) strcmp, n, hash_string);

    eventAddIsh("netdbSaveState", netdbSaveState, nullptr, 3600.0, 1);

    netdbReloadState();

#endif
}

void
netdbPingSite(const char *hostname)
{
#if USE_ICMP
    netdbEntry *n;

    if ((n = netdbLookupHost(hostname)) != nullptr)
        if (n->next_ping_time > squid_curtime)
            return;

    ipcache_nbgethostbyname(hostname, netdbSendPing,
                            new generic_cbdata(xstrdup(hostname)));
#else
    (void)hostname;
#endif
}

void
netdbHandlePingReply(const Ip::Address &from, int hops, int rtt)
{
#if USE_ICMP
    netdbEntry *n;
    int N;
    debugs(38, 3, "netdbHandlePingReply: from " << from);

    if ((n = netdbLookupAddr(from)) == nullptr)
        return;

    N = ++n->pings_recv;

    if (N > 5)
        N = 5;

    if (rtt < 1)
        rtt = 1;

    n->hops = ((n->hops * (N - 1)) + hops) / N;

    n->rtt = ((n->rtt * (N - 1)) + rtt) / N;

    debugs(38, 3, "netdbHandlePingReply: " << n->network  << "; rtt="<<
           std::setw(5)<< std::setprecision(2) << n->rtt << "  hops="<<
           std::setw(4) << n->hops);
#else
    (void)from;
    (void)hops;
    (void)rtt;
#endif
}

void
netdbDump(StoreEntry * sentry)
{
#if USE_ICMP
    netdbEntry *n;
    netdbEntry **list;
    net_db_name *x;
    int k;
    int i;
    int j;
    net_db_peer *p;
    storeAppendPrintf(sentry, "Network DB Statistics:\n");
    storeAppendPrintf(sentry, "%-46.46s %9s %7s %5s %s\n",  /* Max between 16 (IPv4) or 46 (IPv6)   */
                      "Network",
                      "recv/sent",
                      "RTT",
                      "Hops",
                      "Hostnames");
    list = (netdbEntry **)xcalloc(netdbEntry::UseCount(), sizeof(netdbEntry *));
    i = 0;
    hash_first(addr_table);

    while ((n = (netdbEntry *) hash_next(addr_table))) {
        *(list + i) = n;
        ++i;
    }

    if (i != netdbEntry::UseCount())
        debugs(38, DBG_CRITICAL, "WARNING: netdb_addrs count off, found " << i <<
               ", expected " << netdbEntry::UseCount());

    qsort((char *) list,
          i,
          sizeof(netdbEntry *),
          sortByRtt);

    for (k = 0; k < i; ++k) {
        n = *(list + k);
        storeAppendPrintf(sentry, "%-46.46s %4d/%4d %7.1f %5.1f", /* Max between 16 (IPv4) or 46 (IPv6)   */
                          n->network,
                          n->pings_recv,
                          n->pings_sent,
                          n->rtt,
                          n->hops);

        for (x = n->hosts; x; x = x->next)
            storeAppendPrintf(sentry, " %s", hashKeyStr(x));

        storeAppendPrintf(sentry, "\n");

        p = n->peers;

        for (j = 0; j < n->n_peers; ++j, ++p) {
            storeAppendPrintf(sentry, "    %-22.22s %7.1f %5.1f\n",
                              p->peername,
                              p->rtt,
                              p->hops);
        }
    }

    xfree(list);
#else

    storeAppendPrintf(sentry,"NETDB support not compiled into this Squid cache.\n");
#endif
}

int
netdbHostHops(const char *host)
{
#if USE_ICMP
    netdbEntry *n = netdbLookupHost(host);

    if (n) {
        n->last_use_time = squid_curtime;
        return (int) (n->hops + 0.5);
    }
#else
    (void)host;
#endif
    return 0;
}

int
netdbHostRtt(const char *host)
{
#if USE_ICMP
    netdbEntry *n = netdbLookupHost(host);

    if (n) {
        n->last_use_time = squid_curtime;
        return (int) (n->rtt + 0.5);
    }

#else
    (void)host;
#endif
    return 0;
}

void
netdbHostData(const char *host, int *samp, int *rtt, int *hops)
{
#if USE_ICMP
    netdbEntry *n = netdbLookupHost(host);

    if (n == nullptr)
        return;

    *samp = n->pings_recv;

    *rtt = (int) (n->rtt + 0.5);

    *hops = (int) (n->hops + 0.5);

    n->last_use_time = squid_curtime;

#else
    (void)hops;
    (void)rtt;
    (void)samp;
    (void)host;
#endif
}

void
netdbUpdatePeer(const AnyP::Uri &url, CachePeer *e, int irtt, int ihops)
{
#if USE_ICMP
    netdbEntry *n;
    double rtt = (double) irtt;
    double hops = (double) ihops;
    net_db_peer *p;
    debugs(38, 3, url.host() << ", " << ihops << " hops, " << irtt << " rtt");
    n = netdbLookupHost(url.host());

    if (n == nullptr) {
        debugs(38, 3, "host " << url.host() << " not found");
        return;
    }

    if ((p = netdbPeerByName(n, e->host)) == nullptr)
        p = netdbPeerAdd(n, e);

    p->rtt = rtt;

    p->hops = hops;

    p->expires = squid_curtime + 3600;

    if (n->n_peers < 2)
        return;

    qsort((char *) n->peers,
          n->n_peers,
          sizeof(net_db_peer),
          sortPeerByRtt);

#else
    (void)ihops;
    (void)irtt;
    (void)e;
    (void)url;
#endif
}

void
netdbExchangeUpdatePeer(Ip::Address &addr, CachePeer * e, double rtt, double hops)
{
#if USE_ICMP
    netdbEntry *n;
    net_db_peer *p;
    debugs(38, 5, "netdbExchangeUpdatePeer: '" << addr << "', "<<
           std::setfill('0')<< std::setprecision(2) << hops << " hops, " <<
           rtt << " rtt");

    if ( !addr.isIPv4() ) {
        debugs(38, 5, "netdbExchangeUpdatePeer: Aborting peer update for '" << addr << "', NetDB cannot handle IPv6.");
        return;
    }

    n = netdbLookupAddr(addr);

    if (n == nullptr)
        n = netdbAdd(addr);

    assert(nullptr != n);

    if ((p = netdbPeerByName(n, e->host)) == nullptr)
        p = netdbPeerAdd(n, e);

    p->rtt = rtt;

    p->hops = hops;

    p->expires = squid_curtime + 3600;  /* XXX ? */

    if (n->n_peers < 2)
        return;

    qsort((char *) n->peers,
          n->n_peers,
          sizeof(net_db_peer),
          sortPeerByRtt);

#else
    (void)hops;
    (void)rtt;
    (void)e;
    (void)addr;
#endif
}

void
netdbDeleteAddrNetwork(Ip::Address &addr)
{
#if USE_ICMP
    netdbEntry *n = netdbLookupAddr(addr);

    if (n == nullptr)
        return;

    debugs(38, 3, "netdbDeleteAddrNetwork: " << n->network);

    netdbRelease(n);
#else
    (void)addr;
#endif
}

void
netdbBinaryExchange(StoreEntry * s)
{
    HttpReply *reply = new HttpReply;
#if USE_ICMP

    Ip::Address addr;

    netdbEntry *n;
    int i;
    int j;
    int rec_sz;
    char *buf;

    struct in_addr line_addr;
    s->buffer();
    reply->setHeaders(Http::scOkay, "OK", nullptr, -1, squid_curtime, -2);
    s->replaceHttpReply(reply);
    rec_sz = 0;
    rec_sz += 1 + sizeof(struct in_addr);
    rec_sz += 1 + sizeof(int);
    rec_sz += 1 + sizeof(int);
    buf = (char *)memAllocate(MEM_4K_BUF);
    i = 0;
    hash_first(addr_table);

    while ((n = (netdbEntry *) hash_next(addr_table))) {
        if (0.0 == n->rtt)
            continue;

        if (n->rtt > 60000) /* RTT > 1 MIN probably bogus */
            continue;

        if (! (addr = n->network) )
            continue;

        // XXX: NetDB cannot yet handle IPv6 addresses
        if ( !addr.isIPv4() )
            continue;

        buf[i] = (char) NETDB_EX_NETWORK;
        ++i;

        addr.getInAddr(line_addr);
        memcpy(&buf[i], &line_addr, sizeof(struct in_addr));

        i += sizeof(struct in_addr);

        buf[i] = (char) NETDB_EX_RTT;
        ++i;

        j = htonl((int) (n->rtt * 1000));

        memcpy(&buf[i], &j, sizeof(int));

        i += sizeof(int);

        buf[i] = (char) NETDB_EX_HOPS;
        ++i;

        j = htonl((int) (n->hops * 1000));

        memcpy(&buf[i], &j, sizeof(int));

        i += sizeof(int);

        if (i + rec_sz > 4096) {
            s->append(buf, i);
            i = 0;
        }
    }

    if (i > 0) {
        s->append(buf, i);
        i = 0;
    }

    assert(0 == i);
    s->flush();
    memFree(buf, MEM_4K_BUF);
#else

    reply->setHeaders(Http::scBadRequest, "Bad Request", nullptr, -1, squid_curtime, -2);
    s->replaceHttpReply(reply);
    storeAppendPrintf(s, "NETDB support not compiled into this Squid cache.\n");
#endif

    s->complete();
}

void
netdbExchangeStart(void *data)
{
#if USE_ICMP
    CachePeer *p = (CachePeer *)data;
    static const SBuf netDB("netdb");
    char *uri = internalRemoteUri(p->secure.encryptTransport, p->host, p->http_port, "/squid-internal-dynamic/", netDB);
    debugs(38, 3, "Requesting '" << uri << "'");
    const auto mx = MasterXaction::MakePortless<XactionInitiator::initIcmp>();
    HttpRequestPointer req(HttpRequest::FromUrlXXX(uri, mx));

    if (!req) {
        debugs(38, DBG_IMPORTANT, "ERROR: " << MYNAME << ": Bad URI " << uri);
        return;
    }

    netdbExchangeState *ex = new netdbExchangeState(p, req);
    ex->e = storeCreateEntry(uri, uri, RequestFlags(), Http::METHOD_GET);
    assert(nullptr != ex->e);

    ex->sc = storeClientListAdd(ex->e, ex);
    storeClientCopy(ex->sc, ex->e, ex->parsingBuffer.makeInitialSpace(), netdbExchangeHandleReply, ex);

    ex->r->flags.loopDetected = true;   /* cheat! -- force direct */

    // XXX: send as Proxy-Authenticate instead
    if (p->login)
        ex->r->url.userInfo(SBuf(p->login));

    FwdState::fwdStart(Comm::ConnectionPointer(), ex->e, ex->r.getRaw());
#else
    (void)data;
#endif
}

#if USE_ICMP
/// a netdbClosestParent() helper to find the first usable parent CachePeer
/// responsible for the given hostname
static CachePeer *
findUsableParentAtHostname(PeerSelector *ps, const char * const hostname, const HttpRequest &request)
{
    for (const auto &peer: CurrentCachePeers()) {
        const auto p = peer.get();
        // Both fields should be lowercase, but no code ensures that invariant.
        // TODO: net_db_peer should point to CachePeer instead of its hostname!
        if (strcasecmp(p->host, hostname) != 0)
            continue;

        if (neighborType(p, request.url) != PEER_PARENT)
            continue;

        if (!peerHTTPOkay(p, ps))
            continue;

        return p;
    }

    return nullptr;
}
#endif

CachePeer *
netdbClosestParent(PeerSelector *ps)
{
#if USE_ICMP
    assert(ps);
    HttpRequest *request = ps->request;

    netdbEntry *n;
    const ipcache_addrs *ia;
    net_db_peer *h;
    int i;
    n = netdbLookupHost(request->url.host());

    if (nullptr == n) {
        /* try IP addr */
        ia = ipcache_gethostbyname(request->url.host(), 0);

        if (nullptr != ia)
            n = netdbLookupAddr(ia->current());
    }

    if (nullptr == n)
        return nullptr;

    if (0 == n->n_peers)
        return nullptr;

    n->last_use_time = squid_curtime;

    /*
     * Find the parent with the least RTT to the origin server.
     * Make sure we don't return a parent who is farther away than
     * we are.  Note, the n->peers list is pre-sorted by RTT.
     */
    for (i = 0; i < n->n_peers; ++i) {
        h = &n->peers[i];

        if (n->rtt > 0)
            if (n->rtt < h->rtt)
                break;

        if (const auto p = findUsableParentAtHostname(ps, h->peername, *request))
            return p;
    }

#else
    (void)ps;
#endif
    return nullptr;
}

