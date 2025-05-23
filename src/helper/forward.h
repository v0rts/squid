/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_HELPER_FORWARD_H
#define SQUID_SRC_HELPER_FORWARD_H

#include "base/forward.h"

class statefulhelper;

class helper_stateful_server;

/// helper protocol primitives
namespace Helper
{

class Reply;
class Request;

class Client;
class Session;

using ClientPointer = RefCount<Client>;
using StatefulClientPointer = RefCount<statefulhelper>;

} // namespace Helper

typedef void HLPCB(void *, const Helper::Reply &);

#endif /* SQUID_SRC_HELPER_FORWARD_H */

