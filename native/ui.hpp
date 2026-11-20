#pragma once
#include "common.hpp"
#include <memory>

namespace localstt {
struct UiCallbacks {
    std::function<void()> tick;
    std::function<void(std::string)> action;
    std::function<Json()> status;
};
// Construct and run on the same thread. notify() may be called from workers.
class Ui {
public:
    Ui(Config& config, UiCallbacks callbacks);
    ~Ui();
    int run();
    // An empty title leaves the toast with only the app name Windows already shows.
    void notify(std::string title, std::string message);
    // Rebuilds the open settings page from current state; safe from any thread.
    void refresh_settings();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
