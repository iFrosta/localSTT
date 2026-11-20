#include "live_dictation.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace localstt {
namespace {
std::wstring_view trim(std::wstring_view text) {
    constexpr auto whitespace = L" \r\n\t\v\f\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004"
                                L"\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000";
    const auto begin = text.find_first_not_of(whitespace);
    if (begin == text.npos) return {};
    return text.substr(begin, text.find_last_not_of(whitespace) - begin + 1);
}
}
void LiveDictation::stop() {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    changed_.notify_one();
}

void LiveDictation::cancel() {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    changed_.notify_one();
}

void LiveDictation::append(std::wstring_view part) {
    part = trim(part);
    if (part.empty()) return;
    constexpr std::wstring_view punctuation = L",.!?:;)]}";
    if (!text_.empty() && punctuation.find(part.front()) == punctuation.npos) text_ += L' ';
    text_.append(part);
}

void LiveDictation::accept(Hypothesis result, double duration, bool flush, const LiveIO& io) {
    auto& words = result.words;
    std::erase_if(words, [&](const Word& w) {
        return trim(w.text).empty() || !(w.start >= 0 && w.start < w.end && w.end <= duration + 0.05);
    });
    if (words.empty()) {
        previous_.clear();
        if (flush || duration >= maximum_window) {
            append(result.text);
            offset_ += duration;
        }
        return;
    }
    std::size_t count = words.size();
    if (!flush) {
        std::size_t stable = 0;
        while (stable < std::min(words.size(), previous_.size()) &&
               trim(words[stable].text) == trim(previous_[stable].text)) ++stable;
        const auto limit = words.size() == 1 && duration >= maximum_window ? 1 : words.size() - 1;
        count = std::min(stable, limit);
    }
    if (!count) { previous_ = std::move(words); return; }
    std::wstring part;
    for (std::size_t i = 0; i < count; ++i) part += words[i].text;
    append(io.correct ? io.correct(std::move(part)) : part);
    offset_ += flush ? duration : std::min(duration, words[count - 1].end);
    previous_.clear();
}

void LiveDictation::deliver(const LiveIO& io) {
    const auto pending = std::wstring_view(text_).substr(delivered_);
    if (pending.empty()) return;
    auto sent = io.deliver(pending);
    if (sent > pending.size()) throw std::runtime_error("Invalid delivery count");
    delivered_ += sent;
}

void LiveDictation::run(const LiveIO& io, std::chrono::milliseconds poll) {
    for (;;) {
        std::unique_lock lock(mutex_);
        if (cancelled_) return;
        const bool final = stopping_;
        lock.unlock();
        auto window = io.capture(offset_, maximum_window);
        if (window.sample_rate <= 0) throw std::runtime_error("Invalid sample rate");
        const double duration = double(window.mono.size()) / window.sample_rate;
        if (duration >= (final ? 0.15 : 0.5)) {
            auto result = io.recognize(window);
            lock.lock();
            if (cancelled_) return;
            accept(std::move(result), duration, window.reached_tail && (final || window.trailing_pause), io);
        } else {
            lock.lock();
            if (cancelled_) return;
        }
        // Serializing delivery with cancellation prevents late keystrokes after cancel returns.
        // UI delivery must return promptly, reporting only the accepted UTF-16 code units.
        deliver(io);
        if (final && duration < 0.15) return;
        changed_.wait_for(lock, poll, [&] { return stopping_ || cancelled_; });
    }
}
}
