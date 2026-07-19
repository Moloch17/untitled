#include "console.h"

#include <iostream>
#include <thread>

namespace world {

void ConsoleInput::start() {
    std::thread([this] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> guard(mMutex);
            mLines.push_back(line);
        }
    }).detach();
}

bool ConsoleInput::next(std::string* line) {
    std::lock_guard<std::mutex> guard(mMutex);
    if (mLines.empty()) {
        return false;
    }
    *line = mLines.front();
    mLines.pop_front();
    return true;
}

}  // namespace world
