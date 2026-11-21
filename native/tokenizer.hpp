#pragma once
#include "common.hpp"
#include <unordered_map>
#include <array>
namespace localstt {
class Tokenizer {
public:
    explicit Tokenizer(const fs::path& path);
    std::vector<size_t> encode(const std::string& text) const;
    std::string decode(const std::vector<size_t>& tokens) const;
    std::string token_bytes(size_t id) const;
    size_t id(const std::string& token) const;
private:
    std::unordered_map<std::wstring,size_t> vocab_;
    std::vector<std::wstring> tokens_;
    std::unordered_map<std::wstring,size_t> ranks_;
    std::array<wchar_t,256> byte_to_char_{};
    std::unordered_map<wchar_t,char> char_to_byte_;
    std::vector<size_t> bpe(std::wstring word) const;
};
}
