## Copyright (C) 1996-2025 The Squid Software Foundation and contributors
##
## Squid software is distributed under GPLv2+ license and includes
## contributions from numerous individuals and organizations.
## Please see the COPYING and CONTRIBUTORS files for details.
##

include $(top_srcdir)/src/Common.am

noinst_LTLIBRARIES = \
	libcomm.la \
	libminimal.la

## Library holding comm socket handlers
libcomm_la_SOURCES = \
	AcceptLimiter.cc \
	AcceptLimiter.h \
	ConnOpener.cc \
	ConnOpener.h \
	Connection.cc \
	Connection.h \
	Flag.h \
	Incoming.cc \
	Incoming.h \
	IoCallback.cc \
	IoCallback.h \
	Loops.h \
	ModDevPoll.cc \
	ModEpoll.cc \
	ModKqueue.cc \
	ModPoll.cc \
	ModSelect.cc \
	Read.cc \
	Read.h \
	Tcp.cc \
	Tcp.h \
	TcpAcceptor.cc \
	TcpAcceptor.h \
	Write.cc \
	Write.h \
	comm_internal.h \
	forward.h

# a bare-bones implementation of few Comm APIs sufficient for helpers use
libminimal_la_SOURCES = \
	minimal.cc
