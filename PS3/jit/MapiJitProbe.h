// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#pragma once
#include <string>
#include <vector>

// Checks whether generated machine code can run on this console via PS3MAPI (CFW). Report lines
// are also written to USRDIR/jit_probe.txt and launch.log.
std::vector<std::string> MapiJitProbe();
