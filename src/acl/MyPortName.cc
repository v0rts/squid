/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "acl/FilledChecklist.h"
#include "acl/MyPortName.h"
#include "anyp/PortCfg.h"
#include "client_side.h"
#include "http/Stream.h"
#include "HttpRequest.h"

int
Acl::MyPortNameCheck::match(ACLChecklist * const ch)
{
    const auto checklist = Filled(ch);

    if (checklist->conn() != nullptr && checklist->conn()->port != nullptr)
        return data->match(checklist->conn()->port->name);
    if (checklist->request != nullptr)
        return data->match(checklist->request->myportname.termedBuf());
    return 0;
}

