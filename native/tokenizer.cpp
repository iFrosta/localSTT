#include "tokenizer.hpp"
#include <limits>
namespace localstt {
namespace {
int category(wchar_t c){WORD kind=0;GetStringTypeW(CT_CTYPE1,&c,1,&kind);if(kind&C1_ALPHA)return 1;if(kind&C1_DIGIT)return 2;if(kind&C1_SPACE)return 3;return 4;}
std::vector<std::wstring> split(const std::wstring& text){
    std::vector<std::wstring> out;size_t i=0;
    while(i<text.size()){
        bool contraction=false;for(const auto suffix:{L"'s",L"'t",L"'re",L"'ve",L"'m",L"'ll",L"'d"}){std::wstring_view s=suffix;if(text.compare(i,s.size(),s)==0){out.emplace_back(s);i+=s.size();contraction=true;break;}}
        if(contraction)continue;
        size_t start=i;int type=category(text[i]);
        if(text[i]==L' '&&i+1<text.size()&&category(text[i+1])!=3){++i;type=category(text[i]);}
        if(type==3){while(i<text.size()&&category(text[i])==3)++i;if(i<text.size()&&i-start>1)--i;}
        else {while(i<text.size()&&category(text[i])==type)++i;}
        if(i==start)++i;out.push_back(text.substr(start,i-start));
    }return out;
}
std::wstring pair_key(std::wstring_view a,std::wstring_view b){std::wstring key(a);key.push_back(0);key.append(b);return key;}
}
Tokenizer::Tokenizer(const fs::path& path){
    std::array<bool,256> direct{};for(int i=33;i<=126;++i)direct[i]=true;for(int i=161;i<=172;++i)direct[i]=true;for(int i=174;i<=255;++i)direct[i]=true;
    int next=256;for(int i=0;i<256;++i){byte_to_char_[i]=wchar_t(direct[i]?i:next++);char_to_byte_[byte_to_char_[i]]=char(i);}
    auto j=read_json(path);auto v=j.at("model").at("vocab");
    for(auto it=v.begin();it!=v.end();++it){size_t n=it.value().get<size_t>();auto token=wide(it.key());vocab_[token]=n;if(tokens_.size()<=n)tokens_.resize(n+1);tokens_[n]=token;}
    for(const auto& t:j.value("added_tokens",Json::array())){size_t n=t.at("id").get<size_t>();auto token=wide(t.at("content").get<std::string>());vocab_[token]=n;if(tokens_.size()<=n)tokens_.resize(n+1);tokens_[n]=token;}
    size_t rank=0;for(const auto& merge:j.at("model").at("merges")){std::wstring a,b;if(merge.is_array()){a=wide(merge[0].get<std::string>());b=wide(merge[1].get<std::string>());}else{auto s=wide(merge.get<std::string>());auto pos=s.find(L' ');if(pos==s.npos)throw std::runtime_error("Malformed BPE merge");a=s.substr(0,pos);b=s.substr(pos+1);}ranks_[pair_key(a,b)]=rank++;}
}
size_t Tokenizer::id(const std::string& token) const {auto it=vocab_.find(wide(token));if(it==vocab_.end())throw std::runtime_error("Tokenizer lacks token "+token);return it->second;}
std::vector<size_t> Tokenizer::bpe(std::wstring word) const {
    std::vector<std::wstring> pieces;for(auto c:word)pieces.emplace_back(1,c);
    while(pieces.size()>1){size_t best=std::numeric_limits<size_t>::max(),at=0;
        for(size_t i=0;i+1<pieces.size();++i){auto rank=ranks_.find(pair_key(pieces[i],pieces[i+1]));if(rank!=ranks_.end()&&rank->second<best){best=rank->second;at=i;}}
        if(best==std::numeric_limits<size_t>::max())break;auto a=pieces[at],b=pieces[at+1];
        for(size_t i=0;i+1<pieces.size();){if(pieces[i]==a&&pieces[i+1]==b){pieces[i]+=pieces[i+1];pieces.erase(pieces.begin()+i+1);++i;}else ++i;}
    }
    std::vector<size_t> out;for(auto& p:pieces){auto it=vocab_.find(p);if(it==vocab_.end())throw std::runtime_error("BPE token is missing");out.push_back(it->second);}return out;
}
std::vector<size_t> Tokenizer::encode(const std::string& text) const {std::vector<size_t> out;for(const auto& piece:split(wide(text))){std::wstring encoded;for(unsigned char b:utf8(piece))encoded.push_back(byte_to_char_[b]);auto ids=bpe(std::move(encoded));out.insert(out.end(),ids.begin(),ids.end());}return out;}
std::string Tokenizer::token_bytes(size_t token) const {if(token>=tokens_.size())throw std::runtime_error("Invalid token ID");if(token>=id("<|endoftext|>"))return {};std::string out;for(wchar_t c:tokens_[token]){auto it=char_to_byte_.find(c);if(it==char_to_byte_.end())throw std::runtime_error("Invalid byte-level token");out.push_back(it->second);}return out;}
std::string Tokenizer::decode(const std::vector<size_t>& tokens) const {std::string out;for(size_t token:tokens)out+=token_bytes(token);return out;}
}
