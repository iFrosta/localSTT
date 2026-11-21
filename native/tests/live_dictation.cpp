#include "live_dictation.hpp"
#include <cmath>
#include <future>
#include <stdexcept>

using namespace localstt;
using namespace std::chrono_literals;

void check(bool ok) { if (!ok) throw std::runtime_error("Live dictation regression"); }
AudioWindow audio(double seconds) { return {std::vector<float>(std::size_t(seconds * 100)), 100, false, true}; }

int main() {
    LiveDictation live;
    int iteration = 0;
    std::wstring typed;
    LiveIO io;
    io.capture = [&](double offset, double maximum) {
        check(maximum == 25);
        if (iteration >= 3) { check(std::abs(offset - 1.8) < 1e-8); return audio(0); }
        check(offset == (iteration == 2 ? 1 : 0));
        return audio(iteration == 2 ? .8 : 2);
    };
    io.recognize = [&](const AudioWindow&) -> Hypothesis {
        if (++iteration == 1) return {{{L" Привет", 0, .5}, {L" мир", .5, 1}}, L""};
        if (iteration == 2) {
            live.stop();
            return {{{L" Привет", 0, .5}, {L" мир", .5, 1}, {L" сегодня", 1, 1.5}}, L""};
        }
        return {{{L" сегодня.", 0, .5}}, L""};
    };
    io.deliver = [&](std::wstring_view tail) {
        auto n = iteration == 2 ? std::min<std::size_t>(3, tail.size()) : tail.size();
        typed += tail.substr(0, n);
        return n;
    };
    live.run(io, 0ms);
    check(typed == L"Привет мир сегодня." && live.text() == typed && live.delivered() == typed.size());

    LiveDictation cancelled;
    std::promise<void> entered, release;
    auto gate = release.get_future();
    io.capture = [](double, double) { return audio(1); };
    io.recognize = [&](const AudioWindow&) -> Hypothesis {
        entered.set_value(); gate.wait(); return {{{L" поздно", 0, .5}}, L" поздно"};
    };
    io.deliver = [](std::wstring_view) -> std::size_t { throw std::runtime_error("Late delivery"); };
    auto worker = std::async(std::launch::async, [&] { cancelled.run(io, 0ms); });
    entered.get_future().wait(); cancelled.cancel(); release.set_value(); worker.get();
    check(cancelled.text().empty() && cancelled.delivered() == 0);
}
