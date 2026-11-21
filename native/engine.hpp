#pragma once
#include "common.hpp"
#include "live_dictation.hpp"
#include "dictionary.hpp"
#include <atomic>
#include <memory>
namespace localstt {
struct Result { Json json; Hypothesis hypothesis; };
class Engine {
public:
    Engine(Config& config); ~Engine();
    void load();
    Result transcribe(const std::vector<float>& audio, std::string language="", int beam=0, bool words=false, std::atomic<bool>* cancelled=nullptr, std::string_view context={});
    std::wstring correct(std::wstring text) const;
    Json health() const;
    Json gpu_info() const;
private:
    struct Impl; std::unique_ptr<Impl> p_;
};
class Service {
public:
    Service(Config& config);
    Engine engine;
    Result transcribe(const std::vector<float>& audio,std::string language="",int beam=0,bool words=false,bool record=true,std::atomic<bool>* cancelled=nullptr, std::string_view context={});
    void record(const Result& result);
    Json metrics() const;
    std::string cleanup(const std::string& text);
    Json cleanup_recommendation();
    void pull_cleanup(std::string requested = "");
    Json preflight();
    void shutdown();
private:
    Config& config_; mutable std::mutex mutex_; int count_=0; double duration_=0,processing_=0;
    void performance(const std::string& section,Json data);
};
}
