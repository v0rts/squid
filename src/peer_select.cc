/*
 * Copyright (C) 1996-2017 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 44    Peer Selection Algorithm */

#include "squid.h"
#include "acl/FilledChecklist.h"
#include "base/InstanceId.h"
#include "CachePeer.h"
#include "carp.h"
#include "client_side.h"
#include "dns/LookupDetails.h"
#include "errorpage.h"
#include "event.h"
#include "FwdState.h"
#include "globals.h"
#include "hier_code.h"
#include "htcp.h"
#include "http/Stream.h"
#include "HttpRequest.h"
#include "icmp/net_db.h"
#include "ICP.h"
#include "ip/tools.h"
#include "ipcache.h"
#include "neighbors.h"
#include "peer_sourcehash.h"
#include "peer_userhash.h"
#include "PeerSelectState.h"
#include "SquidConfig.h"
#include "SquidTime.h"
#include "Store.h"
#include "URL.h"

/**
 * A CachePeer which has been selected as a possible destination.
 * Listed as pointers here so as to prevent duplicates being added but will
 * be converted to a set of IP address path options before handing back out
 * to the caller.
 *
 * Certain connection flags and outgoing settings will also be looked up and
 * set based on the received request and CachePeer settings before handing back.
 */
class FwdServer
{
    MEMPROXY_CLASS(FwdServer);

public:
    FwdServer(CachePeer *p, hier_code c) :
        _peer(p),
        code(c),
        next(nullptr)
    {}

    CbcPointer<CachePeer> _peer;                /* NULL --> origin server */
    hier_code code;
    FwdServer *next;
};

static struct {
    int timeouts;
} PeerStats;

static const char *DirectStr[] = {
    "DIRECT_UNKNOWN",
    "DIRECT_NO",
    "DIRECT_MAYBE",
    "DIRECT_YES"
};

static void peerSelectFoo(ps_state *);
static void peerPingTimeout(void *data);
static IRCB peerHandlePingReply;
static void peerIcpParentMiss(CachePeer *, icp_common_t *, ps_state *);
#if USE_HTCP
static void peerHtcpParentMiss(CachePeer *, HtcpReplyData *, ps_state *);
static void peerHandleHtcpReply(CachePeer *, peer_t, HtcpReplyData *, void *);
#endif
static int peerCheckNetdbDirect(ps_state * psstate);
static void peerGetSomeNeighbor(ps_state *);
static void peerGetSomeNeighborReplies(ps_state *);
static void peerGetSomeDirect(ps_state *);
static void peerGetSomeParent(ps_state *);
static void peerGetAllParents(ps_state *);
static void peerAddFwdServer(FwdServer **, CachePeer *, hier_code);
static void peerSelectPinned(ps_state * ps);

CBDATA_CLASS_INIT(ps_state);

ps_state::~ps_state()
{
    while (servers) {
        FwdServer *next = servers->next;
        delete servers;
        servers = next;
    }

    if (entry) {
        debugs(44, 3, entry->url());

        if (entry->ping_status == PING_WAITING)
            eventDelete(peerPingTimeout, this);

        entry->ping_status = PING_DONE;
    }

    if (acl_checklist) {
        debugs(44, DBG_IMPORTANT, "calling aclChecklistFree() from ps_state destructor");
        delete acl_checklist;
    }

    HTTPMSGUNLOCK(request);

    if (entry) {
        assert(entry->ping_status != PING_WAITING);
        entry->unlock("peerSelect");
        entry = NULL;
    }

    delete lastError;
}

static int
peerSelectIcpPing(HttpRequest * request, int direct, StoreEntry * entry)
{
    int n;
    assert(entry);
    assert(entry->ping_status == PING_NONE);
    assert(direct != DIRECT_YES);
    debugs(44, 3, "peerSelectIcpPing: " << entry->url());

    if (!request->flags.hierarchical && direct != DIRECT_NO)
        return 0;

    if (EBIT_TEST(entry->flags, KEY_PRIVATE) && !neighbors_do_private_keys)
        if (direct != DIRECT_NO)
            return 0;

    n = neighborsCount(request);

    debugs(44, 3, "peerSelectIcpPing: counted " << n << " neighbors");

    return n;
}

static void
peerSelect(PeerSelectionInitiator *initiator,
           HttpRequest * request,
           AccessLogEntry::Pointer const &al,
           StoreEntry * entry)
{
    if (entry)
        debugs(44, 3, *entry << ' ' << entry->url());
    else
        debugs(44, 3, request->method);

    const auto psstate = new ps_state(initiator);

    psstate->request = request;
    HTTPMSGLOCK(psstate->request);
    psstate->al = al;

    psstate->entry = entry;

#if USE_CACHE_DIGESTS

    request->hier.peer_select_start = current_time;

#endif

    if (psstate->entry)
        psstate->entry->lock("peerSelect");

    peerSelectFoo(psstate);
}

void
PeerSelectionInitiator::startSelectingDestinations(HttpRequest *request, const AccessLogEntry::Pointer &ale, StoreEntry *entry)
{
    subscribed = true;
    peerSelect(this, request, ale, entry);
    // and wait for noteDestination() and/or noteDestinationsEnd() calls
}

static void
peerCheckNeverDirectDone(allow_t answer, void *data)
{
    ps_state *psstate = (ps_state *) data;
    psstate->acl_checklist = NULL;
    debugs(44, 3, "peerCheckNeverDirectDone: " << answer);
    psstate->never_direct = answer;
    switch (answer) {
    case ACCESS_ALLOWED:
        /** if never_direct says YES, do that. */
        psstate->direct = DIRECT_NO;
        debugs(44, 3, HERE << "direct = " << DirectStr[psstate->direct] << " (never_direct allow)");
        break;
    case ACCESS_DENIED: // not relevant.
    case ACCESS_DUNNO:  // not relevant.
        break;
    case ACCESS_AUTH_REQUIRED:
        debugs(44, DBG_IMPORTANT, "WARNING: never_direct resulted in " << answer << ". Username ACLs are not reliable here.");
        break;
    }
    peerSelectFoo(psstate);
}

static void
peerCheckAlwaysDirectDone(allow_t answer, void *data)
{
    ps_state *psstate = (ps_state *)data;
    psstate->acl_checklist = NULL;
    debugs(44, 3, "peerCheckAlwaysDirectDone: " << answer);
    psstate->always_direct = answer;
    switch (answer) {
    case ACCESS_ALLOWED:
        /** if always_direct says YES, do that. */
        psstate->direct = DIRECT_YES;
        debugs(44, 3, HERE << "direct = " << DirectStr[psstate->direct] << " (always_direct allow)");
        break;
    case ACCESS_DENIED: // not relevant.
    case ACCESS_DUNNO:  // not relevant.
        break;
    case ACCESS_AUTH_REQUIRED:
        debugs(44, DBG_IMPORTANT, "WARNING: always_direct resulted in " << answer << ". Username ACLs are not reliable here.");
        break;
    }
    peerSelectFoo(psstate);
}

/// \returns true (after destroying psstate) if the peer initiator is gone
/// \returns false (without side effects) otherwise
static bool
peerSelectionAborted(ps_state *psstate)
{
    if (psstate->interestedInitiator())
        return false;

    debugs(44, 3, "Aborting peer selection: Initiator gone or lost interest.");
    delete psstate;
    return true;
}

void
peerSelectDnsPaths(ps_state *psstate)
{
    if (peerSelectionAborted(psstate))
        return;

    FwdServer *fs = psstate->servers;

    // Bug 3243: CVE 2009-0801
    // Bypass of browser same-origin access control in intercepted communication
    // To resolve this we must use only the original client destination when going DIRECT
    // on intercepted traffic which failed Host verification
    const HttpRequest *req = psstate->request;
    const bool isIntercepted = !req->flags.redirected &&
                               (req->flags.intercepted || req->flags.interceptTproxy);
    const bool useOriginalDst = Config.onoff.client_dst_passthru || !req->flags.hostVerified;
    const bool choseDirect = fs && fs->code == HIER_DIRECT;
    if (isIntercepted && useOriginalDst && choseDirect) {
        // check the client is still around before using any of its details
        if (req->clientConnectionManager.valid()) {
            // construct a "result" adding the ORIGINAL_DST to the set instead of DIRECT
            Comm::ConnectionPointer p = new Comm::Connection();
            p->remote = req->clientConnectionManager->clientConnection->local;
            fs->code = ORIGINAL_DST; // fs->code is DIRECT. This fixes the display.
            psstate->handlePath(p, *fs);
        }

        // clear the used fs and continue
        psstate->servers = fs->next;
        delete fs;
        peerSelectDnsPaths(psstate);
        return;
    }

    // convert the list of FwdServer destinations into destinations IP addresses
    if (fs && psstate->wantsMoreDestinations()) {
        // send the next one off for DNS lookup.
        const char *host = fs->_peer.valid() ? fs->_peer->host : psstate->request->url.host();
        debugs(44, 2, "Find IP destination for: " << psstate->url() << "' via " << host);
        Dns::nbgethostbyname(host, psstate);
        return;
    }

    // Bug 3605: clear any extra listed FwdServer destinations, when the options exceeds max_foward_tries.
    // due to the allocation method of fs, we must deallocate each manually.
    // TODO: use a std::list so we can get the size and abort adding whenever the selection loops reach Config.forward_max_tries
    if (fs) {
        assert(fs == psstate->servers);
        while (fs) {
            psstate->servers = fs->next;
            delete fs;
            fs = psstate->servers;
        }
    }

    // done with DNS lookups. pass back to caller

    debugs(44, 2, psstate->id << " found all " << psstate->foundPaths << " destinations for " << psstate->url());
    debugs(44, 2, "  always_direct = " << psstate->always_direct);
    debugs(44, 2, "   never_direct = " << psstate->never_direct);
    debugs(44, 2, "       timedout = " << psstate->ping.timedout);

    psstate->ping.stop = current_time;
    psstate->request->hier.ping = psstate->ping; // final result

    if (psstate->lastError && psstate->foundPaths) {
        // nobody cares about errors if we found destinations despite them
        debugs(44, 3, "forgetting the last error");
        delete psstate->lastError;
        psstate->lastError = nullptr;
    }

    if (const auto initiator = psstate->interestedInitiator())
        initiator->noteDestinationsEnd(psstate->lastError);
    psstate->lastError = nullptr; // initiator owns the ErrorState object now
    delete psstate;
}

void
ps_state::noteLookup(const Dns::LookupDetails &details)
{
    /* ignore lookup delays that occurred after the initiator moved on */

    if (peerSelectionAborted(this))
        return;

    if (!wantsMoreDestinations())
        return;

    request->recordLookup(details);
}

void
ps_state::noteIp(const Ip::Address &ip)
{
    if (peerSelectionAborted(this))
        return;

    if (!wantsMoreDestinations())
        return;

    const auto peer = servers->_peer.valid();

    // for TPROXY spoofing, we must skip unusable addresses
    if (request->flags.spoofClientIp && !(peer && peer->options.no_tproxy) ) {
        if (ip.isIPv4() != request->client_addr.isIPv4())
            return; // cannot spoof the client address on this link
    }

    Comm::ConnectionPointer p = new Comm::Connection();
    p->remote = ip;
    p->remote.port(peer ? peer->http_port : request->url.port());
    handlePath(p, *servers);
}

void
ps_state::noteIps(const Dns::CachedIps *ia, const Dns::LookupDetails &details)
{
    if (peerSelectionAborted(this))
        return;

    FwdServer *fs = servers;
    if (!ia) {
        debugs(44, 3, "Unknown host: " << (fs->_peer.valid() ? fs->_peer->host : request->url.host()));
        // discard any previous error.
        delete lastError;
        lastError = NULL;
        if (fs->code == HIER_DIRECT) {
            lastError = new ErrorState(ERR_DNS_FAIL, Http::scServiceUnavailable, request);
            lastError->dnsError = details.error;
        }
    }
    // else noteIp() calls have already processed all IPs in *ia

    servers = fs->next;
    delete fs;

    // see if more paths can be found
    peerSelectDnsPaths(this);
}

static int
peerCheckNetdbDirect(ps_state * psstate)
{
#if USE_ICMP
    CachePeer *p;
    int myrtt;
    int myhops;

    if (psstate->direct == DIRECT_NO)
        return 0;

    /* base lookup on RTT and Hops if ICMP NetDB is enabled. */

    myrtt = netdbHostRtt(psstate->request->url.host());
    debugs(44, 3, "MY RTT = " << myrtt << " msec");
    debugs(44, 3, "minimum_direct_rtt = " << Config.minDirectRtt << " msec");

    if (myrtt && myrtt <= Config.minDirectRtt)
        return 1;

    myhops = netdbHostHops(psstate->request->url.host());

    debugs(44, 3, "peerCheckNetdbDirect: MY hops = " << myhops);
    debugs(44, 3, "peerCheckNetdbDirect: minimum_direct_hops = " << Config.minDirectHops);

    if (myhops && myhops <= Config.minDirectHops)
        return 1;

    p = whichPeer(psstate->closest_parent_miss);

    if (p == NULL)
        return 0;

    debugs(44, 3, "peerCheckNetdbDirect: closest_parent_miss RTT = " << psstate->ping.p_rtt << " msec");

    if (myrtt && myrtt <= psstate->ping.p_rtt)
        return 1;

#endif /* USE_ICMP */

    return 0;
}

static void
peerSelectFoo(ps_state * ps)
{
    if (peerSelectionAborted(ps))
        return;

    StoreEntry *entry = ps->entry;
    HttpRequest *request = ps->request;
    debugs(44, 3, request->method << ' ' << request->url.host());

    /** If we don't know whether DIRECT is permitted ... */
    if (ps->direct == DIRECT_UNKNOWN) {
        if (ps->always_direct == ACCESS_DUNNO) {
            debugs(44, 3, "peerSelectFoo: direct = " << DirectStr[ps->direct] << " (always_direct to be checked)");
            /** check always_direct; */
            ACLFilledChecklist *ch = new ACLFilledChecklist(Config.accessList.AlwaysDirect, request, NULL);
            ch->al = ps->al;
            ps->acl_checklist = ch;
            ps->acl_checklist->nonBlockingCheck(peerCheckAlwaysDirectDone, ps);
            return;
        } else if (ps->never_direct == ACCESS_DUNNO) {
            debugs(44, 3, "peerSelectFoo: direct = " << DirectStr[ps->direct] << " (never_direct to be checked)");
            /** check never_direct; */
            ACLFilledChecklist *ch = new ACLFilledChecklist(Config.accessList.NeverDirect, request, NULL);
            ch->al = ps->al;
            ps->acl_checklist = ch;
            ps->acl_checklist->nonBlockingCheck(peerCheckNeverDirectDone, ps);
            return;
        } else if (request->flags.noDirect) {
            /** if we are accelerating, direct is not an option. */
            ps->direct = DIRECT_NO;
            debugs(44, 3, "peerSelectFoo: direct = " << DirectStr[ps->direct] << " (forced non-direct)");
        } else if (request->flags.loopDetected) {
            /** if we are in a forwarding-loop, direct is not an option. */
            ps->direct = DIRECT_YES;
            debugs(44, 3, "peerSelectFoo: direct = " << DirectStr[ps->direct] << " (forwarding loop detected)");
        } else if (peerCheckNetdbDirect(ps)) {
            ps->direct = DIRECT_YES;
            debugs(44, 3, "peerSelectFoo: direct = " << DirectStr[ps->direct] << " (checkNetdbDirect)");
        } else {
            ps->direct = DIRECT_MAYBE;
            debugs(44, 3, "peerSelectFoo: direct = " << DirectStr[ps->direct] << " (default)");
        }

        debugs(44, 3, "peerSelectFoo: direct = " << DirectStr[ps->direct]);
    }

    if (!entry || entry->ping_status == PING_NONE)
        peerSelectPinned(ps);
    if (entry == NULL) {
        (void) 0;
    } else if (entry->ping_status == PING_NONE) {
        peerGetSomeNeighbor(ps);

        if (entry->ping_status == PING_WAITING)
            return;
    } else if (entry->ping_status == PING_WAITING) {
        peerGetSomeNeighborReplies(ps);
        entry->ping_status = PING_DONE;
    }

    switch (ps->direct) {

    case DIRECT_YES:
        peerGetSomeDirect(ps);
        break;

    case DIRECT_NO:
        peerGetSomeParent(ps);
        peerGetAllParents(ps);
        break;

    default:

        if (Config.onoff.prefer_direct)
            peerGetSomeDirect(ps);

        if (request->flags.hierarchical || !Config.onoff.nonhierarchical_direct) {
            peerGetSomeParent(ps);
            peerGetAllParents(ps);
        }

        if (!Config.onoff.prefer_direct)
            peerGetSomeDirect(ps);

        break;
    }

    // resolve the possible peers
    peerSelectDnsPaths(ps);
}

bool peerAllowedToUse(const CachePeer * p, HttpRequest * request);

/**
 * peerSelectPinned
 *
 * Selects a pinned connection.
 */
static void
peerSelectPinned(ps_state * ps)
{
    HttpRequest *request = ps->request;
    if (!request->pinnedConnection())
        return;
    CachePeer *pear = request->pinnedConnection()->pinnedPeer();
    if (Comm::IsConnOpen(request->pinnedConnection()->validatePinnedConnection(request, pear))) {
        if (pear && peerAllowedToUse(pear, request)) {
            peerAddFwdServer(&ps->servers, pear, PINNED);
            if (ps->entry)
                ps->entry->ping_status = PING_DONE;     /* Skip ICP */
        } else if (!pear && ps->direct != DIRECT_NO) {
            peerAddFwdServer(&ps->servers, NULL, PINNED);
            if (ps->entry)
                ps->entry->ping_status = PING_DONE;     /* Skip ICP */
        }
    }
}

/**
 * peerGetSomeNeighbor
 *
 * Selects a neighbor (parent or sibling) based on one of the
 * following methods:
 *      Cache Digests
 *      CARP
 *      ICMP Netdb RTT estimates
 *      ICP/HTCP queries
 */
static void
peerGetSomeNeighbor(ps_state * ps)
{
    StoreEntry *entry = ps->entry;
    HttpRequest *request = ps->request;
    CachePeer *p;
    hier_code code = HIER_NONE;
    assert(entry->ping_status == PING_NONE);

    if (ps->direct == DIRECT_YES) {
        entry->ping_status = PING_DONE;
        return;
    }

#if USE_CACHE_DIGESTS
    if ((p = neighborsDigestSelect(request))) {
        if (neighborType(p, request->url) == PEER_PARENT)
            code = CD_PARENT_HIT;
        else
            code = CD_SIBLING_HIT;
    } else
#endif
        if ((p = netdbClosestParent(request))) {
            code = CLOSEST_PARENT;
        } else if (peerSelectIcpPing(request, ps->direct, entry)) {
            debugs(44, 3, "peerSelect: Doing ICP pings");
            ps->ping.start = current_time;
            ps->ping.n_sent = neighborsUdpPing(request,
                                               entry,
                                               peerHandlePingReply,
                                               ps,
                                               &ps->ping.n_replies_expected,
                                               &ps->ping.timeout);

            if (ps->ping.n_sent == 0)
                debugs(44, DBG_CRITICAL, "WARNING: neighborsUdpPing returned 0");
            debugs(44, 3, "peerSelect: " << ps->ping.n_replies_expected <<
                   " ICP replies expected, RTT " << ps->ping.timeout <<
                   " msec");

            if (ps->ping.n_replies_expected > 0) {
                entry->ping_status = PING_WAITING;
                eventAdd("peerPingTimeout",
                         peerPingTimeout,
                         ps,
                         0.001 * ps->ping.timeout,
                         0);
                return;
            }
        }

    if (code != HIER_NONE) {
        assert(p);
        debugs(44, 3, "peerSelect: " << hier_code_str[code] << "/" << p->host);
        peerAddFwdServer(&ps->servers, p, code);
    }

    entry->ping_status = PING_DONE;
}

/*
 * peerGetSomeNeighborReplies
 *
 * Selects a neighbor (parent or sibling) based on ICP/HTCP replies.
 */
static void
peerGetSomeNeighborReplies(ps_state * ps)
{
    HttpRequest *request = ps->request;
    CachePeer *p = NULL;
    hier_code code = HIER_NONE;
    assert(ps->entry->ping_status == PING_WAITING);
    assert(ps->direct != DIRECT_YES);

    if (peerCheckNetdbDirect(ps)) {
        code = CLOSEST_DIRECT;
        debugs(44, 3, hier_code_str[code] << "/" << request->url.host());
        peerAddFwdServer(&ps->servers, NULL, code);
        return;
    }

    if ((p = ps->hit)) {
        code = ps->hit_type == PEER_PARENT ? PARENT_HIT : SIBLING_HIT;
    } else {
        if (!ps->closest_parent_miss.isAnyAddr()) {
            p = whichPeer(ps->closest_parent_miss);
            code = CLOSEST_PARENT_MISS;
        } else if (!ps->first_parent_miss.isAnyAddr()) {
            p = whichPeer(ps->first_parent_miss);
            code = FIRST_PARENT_MISS;
        }
    }
    if (p && code != HIER_NONE) {
        debugs(44, 3, hier_code_str[code] << "/" << p->host);
        peerAddFwdServer(&ps->servers, p, code);
    }
}

/*
 * peerGetSomeDirect
 *
 * Simply adds a 'direct' entry to the FwdServers list if this
 * request can be forwarded directly to the origin server
 */
static void
peerGetSomeDirect(ps_state * ps)
{
    if (ps->direct == DIRECT_NO)
        return;

    /* WAIS is not implemented natively */
    if (ps->request->url.getScheme() == AnyP::PROTO_WAIS)
        return;

    peerAddFwdServer(&ps->servers, NULL, HIER_DIRECT);
}

static void
peerGetSomeParent(ps_state * ps)
{
    CachePeer *p;
    HttpRequest *request = ps->request;
    hier_code code = HIER_NONE;
    debugs(44, 3, request->method << ' ' << request->url.host());

    if (ps->direct == DIRECT_YES)
        return;

    if ((p = peerSourceHashSelectParent(request))) {
        code = SOURCEHASH_PARENT;
#if USE_AUTH
    } else if ((p = peerUserHashSelectParent(request))) {
        code = USERHASH_PARENT;
#endif
    } else if ((p = carpSelectParent(request))) {
        code = CARP;
    } else if ((p = getRoundRobinParent(request))) {
        code = ROUNDROBIN_PARENT;
    } else if ((p = getWeightedRoundRobinParent(request))) {
        code = ROUNDROBIN_PARENT;
    } else if ((p = getFirstUpParent(request))) {
        code = FIRSTUP_PARENT;
    } else if ((p = getDefaultParent(request))) {
        code = DEFAULT_PARENT;
    }

    if (code != HIER_NONE) {
        debugs(44, 3, "peerSelect: " << hier_code_str[code] << "/" << p->host);
        peerAddFwdServer(&ps->servers, p, code);
    }
}

/* Adds alive parents. Used as a last resort for never_direct.
 */
static void
peerGetAllParents(ps_state * ps)
{
    CachePeer *p;
    HttpRequest *request = ps->request;
    /* Add all alive parents */

    for (p = Config.peers; p; p = p->next) {
        /* XXX: neighbors.c lacks a public interface for enumerating
         * parents to a request so we have to dig some here..
         */

        if (neighborType(p, request->url) != PEER_PARENT)
            continue;

        if (!peerHTTPOkay(p, request))
            continue;

        debugs(15, 3, "peerGetAllParents: adding alive parent " << p->host);

        peerAddFwdServer(&ps->servers, p, ANY_OLD_PARENT);
    }

    /* XXX: should add dead parents here, but it is currently
     * not possible to find out which parents are dead or which
     * simply are not configured to handle the request.
     */
    /* Add default parent as a last resort */
    if ((p = getDefaultParent(request))) {
        peerAddFwdServer(&ps->servers, p, DEFAULT_PARENT);
    }
}

static void
peerPingTimeout(void *data)
{
    ps_state *psstate = (ps_state *)data;
    debugs(44, 3, psstate->url());

    if (StoreEntry *entry = psstate->entry)
        entry->ping_status = PING_DONE;

    if (peerSelectionAborted(psstate))
        return;

    ++PeerStats.timeouts;
    psstate->ping.timedout = 1;
    peerSelectFoo(psstate);
}

void
peerSelectInit(void)
{
    memset(&PeerStats, '\0', sizeof(PeerStats));
}

static void
peerIcpParentMiss(CachePeer * p, icp_common_t * header, ps_state * ps)
{
    int rtt;

#if USE_ICMP
    if (Config.onoff.query_icmp) {
        if (header->flags & ICP_FLAG_SRC_RTT) {
            rtt = header->pad & 0xFFFF;
            int hops = (header->pad >> 16) & 0xFFFF;

            if (rtt > 0 && rtt < 0xFFFF)
                netdbUpdatePeer(ps->request->url, p, rtt, hops);

            if (rtt && (ps->ping.p_rtt == 0 || rtt < ps->ping.p_rtt)) {
                ps->closest_parent_miss = p->in_addr;
                ps->ping.p_rtt = rtt;
            }
        }
    }
#endif /* USE_ICMP */

    /* if closest-only is set, then don't allow FIRST_PARENT_MISS */
    if (p->options.closest_only)
        return;

    /* set FIRST_MISS if there is no CLOSEST parent */
    if (!ps->closest_parent_miss.isAnyAddr())
        return;

    rtt = (tvSubMsec(ps->ping.start, current_time) - p->basetime) / p->weight;

    if (rtt < 1)
        rtt = 1;

    if (ps->first_parent_miss.isAnyAddr() || rtt < ps->ping.w_rtt) {
        ps->first_parent_miss = p->in_addr;
        ps->ping.w_rtt = rtt;
    }
}

static void
peerHandleIcpReply(CachePeer * p, peer_t type, icp_common_t * header, void *data)
{
    ps_state *psstate = (ps_state *)data;
    icp_opcode op = header->getOpCode();
    debugs(44, 3, "peerHandleIcpReply: " << icp_opcode_str[op] << " " << psstate->url()  );
#if USE_CACHE_DIGESTS && 0
    /* do cd lookup to count false misses */

    if (p && request)
        peerNoteDigestLookup(request, p,
                             peerDigestLookup(p, request, psstate->entry));

#endif

    ++ psstate->ping.n_recv;

    if (op == ICP_MISS || op == ICP_DECHO) {
        if (type == PEER_PARENT)
            peerIcpParentMiss(p, header, psstate);
    } else if (op == ICP_HIT) {
        psstate->hit = p;
        psstate->hit_type = type;
        peerSelectFoo(psstate);
        return;
    }

    if (psstate->ping.n_recv < psstate->ping.n_replies_expected)
        return;

    peerSelectFoo(psstate);
}

#if USE_HTCP
static void
peerHandleHtcpReply(CachePeer * p, peer_t type, HtcpReplyData * htcp, void *data)
{
    ps_state *psstate = (ps_state *)data;
    debugs(44, 3, "" << (htcp->hit ? "HIT" : "MISS") << " " << psstate->url());
    ++ psstate->ping.n_recv;

    if (htcp->hit) {
        psstate->hit = p;
        psstate->hit_type = type;
        peerSelectFoo(psstate);
        return;
    }

    if (type == PEER_PARENT)
        peerHtcpParentMiss(p, htcp, psstate);

    if (psstate->ping.n_recv < psstate->ping.n_replies_expected)
        return;

    peerSelectFoo(psstate);
}

static void
peerHtcpParentMiss(CachePeer * p, HtcpReplyData * htcp, ps_state * ps)
{
    int rtt;

#if USE_ICMP
    if (Config.onoff.query_icmp) {
        if (htcp->cto.rtt > 0) {
            rtt = (int) htcp->cto.rtt * 1000;
            int hops = (int) htcp->cto.hops * 1000;
            netdbUpdatePeer(ps->request->url, p, rtt, hops);

            if (rtt && (ps->ping.p_rtt == 0 || rtt < ps->ping.p_rtt)) {
                ps->closest_parent_miss = p->in_addr;
                ps->ping.p_rtt = rtt;
            }
        }
    }
#endif /* USE_ICMP */

    /* if closest-only is set, then don't allow FIRST_PARENT_MISS */
    if (p->options.closest_only)
        return;

    /* set FIRST_MISS if there is no CLOSEST parent */
    if (!ps->closest_parent_miss.isAnyAddr())
        return;

    rtt = (tvSubMsec(ps->ping.start, current_time) - p->basetime) / p->weight;

    if (rtt < 1)
        rtt = 1;

    if (ps->first_parent_miss.isAnyAddr() || rtt < ps->ping.w_rtt) {
        ps->first_parent_miss = p->in_addr;
        ps->ping.w_rtt = rtt;
    }
}

#endif

static void
peerHandlePingReply(CachePeer * p, peer_t type, AnyP::ProtocolType proto, void *pingdata, void *data)
{
    if (proto == AnyP::PROTO_ICP)
        peerHandleIcpReply(p, type, (icp_common_t *)pingdata, data);

#if USE_HTCP

    else if (proto == AnyP::PROTO_HTCP)
        peerHandleHtcpReply(p, type, (HtcpReplyData *)pingdata, data);

#endif

    else
        debugs(44, DBG_IMPORTANT, "peerHandlePingReply: unknown protocol " << proto);
}

static void
peerAddFwdServer(FwdServer ** FSVR, CachePeer * p, hier_code code)
{
    debugs(44, 5, "peerAddFwdServer: adding " <<
           (p ? p->host : "DIRECT")  << " " <<
           hier_code_str[code]  );
    FwdServer *fs = new FwdServer(p, code);

    while (*FSVR)
        FSVR = &(*FSVR)->next;

    *FSVR = fs;
}

ps_state::ps_state(PeerSelectionInitiator *initiator):
    request(nullptr),
    entry (NULL),
    always_direct(Config.accessList.AlwaysDirect?ACCESS_DUNNO:ACCESS_DENIED),
    never_direct(Config.accessList.NeverDirect?ACCESS_DUNNO:ACCESS_DENIED),
    direct(DIRECT_UNKNOWN),
    lastError(NULL),
    servers (NULL),
    first_parent_miss(),
    closest_parent_miss(),
    hit(NULL),
    hit_type(PEER_NONE),
    acl_checklist (NULL),
    initiator_(initiator)
{
    ; // no local defaults.
}

const SBuf
ps_state::url() const
{
    if (entry)
        return SBuf(entry->url());

    if (request)
        return request->effectiveRequestUri();

    static const SBuf noUrl("[no URL]");
    return noUrl;
}

/// \returns valid/interested peer initiator or nil
PeerSelectionInitiator *
ps_state::interestedInitiator()
{
    const auto initiator = initiator_.valid();

    if (!initiator) {
        debugs(44, 3, id << " initiator gone");
        return nullptr;
    }

    if (!initiator->subscribed) {
        debugs(44, 3, id << " initiator lost interest");
        return nullptr;
    }

    debugs(44, 7, id);
    return initiator;
}

bool
ps_state::wantsMoreDestinations() const {
    const auto maxCount = Config.forward_max_tries;
    return maxCount >= 0 && foundPaths <
           static_cast<std::make_unsigned<decltype(maxCount)>::type>(maxCount);
}

void
ps_state::handlePath(Comm::ConnectionPointer &path, FwdServer &fs)
{
    ++foundPaths;

    path->peerType = fs.code;
    path->setPeer(fs._peer.get());

    // check for a configured outgoing address for this destination...
    getOutgoingAddress(request, path);

    request->hier.ping = ping; // may be updated later

    debugs(44, 2, id << " found " << path << ", destination #" << foundPaths << " for " << url());
    debugs(44, 2, "  always_direct = " << always_direct);
    debugs(44, 2, "   never_direct = " << never_direct);
    debugs(44, 2, "       timedout = " << ping.timedout);

    if (const auto initiator = interestedInitiator())
        initiator->noteDestination(path);
}

InstanceIdDefinitions(ps_state, "PeerSelector");

ping_data::ping_data() :
    n_sent(0),
    n_recv(0),
    n_replies_expected(0),
    timeout(0),
    timedout(0),
    w_rtt(0),
    p_rtt(0)
{
    start.tv_sec = 0;
    start.tv_usec = 0;
    stop.tv_sec = 0;
    stop.tv_usec = 0;
}

