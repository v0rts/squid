## Copyright (C) 1996-2025 The Squid Software Foundation and contributors
##
## Squid software is distributed under GPLv2+ license and includes
## contributions from numerous individuals and organizations.
## Please see the COPYING and CONTRIBUTORS files for details.
##

include $(top_srcdir)/src/Common.am

noinst_LTLIBRARIES = libipc.la

libipc_la_SOURCES = \
	Coordinator.cc \
	Coordinator.h \
	FdNotes.cc \
	FdNotes.h \
	Forwarder.cc \
	Forwarder.h \
	Inquirer.cc \
	Inquirer.h \
	Kid.cc \
	Kid.h \
	Kids.cc \
	Kids.h \
	MemMap.cc \
	MemMap.h \
	Messages.h \
	Port.cc \
	Port.h \
	QuestionerId.cc \
	QuestionerId.h \
	Queue.cc \
	Queue.h \
	ReadWriteLock.cc \
	ReadWriteLock.h \
	Request.h \
	RequestId.cc \
	RequestId.h \
	Response.h \
	SharedListen.cc \
	SharedListen.h \
	StartListening.cc \
	StartListening.h \
	StoreMap.cc \
	StoreMap.h \
	Strand.cc \
	Strand.h \
	StrandCoord.cc \
	StrandCoord.h \
	StrandCoords.h \
	StrandSearch.cc \
	StrandSearch.h \
	TypedMsgHdr.cc \
	TypedMsgHdr.h \
	UdsOp.cc \
	UdsOp.h \
	forward.h \
	mem/FlexibleArray.h \
	mem/Page.cc \
	mem/Page.h \
	mem/PagePool.cc \
	mem/PagePool.h \
	mem/PageStack.cc \
	mem/PageStack.h \
	mem/Pages.cc \
	mem/Pages.h \
	mem/Pointer.h \
	mem/Segment.cc \
	mem/Segment.h \
	mem/forward.h

DEFS += -DDEFAULT_STATEDIR=\"$(localstatedir)/run/squid\"

install-data-local:
	$(mkinstalldirs) $(DESTDIR)$(localstatedir)/run/squid;

libipc_la_LIBADD = $(ATOMICLIB)

