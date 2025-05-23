/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_DISKIO_DISKDAEMON_DISKDAEMONDISKIOMODULE_H
#define SQUID_SRC_DISKIO_DISKDAEMON_DISKDAEMONDISKIOMODULE_H

#include "DiskIO/DiskIOModule.h"

class DiskDaemonDiskIOModule : public DiskIOModule
{

public:
    static DiskDaemonDiskIOModule &GetInstance();
    DiskDaemonDiskIOModule();
    void init() override;
    void gracefulShutdown() override;
    char const *type () const override;
    DiskIOStrategy* createStrategy() override;

private:
    static DiskDaemonDiskIOModule Instance;
    bool initialised;
    void registerWithCacheManager(void);
};

#endif /* SQUID_SRC_DISKIO_DISKDAEMON_DISKDAEMONDISKIOMODULE_H */

