/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#include "squid.h"
#include "store_digest.h"

#define STUB_API "store_digets.cc"
#include "tests/STUB.h"

class StoreEntry;
void storeDigestInit(void) STUB
void storeDigestNoteStoreReady(void) STUB
void storeDigestDel(const StoreEntry *) STUB
void storeDigestReport(StoreEntry *) STUB

