/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_DISKIO_READREQUEST_H
#define SQUID_SRC_DISKIO_READREQUEST_H

#include "base/RefCount.h"
#include "cbdata.h"

class ReadRequest : public RefCountable
{
    CBDATA_CLASS(ReadRequest);

public:
    typedef RefCount<ReadRequest> Pointer;
    ReadRequest(char *buf, off_t offset, size_t len);
    ~ReadRequest() override {}

    char *buf;
    off_t offset;
    size_t len;
};

#endif /* SQUID_SRC_DISKIO_READREQUEST_H */

