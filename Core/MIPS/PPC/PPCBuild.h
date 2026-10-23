// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Whether the PPC JIT backend is compiled: on 64-bit PowerPC (the PS3, the ppc64 Linux test build),
// and in the desktop host test build (PPSSPP_RSX_HOSTSIM), where it only generates code, to time
// and profile the compiler (PPC_XPROF=1 with --cpu=ir; see IRJit.cpp).
#pragma once
#include "ppsspp_config.h"
#if (PPSSPP_ARCH(POWERPC) && PPSSPP_ARCH(64BIT)) || defined(PPSSPP_RSX_HOSTSIM)
#define PPSSPP_PPC_JIT_BUILD 1
#endif
