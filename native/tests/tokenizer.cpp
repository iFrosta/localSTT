#include "tokenizer.hpp"
#include <iostream>
using namespace localstt;
int main(int argc,char**argv){try{if(argc!=3)throw std::runtime_error("tokenizer test needs model and fixture paths");Tokenizer t(wide(argv[1]));auto cases=read_json(wide(argv[2]));for(const auto& c:cases){auto text=c["text"].get<std::string>();auto ids=t.encode(text);if(ids!=c["ids"].get<std::vector<size_t>>())throw std::runtime_error("Tokenizer mismatch for "+text);if(t.decode(ids)!=text)throw std::runtime_error("Tokenizer round trip failed");}std::cout<<cases.size()<<" tokenizer reference cases passed\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
