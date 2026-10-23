// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Executable memory for the PS3 JIT. A normal LV2 process can't write its code or make data
// executable, but CFW with PS3MAPI (Cobra/webMAN) can write into a process's memory, including
// its code segment. The JIT therefore gets an 8 MB area of nops inside our own .text: code is
// generated into an ordinary buffer (the "writable mirror") and copied into that area through
// PS3MAPI, followed by the cache maintenance needed for the CPU to see new instructions.
#pragma once
#include <cstddef>
#include <cstdint>

namespace PS3Jit {

// The executable area and its size.
uint8_t *CodeArea();
size_t CodeAreaSize();

// True when PS3MAPI is present and a test write into the area works. Cached after the first call.
bool Available();
const char *StatusText();

// Copies len bytes from src into the executable area at code (must be inside it) and makes the
// instruction cache see them. Returns false on failure.
bool Commit(const uint8_t *code, const uint8_t *src, size_t len);

// Statistics: bytes and calls committed.
uint64_t CommittedBytes();
uint64_t CommitCalls();
// Time base ticks spent committing.
uint64_t CommitTicks();

}  // namespace PS3Jit
