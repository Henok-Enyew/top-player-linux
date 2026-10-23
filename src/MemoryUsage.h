#pragma once

#include <QtGlobal>

// The player's memory use, and keeping it low. Decoding runs on many
// threads; glibc gives busy threads arenas of their own and keeps what they
// freed, so a player that has played a few files holds on to far more memory
// than it uses. See docs/memory-audit.md.
namespace MemoryUsage {

// Caps glibc's malloc arenas (unless $MALLOC_ARENA_MAX says otherwise).
// Call first thing in main(), before any thread starts.
void tuneAllocator();
// Hands memory freed since to the system, a moment from now, once for any
// number of calls in a row. Call after dropping something big (a helper
// player, a file's caches).
void releaseFreed();
// Resident memory (RSS) of the process in bytes, or -1 if unknown.
qint64 residentBytes();
// Of that, memory the process allocated itself (heap, thread stacks, decoded
// frames), not counting the code of the libraries it maps: what Settings >
// About reports and docs/memory-audit.md measures.
qint64 anonymousBytes();

} // namespace MemoryUsage
