/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "acl/FilledChecklist.h"
#include "acl/Gadgets.h"
#include "base/TextException.h"
#include "clients/Client.h"
#include "comm/Connection.h"
#include "comm/forward.h"
#include "comm/Write.h"
#include "error/Detail.h"
#include "errorpage.h"
#include "fd.h"
#include "HttpHdrContRange.h"
#include "HttpReply.h"
#include "HttpRequest.h"
#include "SquidConfig.h"
#include "StatCounters.h"
#include "Store.h"
#include "tools.h"

#if USE_ADAPTATION
#include "adaptation/AccessCheck.h"
#include "adaptation/Answer.h"
#include "adaptation/Iterator.h"
#include "base/AsyncCall.h"
#endif

// implemented in client_side_reply.cc until sides have a common parent
void purgeEntriesByUrl(HttpRequest * req, const char *url);

Client::Client(FwdState *theFwdState) :
    AsyncJob("Client"),
    fwd(theFwdState),
    request(fwd->request)
{
    entry = fwd->entry;
    entry->lock("Client");
}

Client::~Client()
{
    // paranoid: check that swanSong has been called
    assert(!requestBodySource);
#if USE_ADAPTATION
    assert(!virginBodyDestination);
    assert(!adaptedBodySource);
#endif

    entry->unlock("Client");

    HTTPMSGUNLOCK(theVirginReply);
    HTTPMSGUNLOCK(theFinalReply);

    if (responseBodyBuffer != nullptr) {
        delete responseBodyBuffer;
        responseBodyBuffer = nullptr;
    }
}

void
Client::swanSong()
{
    // get rid of our piping obligations
    if (requestBodySource != nullptr)
        stopConsumingFrom(requestBodySource);

#if USE_ADAPTATION
    cleanAdaptation();
#endif

    if (!doneWithServer())
        closeServer();

    if (!doneWithFwd) {
        doneWithFwd = "swanSong()";
        fwd->handleUnregisteredServerEnd();
    }

    BodyConsumer::swanSong();
#if USE_ADAPTATION
    Initiator::swanSong();
    BodyProducer::swanSong();
#endif

    // paranoid: check that swanSong has been called
    // extra paranoid: yeah, I really mean it. they MUST pass here.
    assert(!requestBodySource);
#if USE_ADAPTATION
    assert(!virginBodyDestination);
    assert(!adaptedBodySource);
#endif
}

HttpReply *
Client::virginReply()
{
    assert(theVirginReply);
    return theVirginReply;
}

const HttpReply *
Client::virginReply() const
{
    assert(theVirginReply);
    return theVirginReply;
}

HttpReply *
Client::setVirginReply(HttpReply *rep)
{
    debugs(11,5, this << " setting virgin reply to " << rep);
    assert(!theVirginReply);
    assert(rep);
    theVirginReply = rep;
    HTTPMSGLOCK(theVirginReply);
    if (fwd->al)
        fwd->al->reply = theVirginReply;
    return theVirginReply;
}

HttpReply *
Client::finalReply()
{
    assert(theFinalReply);
    return theFinalReply;
}

HttpReply *
Client::setFinalReply(HttpReply *rep)
{
    debugs(11,5, this << " setting final reply to " << rep);

    assert(!theFinalReply);
    assert(rep);
    theFinalReply = rep;
    HTTPMSGLOCK(theFinalReply);
    if (fwd->al)
        fwd->al->reply = theFinalReply;

    // give entry the reply because haveParsedReplyHeaders() expects it there
    entry->replaceHttpReply(theFinalReply, false); // but do not write yet
    haveParsedReplyHeaders(); // update the entry/reply (e.g., set timestamps)
    if (!EBIT_TEST(entry->flags, RELEASE_REQUEST) && blockCaching())
        entry->release();
    entry->startWriting(); // write the updated entry to store

    return theFinalReply;
}

void
Client::markParsedVirginReplyAsWhole(const char *reasonWeAreSure)
{
    assert(reasonWeAreSure);
    debugs(11, 3, reasonWeAreSure);
    markedParsedVirginReplyAsWhole = reasonWeAreSure;
}

// called when no more server communication is expected; may quit
void
Client::serverComplete()
{
    debugs(11,5, "serverComplete " << this);

    if (!doneWithServer()) {
        closeServer();
        assert(doneWithServer());
    }

    completed = true;

    if (requestBodySource != nullptr)
        stopConsumingFrom(requestBodySource);

    if (responseBodyBuffer != nullptr)
        return;

    serverComplete2();
}

void
Client::serverComplete2()
{
    debugs(11,5, "serverComplete2 " << this);

#if USE_ADAPTATION
    if (virginBodyDestination != nullptr)
        stopProducingFor(virginBodyDestination, true);

    if (!doneWithAdaptation())
        return;
#endif

    completeForwarding();
}

bool Client::doneAll() const
{
    return  doneWithServer() &&
#if USE_ADAPTATION
            doneWithAdaptation() &&
            Adaptation::Initiator::doneAll() &&
            BodyProducer::doneAll() &&
#endif
            BodyConsumer::doneAll();
}

// FTP side overloads this to work around multiple calls to fwd->complete
void
Client::completeForwarding()
{
    debugs(11,5, "completing forwarding for "  << fwd);
    assert(fwd != nullptr);

    auto storedWholeReply = markedParsedVirginReplyAsWhole;
#if USE_ADAPTATION
    // This precondition is necessary for its two implications:
    // * We cannot be waiting to decide whether to adapt this response. Thus,
    //   the startedAdaptation check below correctly detects all adaptation
    //   cases (i.e. it does not miss adaptationAccessCheckPending ones).
    // * We cannot be waiting to consume/store received adapted response bytes.
    //   Thus, receivedWholeAdaptedReply implies that we stored everything.
    Assure(doneWithAdaptation());

    if (startedAdaptation)
        storedWholeReply = receivedWholeAdaptedReply ? "receivedWholeAdaptedReply" : nullptr;
#endif

    if (storedWholeReply)
        fwd->markStoredReplyAsWhole(storedWholeReply);

    doneWithFwd = "completeForwarding()";
    fwd->complete();
}

// Register to receive request body
bool Client::startRequestBodyFlow()
{
    HttpRequestPointer r(originalRequest());
    assert(r->body_pipe != nullptr);
    requestBodySource = r->body_pipe;
    if (requestBodySource->setConsumerIfNotLate(this)) {
        debugs(11,3, "expecting request body from " <<
               requestBodySource->status());
        return true;
    }

    debugs(11,3, "aborting on partially consumed request body: " <<
           requestBodySource->status());
    requestBodySource = nullptr;
    return false;
}

// Entry-dependent callbacks use this check to quit if the entry went bad
bool
Client::abortOnBadEntry(const char *abortReason)
{
    if (entry->isAccepting())
        return false;

    debugs(11,5, "entry is not Accepting!");
    abortOnData(abortReason);
    return true;
}

// more request or adapted response body is available
void
Client::noteMoreBodyDataAvailable(BodyPipe::Pointer bp)
{
#if USE_ADAPTATION
    if (adaptedBodySource == bp) {
        handleMoreAdaptedBodyAvailable();
        return;
    }
#endif
    if (requestBodySource == bp)
        handleMoreRequestBodyAvailable();
}

// the entire request or adapted response body was provided, successfully
void
Client::noteBodyProductionEnded(BodyPipe::Pointer bp)
{
#if USE_ADAPTATION
    if (adaptedBodySource == bp) {
        handleAdaptedBodyProductionEnded();
        return;
    }
#endif
    if (requestBodySource == bp)
        handleRequestBodyProductionEnded();
}

// premature end of the request or adapted response body production
void
Client::noteBodyProducerAborted(BodyPipe::Pointer bp)
{
#if USE_ADAPTATION
    if (adaptedBodySource == bp) {
        handleAdaptedBodyProducerAborted();
        return;
    }
#endif
    if (requestBodySource == bp)
        handleRequestBodyProducerAborted();
}

bool
Client::abortOnData(const char *reason)
{
    abortAll(reason);
    return true;
}

// more origin request body data is available
void
Client::handleMoreRequestBodyAvailable()
{
    if (!requestSender)
        sendMoreRequestBody();
    else
        debugs(9,3, "waiting for request body write to complete");
}

// there will be no more handleMoreRequestBodyAvailable calls
void
Client::handleRequestBodyProductionEnded()
{
    receivedWholeRequestBody = true;
    if (!requestSender)
        doneSendingRequestBody();
    else
        debugs(9,3, "waiting for request body write to complete");
}

// called when we are done sending request body; kids extend this
void
Client::doneSendingRequestBody()
{
    debugs(9,3, "done sending request body");
    assert(requestBodySource != nullptr);
    stopConsumingFrom(requestBodySource);

    // kids extend this
}

// called when body producers aborts; kids extend this
void
Client::handleRequestBodyProducerAborted()
{
    if (requestSender != nullptr)
        debugs(9,3, "fyi: request body aborted while we were sending");

    fwd->dontRetry(true); // the problem is not with the server
    stopConsumingFrom(requestBodySource); // requestSender, if any, will notice

    // kids extend this
}

// called when we wrote request headers(!) or a part of the body
void
Client::sentRequestBody(const CommIoCbParams &io)
{
    debugs(11, 5, "sentRequestBody: FD " << io.fd << ": size " << io.size << ": errflag " << io.flag << ".");
    debugs(32,3, "sentRequestBody called");

    requestSender = nullptr;

    if (io.size > 0) {
        fd_bytes(io.fd, io.size, IoDirection::Write);
        statCounter.server.all.kbytes_out += io.size;
        // kids should increment their counters
    }

    if (io.flag == Comm::ERR_CLOSING)
        return;

    if (!requestBodySource) {
        debugs(9,3, "detected while-we-were-sending abort");
        return; // do nothing;
    }

    // both successful and failed writes affect response times
    request->hier.notePeerWrite();

    if (io.flag) {
        debugs(11, DBG_IMPORTANT, "ERROR: sentRequestBody failure: FD " << io.fd << ": " << xstrerr(io.xerrno));
        ErrorState *err;
        err = new ErrorState(ERR_WRITE_ERROR, Http::scBadGateway, fwd->request, fwd->al);
        err->xerrno = io.xerrno;
        fwd->fail(err);
        abortOnData("I/O error while sending request body");
        return;
    }

    if (EBIT_TEST(entry->flags, ENTRY_ABORTED)) {
        abortOnData("store entry aborted while sending request body");
        return;
    }

    if (!requestBodySource->exhausted())
        sendMoreRequestBody();
    else if (receivedWholeRequestBody)
        doneSendingRequestBody();
    else
        debugs(9,3, "waiting for body production end or abort");
}

void
Client::sendMoreRequestBody()
{
    assert(requestBodySource != nullptr);
    assert(!requestSender);

    const Comm::ConnectionPointer conn = dataConnection();

    if (!Comm::IsConnOpen(conn)) {
        debugs(9,3, "cannot send request body to closing " << conn);
        return; // wait for the kid's close handler; TODO: assert(closer);
    }

    MemBuf buf;
    if (getMoreRequestBody(buf) && buf.contentSize() > 0) {
        debugs(9,3, "will write " << buf.contentSize() << " request body bytes");
        typedef CommCbMemFunT<Client, CommIoCbParams> Dialer;
        requestSender = JobCallback(93,3, Dialer, this, Client::sentRequestBody);
        Comm::Write(conn, &buf, requestSender);
    } else {
        debugs(9,3, "will wait for more request body bytes or eof");
        requestSender = nullptr;
    }
}

/// either fill buf with available [encoded] request body bytes or return false
bool
Client::getMoreRequestBody(MemBuf &buf)
{
    // default implementation does not encode request body content
    Must(requestBodySource != nullptr);
    return requestBodySource->getMoreData(buf);
}

// Compares hosts in urls, returns false if different, no sheme, or no host.
static bool
sameUrlHosts(const char *url1, const char *url2)
{
    // XXX: Want AnyP::Uri::parse() here, but it uses static storage and copying
    const char *host1 = strchr(url1, ':');
    const char *host2 = strchr(url2, ':');

    if (host1 && host2) {
        // skip scheme slashes
        do {
            ++host1;
            ++host2;
        } while (*host1 == '/' && *host2 == '/');

        if (!*host1)
            return false; // no host

        // increment while the same until we reach the end of the URL/host
        while (*host1 && *host1 != '/' && *host1 == *host2) {
            ++host1;
            ++host2;
        }
        return *host1 == *host2;
    }

    return false; // no URL scheme
}

// purges entries that match the value of a given HTTP [response] header
static void
purgeEntriesByHeader(HttpRequest *req, const char *reqUrl, Http::Message *rep, Http::HdrType hdr)
{
    const auto hdrUrl = rep->header.getStr(hdr);
    if (!hdrUrl)
        return;

    /*
     * If the URL is relative, make it absolute so we can find it.
     * If it's absolute, make sure the host parts match to avoid DOS attacks
     * as per RFC 2616 13.10.
     */
    SBuf absUrlMaker;
    const char *absUrl = nullptr;
    if (urlIsRelative(hdrUrl)) {
        if (req->method.id() == Http::METHOD_CONNECT)
            absUrl = hdrUrl; // TODO: merge authority-uri and hdrUrl
        else if (req->url.getScheme() == AnyP::PROTO_URN)
            absUrl = req->url.absolute().c_str();
        else {
            AnyP::Uri tmpUrl = req->url;
            if (*hdrUrl == '/') {
                // RFC 3986 section 4.2: absolute-path reference
                // for this logic replace the entire request-target URI path
                tmpUrl.path(hdrUrl);
            } else {
                tmpUrl.addRelativePath(reqUrl);
            }
            absUrlMaker = tmpUrl.absolute();
            absUrl = absUrlMaker.c_str();
        }
    } else if (!sameUrlHosts(reqUrl, hdrUrl)) {
        return;
    } else
        absUrl = hdrUrl;

    purgeEntriesByUrl(req, absUrl);
}

// some HTTP methods should purge matching cache entries
void
Client::maybePurgeOthers()
{
    // only some HTTP methods should purge matching cache entries
    if (!request->method.purgesOthers())
        return;

    // and probably only if the response was successful
    if (theFinalReply->sline.status() >= 400)
        return;

    // XXX: should we use originalRequest() here?
    SBuf tmp(request->effectiveRequestUri());
    const char *reqUrl = tmp.c_str();
    debugs(88, 5, "maybe purging due to " << request->method << ' ' << tmp);
    purgeEntriesByUrl(request.getRaw(), reqUrl);
    purgeEntriesByHeader(request.getRaw(), reqUrl, theFinalReply, Http::HdrType::LOCATION);
    purgeEntriesByHeader(request.getRaw(), reqUrl, theFinalReply, Http::HdrType::CONTENT_LOCATION);
}

/// called when we have final (possibly adapted) reply headers; kids extend
void
Client::haveParsedReplyHeaders()
{
    Must(theFinalReply);
    maybePurgeOthers();

    // adaptation may overwrite old offset computed using the virgin response
    currentOffset = 0;
    if (const auto cr = theFinalReply->contentRange()) {
        if (cr->spec.offset != HttpHdrRangeSpec::UnknownPosition)
            currentOffset = cr->spec.offset;
    }
}

/// whether to prevent caching of an otherwise cachable response
bool
Client::blockCaching()
{
    if (const auto acl = Config.accessList.storeMiss) {
        // This relatively expensive check is not in StoreEntry::checkCachable:
        // That method lacks HttpRequest and may be called too many times.
        ACLFilledChecklist ch(acl, originalRequest().getRaw());
        ch.updateAle(fwd->al);
        ch.updateReply(&entry->mem().freshestReply());
        if (!ch.fastCheck().allowed()) { // when in doubt, block
            debugs(20, 3, "store_miss prohibits caching");
            return true;
        }
    }
    return false;
}

HttpRequestPointer
Client::originalRequest()
{
    return request;
}

#if USE_ADAPTATION
/// Initiate an asynchronous adaptation transaction which will call us back.
void
Client::startAdaptation(const Adaptation::ServiceGroupPointer &group, HttpRequest *cause)
{
    debugs(11, 5, "Client::startAdaptation() called");
    // check whether we should be sending a body as well
    // start body pipe to feed ICAP transaction if needed
    assert(!virginBodyDestination);
    HttpReply *vrep = virginReply();
    assert(!vrep->body_pipe);
    int64_t size = 0;
    if (vrep->expectingBody(cause->method, size) && size) {
        virginBodyDestination = new BodyPipe(this);
        vrep->body_pipe = virginBodyDestination;
        debugs(93, 6, "will send virgin reply body to " <<
               virginBodyDestination << "; size: " << size);
        if (size > 0)
            virginBodyDestination->setBodySize(size);
    }

    adaptedHeadSource = initiateAdaptation(
                            new Adaptation::Iterator(vrep, cause, fwd->al, group));
    startedAdaptation = initiated(adaptedHeadSource);
    Must(startedAdaptation);
}

// properly cleans up ICAP-related state
// may be called multiple times
void Client::cleanAdaptation()
{
    debugs(11,5, "cleaning ICAP; ACL: " << adaptationAccessCheckPending);

    if (virginBodyDestination != nullptr)
        stopProducingFor(virginBodyDestination, false);

    announceInitiatorAbort(adaptedHeadSource);

    if (adaptedBodySource != nullptr)
        stopConsumingFrom(adaptedBodySource);

    if (!adaptationAccessCheckPending) // we cannot cancel a pending callback
        assert(doneWithAdaptation()); // make sure the two methods are in sync
}

bool
Client::doneWithAdaptation() const
{
    return !adaptationAccessCheckPending &&
           !virginBodyDestination && !adaptedHeadSource && !adaptedBodySource;
}

// sends virgin reply body to ICAP, buffering excesses if needed
void
Client::adaptVirginReplyBody(const char *data, ssize_t len)
{
    assert(startedAdaptation);

    if (!virginBodyDestination) {
        debugs(11,3, "ICAP does not want more virgin body");
        return;
    }

    // grow overflow area if already overflowed
    if (responseBodyBuffer) {
        responseBodyBuffer->append(data, len);
        data = responseBodyBuffer->content();
        len = responseBodyBuffer->contentSize();
    }

    const ssize_t putSize = virginBodyDestination->putMoreData(data, len);
    data += putSize;
    len -= putSize;

    // if we had overflow area, shrink it as necessary
    if (responseBodyBuffer) {
        if (putSize == responseBodyBuffer->contentSize()) {
            delete responseBodyBuffer;
            responseBodyBuffer = nullptr;
        } else {
            responseBodyBuffer->consume(putSize);
        }
        return;
    }

    // if we did not have an overflow area, create it as needed
    if (len > 0) {
        assert(!responseBodyBuffer);
        responseBodyBuffer = new MemBuf;
        responseBodyBuffer->init(4096, SQUID_TCP_SO_RCVBUF * 10);
        responseBodyBuffer->append(data, len);
    }
}

// can supply more virgin response body data
void
Client::noteMoreBodySpaceAvailable(BodyPipe::Pointer)
{
    if (responseBodyBuffer) {
        addVirginReplyBody(nullptr, 0); // kick the buffered fragment alive again
        if (completed && !responseBodyBuffer) {
            serverComplete2();
            return;
        }
    }
    maybeReadVirginBody();
}

// the consumer of our virgin response body aborted
void
Client::noteBodyConsumerAborted(BodyPipe::Pointer)
{
    stopProducingFor(virginBodyDestination, false);

    // do not force closeServer here in case we need to bypass AdaptationQueryAbort

    if (doneWithAdaptation()) // we may still be receiving adapted response
        handleAdaptationCompleted();
}

// received adapted response headers (body may follow)
void
Client::noteAdaptationAnswer(const Adaptation::Answer &answer)
{
    clearAdaptation(adaptedHeadSource); // we do not expect more messages

    switch (answer.kind) {
    case Adaptation::Answer::akForward:
        handleAdaptedHeader(const_cast<Http::Message*>(answer.message.getRaw()));
        break;

    case Adaptation::Answer::akBlock:
        handleAdaptationBlocked(answer);
        break;

    case Adaptation::Answer::akError:
        handleAdaptationAborted(!answer.final);
        break;
    }
}

void
Client::handleAdaptedHeader(Http::Message *msg)
{
    if (abortOnBadEntry("entry went bad while waiting for adapted headers")) {
        // If the adapted response has a body, the ICAP side needs to know
        // that nobody will consume that body. We will be destroyed upon
        // return. Tell the ICAP side that it is on its own.
        HttpReply *rep = dynamic_cast<HttpReply*>(msg);
        assert(rep);
        if (rep->body_pipe != nullptr)
            rep->body_pipe->expectNoConsumption();

        return;
    }

    HttpReply *rep = dynamic_cast<HttpReply*>(msg);
    assert(rep);
    debugs(11,5, this << " setting adapted reply to " << rep);
    setFinalReply(rep);

    assert(!adaptedBodySource);
    if (rep->body_pipe != nullptr) {
        // subscribe to receive adapted body
        adaptedBodySource = rep->body_pipe;
        // assume that ICAP does not auto-consume on failures
        const bool result = adaptedBodySource->setConsumerIfNotLate(this);
        assert(result);
        checkAdaptationWithBodyCompletion();
    } else {
        // no body
        Assure(!adaptedReplyAborted);
        receivedWholeAdaptedReply = true;
        if (doneWithAdaptation()) // we may still be sending virgin response
            handleAdaptationCompleted();
    }
}

void
Client::resumeBodyStorage()
{
    if (abortOnBadEntry("store entry aborted while kick producer callback"))
        return;

    if (!adaptedBodySource)
        return;

    handleMoreAdaptedBodyAvailable();

    checkAdaptationWithBodyCompletion();
}

// more adapted response body is available
void
Client::handleMoreAdaptedBodyAvailable()
{
    if (abortOnBadEntry("entry refuses adapted body"))
        return;

    assert(entry);

    size_t contentSize = adaptedBodySource->buf().contentSize();

    if (!contentSize)
        return; // XXX: bytesWanted asserts on zero-size ranges

    const size_t spaceAvailable = entry->bytesWanted(Range<size_t>(0, contentSize), true);

    if (spaceAvailable < contentSize ) {
        // No or partial body data consuming
        typedef NullaryMemFunT<Client> Dialer;
        AsyncCall::Pointer call = asyncCall(93, 5, "Client::resumeBodyStorage",
                                            Dialer(this, &Client::resumeBodyStorage));
        entry->deferProducer(call);
    }

    if (!spaceAvailable)  {
        debugs(11, 5, "NOT storing " << contentSize << " bytes of adapted " <<
               "response body at offset " << adaptedBodySource->consumedSize());
        return;
    }

    if (spaceAvailable < contentSize ) {
        debugs(11, 5, "postponing storage of " <<
               (contentSize - spaceAvailable) << " body bytes");
        contentSize = spaceAvailable;
    }

    debugs(11,5, "storing " << contentSize << " bytes of adapted " <<
           "response body at offset " << adaptedBodySource->consumedSize());

    BodyPipeCheckout bpc(*adaptedBodySource);
    const StoreIOBuffer ioBuf(&bpc.buf, currentOffset, contentSize);
    currentOffset += ioBuf.length;
    entry->write(ioBuf);
    bpc.buf.consume(contentSize);
    bpc.checkIn();
}

// the entire adapted response body was produced, successfully
void
Client::handleAdaptedBodyProductionEnded()
{
    if (abortOnBadEntry("entry went bad while waiting for adapted body eof"))
        return;

    Assure(!adaptedReplyAborted);
    receivedWholeAdaptedReply = true;

    checkAdaptationWithBodyCompletion();
}

void
Client::checkAdaptationWithBodyCompletion()
{
    if (!adaptedBodySource) {
        debugs(11, 7, "not consuming; " << startedAdaptation);
        return;
    }

    if (!receivedWholeAdaptedReply && !adaptedReplyAborted) {
        // wait for noteBodyProductionEnded() or noteBodyProducerAborted()
        // because completeForwarding() needs to know whether we receivedWholeAdaptedReply
        debugs(11, 7, "waiting for adapted body production ending");
        return;
    }

    if (!adaptedBodySource->exhausted()) {
        debugs(11, 5, "waiting to consume the remainder of the adapted body from " << adaptedBodySource->status());
        return; // resumeBodyStorage() should eventually consume the rest
    }

    stopConsumingFrom(adaptedBodySource);

    if (doneWithAdaptation()) // we may still be sending virgin response
        handleAdaptationCompleted();
}

// premature end of the adapted response body
void Client::handleAdaptedBodyProducerAborted()
{
    if (abortOnBadEntry("entry went bad while waiting for the now-aborted adapted body"))
        return;

    Assure(!receivedWholeAdaptedReply);
    adaptedReplyAborted = true;
    Must(adaptedBodySource != nullptr);
    if (!adaptedBodySource->exhausted()) {
        debugs(11,5, "waiting to consume the remainder of the aborted adapted body");
        return; // resumeBodyStorage() should eventually consume the rest
    }

    if (handledEarlyAdaptationAbort())
        return;

    checkAdaptationWithBodyCompletion(); // the user should get a truncated response
}

// common part of noteAdaptationAnswer and handleAdaptedBodyProductionEnded
void
Client::handleAdaptationCompleted()
{
    debugs(11,5, "handleAdaptationCompleted");
    cleanAdaptation();

    // We stop reading origin response because we have no place to put it(*) and
    // cannot use it. If some origin servers do not like that or if we want to
    // reuse more pconns, we can add code to discard unneeded origin responses.
    // (*) TODO: Is it possible that the adaptation xaction is still running?
    if (mayReadVirginReplyBody()) {
        debugs(11,3, "closing origin conn due to ICAP completion");
        closeServer();
    }

    completeForwarding();
}

// common part of noteAdaptation*Aborted and noteBodyConsumerAborted methods
void
Client::handleAdaptationAborted(bool bypassable)
{
    debugs(11,5, "handleAdaptationAborted; bypassable: " << bypassable <<
           ", entry empty: " << entry->isEmpty());

    if (abortOnBadEntry("entry went bad while ICAP aborted"))
        return;

    // TODO: bypass if possible
    if (!handledEarlyAdaptationAbort())
        abortAll("adaptation failure with a filled entry");
}

/// If the store entry is still empty, fully handles adaptation abort, returning
/// true. Otherwise just updates the request error detail and returns false.
bool
Client::handledEarlyAdaptationAbort()
{
    if (entry->isEmpty()) {
        debugs(11,8, "adaptation failure with an empty entry: " << *entry);
        const auto err = new ErrorState(ERR_ICAP_FAILURE, Http::scInternalServerError, request.getRaw(), fwd->al);
        static const auto d = MakeNamedErrorDetail("ICAP_RESPMOD_EARLY");
        err->detailError(d);
        fwd->fail(err);
        fwd->dontRetry(true);
        abortAll("adaptation failure with an empty entry");
        return true; // handled
    }

    if (request) { // update logged info directly
        static const auto d = MakeNamedErrorDetail("ICAP_RESPMOD_LATE");
        request->detailError(ERR_ICAP_FAILURE, d);
    }

    return false; // the caller must handle
}

// adaptation service wants us to deny HTTP client access to this response
void
Client::handleAdaptationBlocked(const Adaptation::Answer &answer)
{
    const auto blockedAnswer = answer.blockedToChecklistAnswer();

    debugs(11,5, blockedAnswer.lastCheckDescription());

    if (abortOnBadEntry("entry went bad while ICAP aborted"))
        return;

    if (!entry->isEmpty()) { // too late to block (should not really happen)
        if (request) {
            static const auto d = MakeNamedErrorDetail("RESPMOD_BLOCK_LATE");
            request->detailError(ERR_ICAP_FAILURE, d);
        }
        abortAll("late adaptation block");
        return;
    }

    debugs(11,7, "creating adaptation block response");

    auto page_id = FindDenyInfoPage(blockedAnswer, true);
    if (page_id == ERR_NONE)
        page_id = ERR_ACCESS_DENIED;

    const auto err = new ErrorState(page_id, Http::scForbidden, request.getRaw(), fwd->al);
    static const auto d = MakeNamedErrorDetail("RESPMOD_BLOCK_EARLY");
    err->detailError(d);
    fwd->fail(err);
    fwd->dontRetry(true);

    abortOnData("timely adaptation block");
}

void
Client::noteAdaptationAclCheckDone(Adaptation::ServiceGroupPointer group)
{
    adaptationAccessCheckPending = false;

    if (abortOnBadEntry("entry went bad while waiting for ICAP ACL check"))
        return;

    // TODO: Should non-ICAP and ICAP REPMOD pre-cache paths check this?
    // That check now only happens on REQMOD pre-cache and REPMOD post-cache, in processReplyAccess().
    if (virginReply()->expectedBodyTooLarge(*request)) {
        sendBodyIsTooLargeError();
        return;
    }
    // TODO: Should we check receivedBodyTooLarge as well?

    if (!group) {
        debugs(11,3, "no adapation needed");
        setFinalReply(virginReply());
        processReplyBody();
        return;
    }

    startAdaptation(group, originalRequest().getRaw());
    processReplyBody();
}
#endif

void
Client::sendBodyIsTooLargeError()
{
    const auto err = new ErrorState(ERR_TOO_BIG, Http::scForbidden, request.getRaw(), fwd->al);
    fwd->fail(err);
    fwd->dontRetry(true);
    abortOnData("Virgin body too large.");
}

// TODO: when HttpStateData sends all errors to ICAP,
// we should be able to move this at the end of setVirginReply().
void
Client::adaptOrFinalizeReply()
{
#if USE_ADAPTATION
    // TODO: merge with client side and return void to hide the on/off logic?
    // The callback can be called with a NULL service if adaptation is off.
    adaptationAccessCheckPending = Adaptation::AccessCheck::Start(
                                       Adaptation::methodRespmod, Adaptation::pointPreCache,
                                       originalRequest().getRaw(), virginReply(), fwd->al, this);
    debugs(11,5, "adaptationAccessCheckPending=" << adaptationAccessCheckPending);
    if (adaptationAccessCheckPending)
        return;
#endif

    setFinalReply(virginReply());
}

/// initializes bodyBytesRead stats if needed and applies delta
void
Client::adjustBodyBytesRead(const int64_t delta)
{
    int64_t &bodyBytesRead = originalRequest()->hier.bodyBytesRead;

    // if we got here, do not log a dash even if we got nothing from the server
    if (bodyBytesRead < 0)
        bodyBytesRead = 0;

    bodyBytesRead += delta; // supports negative and zero deltas

    // check for overflows ("infinite" response?) and underflows (a bug)
    Must(bodyBytesRead >= 0);
}

void
Client::delayRead()
{
    Assure(!waitingForDelayAwareReadChance);
    waitingForDelayAwareReadChance = true;

    using DeferredReadDialer = NullaryMemFunT<Client>;
    AsyncCall::Pointer call = asyncCall(11, 5, "Client::noteDelayAwareReadChance",
                                        DeferredReadDialer(this, &Client::noteDelayAwareReadChance));
    entry->mem().delayRead(call);
}

void
Client::addVirginReplyBody(const char *data, ssize_t len)
{
    adjustBodyBytesRead(len);

#if USE_ADAPTATION
    assert(!adaptationAccessCheckPending); // or would need to buffer while waiting
    if (startedAdaptation) {
        adaptVirginReplyBody(data, len);
        return;
    }
#endif
    storeReplyBody(data, len);
}

// writes virgin or adapted reply body to store
void
Client::storeReplyBody(const char *data, ssize_t len)
{
    // write even if len is zero to push headers towards the client side
    entry->write (StoreIOBuffer(len, currentOffset, (char*)data));

    currentOffset += len;
}

size_t
Client::calcBufferSpaceToReserve(size_t space, const size_t wantSpace) const
{
    if (space < wantSpace) {
        const size_t maxSpace = SBuf::maxSize; // absolute best
        space = min(wantSpace, maxSpace); // do not promise more than asked
    }

#if USE_ADAPTATION
    if (responseBodyBuffer) {
        return 0;   // Stop reading if already overflowed waiting for ICAP to catch up
    }

    if (virginBodyDestination != nullptr) {
        /*
         * BodyPipe buffer has a finite size limit.  We
         * should not read more data from the network than will fit
         * into the pipe buffer or we _lose_ what did not fit if
         * the response ends sooner that BodyPipe frees up space:
         * There is no code to keep pumping data into the pipe once
         * response ends and serverComplete() is called.
         */
        const size_t adaptor_space = virginBodyDestination->buf().potentialSpaceSize();

        debugs(11,9, "Client may read up to min(" <<
               adaptor_space << ", " << space << ") bytes");

        if (adaptor_space < space)
            space = adaptor_space;
    }
#endif

    return space;
}

size_t
Client::replyBodySpace(const MemBuf &readBuf, const size_t minSpace) const
{
    size_t space = readBuf.spaceSize(); // available space w/o heroic measures
    if (space < minSpace) {
        const size_t maxSpace = readBuf.potentialSpaceSize(); // absolute best
        space = min(minSpace, maxSpace); // do not promise more than asked
    }

#if USE_ADAPTATION
    if (responseBodyBuffer) {
        return 0;   // Stop reading if already overflowed waiting for ICAP to catch up
    }

    if (virginBodyDestination != nullptr) {
        /*
         * BodyPipe buffer has a finite size limit.  We
         * should not read more data from the network than will fit
         * into the pipe buffer or we _lose_ what did not fit if
         * the response ends sooner that BodyPipe frees up space:
         * There is no code to keep pumping data into the pipe once
         * response ends and serverComplete() is called.
         *
         * If the pipe is totally full, don't register the read handler.
         * The BodyPipe will call our noteMoreBodySpaceAvailable() method
         * when it has free space again.
         */
        size_t adaptation_space =
            virginBodyDestination->buf().potentialSpaceSize();

        debugs(11,9, "Client may read up to min(" <<
               adaptation_space << ", " << space << ") bytes");

        if (adaptation_space < space)
            space = adaptation_space;
    }
#endif

    return space;
}

