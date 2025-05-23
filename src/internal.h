/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/*
 * DEBUG: section 76    Internal Squid Object handling
 * AUTHOR: Duane, Alex, Henrik
 */

#ifndef SQUID_SRC_INTERNAL_H
#define SQUID_SRC_INTERNAL_H

#include "comm/forward.h"
#include "log/forward.h"
#include "sbuf/forward.h"

class HttpRequest;
class StoreEntry;

void internalStart(const Comm::ConnectionPointer &clientConn, HttpRequest *, StoreEntry *, const AccessLogEntryPointer &);
bool internalCheck(const SBuf &urlPath);
bool internalStaticCheck(const SBuf &urlPath);
char *internalLocalUri(const char *dir, const SBuf &name);
char *internalRemoteUri(bool, const char *, unsigned short, const char *, const SBuf &);
const char *internalHostname(void);
bool internalHostnameIs(const SBuf &);

/// whether the given request URL path points to a cache manager (not
/// necessarily running on this Squid instance)
bool ForSomeCacheManager(const SBuf &);

#endif /* SQUID_SRC_INTERNAL_H */

