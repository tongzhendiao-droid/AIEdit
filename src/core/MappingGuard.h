#pragma once

#include <cstddef>
#include <cstdint>

namespace aiedit::core {

// Keeps a truncated file from crashing the process (POSIX).
//
// When another process truncates a memory-mapped file, as logrotate's copytruncate does to the
// log it rotates, reading a mapped page that now lies past the end of the file raises SIGBUS.
// For mappings registered here the SIGBUS handler instead puts zero-filled pages over the rest
// of the mapping, and the read goes on and sees zeros. Those last only until the file is read
// again: the views reload as soon as they notice the file shrink. A SIGBUS anywhere else is
// passed on to whatever handled it before.
//
// Windows does not let a mapped file be truncated, so there this does nothing.
void guardMapping(const void* start, std::size_t length);
// Before unmapping.
void unguardMapping(const void* start);
// How many times reads past the end of a truncated file were caught (for tests).
std::uint64_t truncatedReadsCaught();

} // namespace aiedit::core
