#pragma once

#include <deque>
#include <mutex>
#include <string>

namespace world {

// Reads command lines from stdin on a background thread.
//
// The thread only ever appends to a queue; the simulation drains it between
// ticks. Reading stdin blocks, and blocking is exactly what must not happen on
// the thread running the world, so the two are kept apart and share nothing but
// a mutex-protected queue of strings.
class ConsoleInput {
public:
    // Starts reading. Safe to call when stdin is not a terminal: the thread
    // simply sees EOF and stops.
    void start();

    // Pops one pending line, if any. Never blocks.
    bool next(std::string* line);

private:
    std::mutex mMutex;
    std::deque<std::string> mLines;
};

}  // namespace world
