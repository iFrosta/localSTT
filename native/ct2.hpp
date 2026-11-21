#pragma once
#include "engine.hpp"
namespace localstt {
class Ct2Backend {
public:
    explicit Ct2Backend(Config& config);
    ~Ct2Backend();
    void load();
    Result transcribe(const std::vector<float>& audio,std::string language,int beam,bool words,std::atomic<bool>* cancel,
                      std::string_view committed_context = {});
    Json health() const;
    Json gpu_info() const;
    // The dictionary's term list, re-encoded whenever dictionary.json changes.
    void set_initial_prompt(const std::string& prompt);
private:
    struct Impl;std::unique_ptr<Impl> p_;
};
}
