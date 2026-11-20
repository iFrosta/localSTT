#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace localstt {

struct Word {
    std::wstring text;
    double start, end;
};

struct Hypothesis {
    std::vector<Word> words;
    std::wstring text;
};

struct AudioWindow {
    std::vector<float> mono;
    int sample_rate;
    bool trailing_pause;
    bool reached_tail;
};

// The recorder supplies an immutable snapshot, so audio arriving during ASR
// remains available for the next window. An empty final snapshot ends the run.
struct LiveIO {
    std::function<AudioWindow(double offset, double maximum_seconds)> capture;
    std::function<Hypothesis(const AudioWindow&)> recognize;
    std::function<std::wstring(std::wstring)> correct;
    std::function<std::size_t(std::wstring_view)> deliver;
};

class LiveDictation {
public:
    static constexpr double maximum_window = 25.0;
    // run() owns one worker thread. stop() and cancel() may be called by the UI.
    void run(const LiveIO& io, std::chrono::milliseconds poll = std::chrono::milliseconds(900));
    void stop();
    void cancel();
    // Read these after the worker has returned.
    const std::wstring& text() const { return text_; }
    double offset() const { return offset_; }
    std::size_t delivered() const { return delivered_; }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false;
    bool cancelled_ = false;
    std::wstring text_;
    std::size_t delivered_ = 0;
    double offset_ = 0;
    std::vector<Word> previous_;
    void accept(Hypothesis result, double duration, bool flush, const LiveIO& io);
    void append(std::wstring_view part);
    void deliver(const LiveIO& io);
};
}
