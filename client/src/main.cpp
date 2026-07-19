#include "application.h"

int main() {
    game::Application app;
    if (!app.init(1280, 720, "untitled")) {
        return 1;
    }
    app.run();
    return 0;
}
