/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "anyp/PortCfg.h"
#include "comm/Connection.h"
#include "CommCalls.h"
#include "fde.h"
#include "globals.h"

/* CommCommonCbParams */

CommCommonCbParams::CommCommonCbParams(void *aData):
    data(cbdataReference(aData)), conn(), flag(Comm::OK), xerrno(0), fd(-1)
{
}

CommCommonCbParams::CommCommonCbParams(const CommCommonCbParams &p):
    data(cbdataReference(p.data)), conn(p.conn), flag(p.flag), xerrno(p.xerrno), fd(p.fd)
{
}

CommCommonCbParams::~CommCommonCbParams()
{
    cbdataReferenceDone(data);
}

void
CommCommonCbParams::print(std::ostream &os) const
{
    if (conn != nullptr)
        os << conn;
    else
        os << "FD " << fd;

    if (xerrno)
        os << ", errno=" << xerrno;
    if (flag != Comm::OK)
        os << ", flag=" << flag;
    if (data)
        os << ", data=" << data;
}

/* CommAcceptCbParams */

CommAcceptCbParams::CommAcceptCbParams(void *aData):
    CommCommonCbParams(aData)
{
}

// XXX: Add CommAcceptCbParams::syncWithComm(). Adjust syncWithComm() API if all
// implementations always return true.

void
CommAcceptCbParams::print(std::ostream &os) const
{
    CommCommonCbParams::print(os);

    if (port && port->listenConn)
        os << ", " << port->listenConn->codeContextGist();
}

/* CommConnectCbParams */

CommConnectCbParams::CommConnectCbParams(void *aData):
    CommCommonCbParams(aData)
{
}

bool
CommConnectCbParams::syncWithComm()
{
    assert(conn);

    // change parameters if this is a successful callback that was scheduled
    // after its Comm-registered connection started to close

    if (flag != Comm::OK) {
        assert(!conn->isOpen());
        return true; // not a successful callback; cannot go out of sync
    }

    assert(conn->isOpen());
    if (!fd_table[conn->fd].closing())
        return true; // no closing in progress; in sync (for now)

    debugs(5, 3, "converting to Comm::ERR_CLOSING: " << conn);
    conn->noteClosure();
    flag = Comm::ERR_CLOSING;
    return true; // now the callback is in sync with Comm again
}

/* CommIoCbParams */

CommIoCbParams::CommIoCbParams(void *aData): CommCommonCbParams(aData),
    buf(nullptr), size(0)
{
}

bool
CommIoCbParams::syncWithComm()
{
    // change parameters if the call was scheduled before comm_close but
    // is being fired after comm_close
    if ((conn->fd < 0 || fd_table[conn->fd].closing()) && flag != Comm::ERR_CLOSING) {
        debugs(5, 3, "converting late call to Comm::ERR_CLOSING: " << conn);
        flag = Comm::ERR_CLOSING;
    }
    return true; // now we are in sync and can handle the call
}

void
CommIoCbParams::print(std::ostream &os) const
{
    CommCommonCbParams::print(os);
    if (buf) {
        os << ", size=" << size;
        os << ", buf=" << (void*)buf;
    }
}

/* CommCloseCbParams */

CommCloseCbParams::CommCloseCbParams(void *aData):
    CommCommonCbParams(aData)
{
}

/* CommTimeoutCbParams */

CommTimeoutCbParams::CommTimeoutCbParams(void *aData):
    CommCommonCbParams(aData)
{
}

/* CommAcceptCbPtrFun */

CommAcceptCbPtrFun::CommAcceptCbPtrFun(IOACB *aHandler,
                                       const CommAcceptCbParams &aParams):
    CommDialerParamsT<CommAcceptCbParams>(aParams),
    handler(aHandler)
{
}

CommAcceptCbPtrFun::CommAcceptCbPtrFun(const CommAcceptCbPtrFun &o):
    CommDialerParamsT<CommAcceptCbParams>(o.params),
    handler(o.handler)
{
}

void
CommAcceptCbPtrFun::dial()
{
    handler(params);
}

void
CommAcceptCbPtrFun::print(std::ostream &os) const
{
    os << '(';
    params.print(os);
    os << ')';
}

/* CommConnectCbPtrFun */

CommConnectCbPtrFun::CommConnectCbPtrFun(CNCB *aHandler,
        const CommConnectCbParams &aParams):
    CommDialerParamsT<CommConnectCbParams>(aParams),
    handler(aHandler)
{
}

void
CommConnectCbPtrFun::dial()
{
    handler(params.conn, params.flag, params.xerrno, params.data);
}

void
CommConnectCbPtrFun::print(std::ostream &os) const
{
    os << '(';
    params.print(os);
    os << ')';
}

/* CommIoCbPtrFun */

CommIoCbPtrFun::CommIoCbPtrFun(IOCB *aHandler, const CommIoCbParams &aParams):
    CommDialerParamsT<CommIoCbParams>(aParams),
    handler(aHandler)
{
}

void
CommIoCbPtrFun::dial()
{
    handler(params.conn, params.buf, params.size, params.flag, params.xerrno, params.data);
}

void
CommIoCbPtrFun::print(std::ostream &os) const
{
    os << '(';
    params.print(os);
    os << ')';
}

/* CommCloseCbPtrFun */

CommCloseCbPtrFun::CommCloseCbPtrFun(CLCB *aHandler,
                                     const CommCloseCbParams &aParams):
    CommDialerParamsT<CommCloseCbParams>(aParams),
    handler(aHandler)
{
}

void
CommCloseCbPtrFun::dial()
{
    handler(params);
}

void
CommCloseCbPtrFun::print(std::ostream &os) const
{
    os << '(';
    params.print(os);
    os << ')';
}

/* CommTimeoutCbPtrFun */

CommTimeoutCbPtrFun::CommTimeoutCbPtrFun(CTCB *aHandler,
        const CommTimeoutCbParams &aParams):
    CommDialerParamsT<CommTimeoutCbParams>(aParams),
    handler(aHandler)
{
}

void
CommTimeoutCbPtrFun::dial()
{
    handler(params);
}

void
CommTimeoutCbPtrFun::print(std::ostream &os) const
{
    os << '(';
    params.print(os);
    os << ')';
}

