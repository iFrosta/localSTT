#include "features.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace localstt;
namespace {
void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
void check_rejected(const std::vector<float>& input,int mels,int fft,int hop,int padding) {
    bool threw=false;
    try {log_mel(input,mels,fft,hop,padding);}catch(const std::invalid_argument&){threw=true;}
    require(threw,"Invalid feature-extraction arguments must be rejected");
}
std::uint32_t read_u32(std::istream& stream) {
    unsigned char bytes[4]{};
    stream.read(reinterpret_cast<char*>(bytes),4);
    if(!stream)throw std::runtime_error("Truncated feature reference");
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1])<<8) |
           (std::uint32_t(bytes[2])<<16) | (std::uint32_t(bytes[3])<<24);
}
void compare_fixture(const char* path) {
    std::ifstream stream(path,std::ios::binary);
    char magic[8]{};stream.read(magic,8);
    require(std::string(magic,8)=="FMEL0001","Invalid feature reference file");
    const auto cases=read_u32(stream);
    require(cases>0&&cases<=100,"Invalid reference case count");
    for(std::uint32_t c=0;c<cases;++c) {
        const auto name_size=read_u32(stream);require(name_size<1000,"Invalid fixture name");
        std::string name(name_size,0);stream.read(name.data(),name.size());
        const int mels=read_u32(stream),fft=read_u32(stream),hop=read_u32(stream),padding=read_u32(stream);
        const auto samples=read_u32(stream),frames=read_u32(stream);
        require(samples<16000*600&&frames<100000,"Reference case too large");
        std::vector<float> audio(samples),expected(static_cast<std::size_t>(mels)*frames);
        stream.read(reinterpret_cast<char*>(audio.data()),audio.size()*sizeof(float));
        stream.read(reinterpret_cast<char*>(expected.data()),expected.size()*sizeof(float));
        require(bool(stream),"Truncated reference samples");
        const auto started=std::chrono::steady_clock::now();
        const auto actual=log_mel(audio,mels,fft,hop,padding);
        const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        require(actual.n_mels==static_cast<std::size_t>(mels)&&actual.frames==frames,"Feature matrix shape differs from faster-whisper");
        float maximum_error=0;
        double total_error=0;
        for(std::size_t i=0;i<expected.size();++i) {
            require(std::isfinite(actual.data[i]),"Non-finite feature value");
            const float error=std::abs(actual.data[i]-expected[i]);
            maximum_error=std::max(maximum_error,error);total_error+=error;
        }
        std::cout<<name<<": "<<elapsed<<" ms; maximum error "<<maximum_error
                 <<", mean error "<<total_error/expected.size()<<'\n';
        require(maximum_error<0.0002f,"Log mel features differ from NumPy reference by more than 2e-4");
    }
}
}
int main(int argc,char** argv) {
    try {
        const auto empty=log_mel({});
        require(empty.n_mels==128&&empty.frames==1,"Empty input includes the default 160 zero samples");
        for(float value:empty.data)require(value==-1.5f,"Silence normalization must equal -1.5");
        const auto silence=log_mel(std::vector<float>(512));
        require(silence.frames==4,"Feature frame count must include 160 tail samples and drop the last STFT frame");
        for(float value:silence.data)require(value==-1.5f,"Silent frames must be finite");
        std::vector<float> signal(640);
        for(int i=0;i<640;++i)signal[i]=static_cast<float>(((i*37)%101-50)/50.0);
        const auto compact=log_mel(signal,80);
        require(compact.frames==5&&compact.n_mels==80&&compact.data.size()==400,"80-mel matrix dimensions");
        // Independent golden values generated with installed faster-whisper's
        // NumPy FeatureExtractor; covers first/last reflect-padded frames and
        // low/high mel bins. No Python is required to run this test.
        const std::pair<std::size_t,float> reference[]={
            {0,0.35616064071655273f},{1,-0.010211586952209473f},{3,-0.29619550704956055f},
            {4,0.12561941146850586f},{5,0.1251031756401062f},{17,0.7763779759407043f},
            {48,0.5571547150611877f},{99,0.5828959941864014f},{145,0.9573699235916138f},
            {199,0.9643685817718506f},{245,0.512185275554657f},{300,0.8511170744895935f},
            {345,0.8727800846099854f},{397,0.8455974459648132f},{399,0.7833250761032104f}};
        for(auto [index,value]:reference)require(std::abs(compact.data[index]-value)<0.0002f,"Compact NumPy reference mismatch");
        require(log_mel(signal,80,400,160,0).frames==4,"Disabling tail padding must remove one frame");
        check_rejected({},128,400,160,0);
        check_rejected(signal,0,400,160,160);
        check_rejected(signal,128,1,160,160);
        check_rejected(signal,128,400,0,160);
        check_rejected(signal,128,400,160,-1);
        check_rejected({std::numeric_limits<float>::quiet_NaN()},128,400,160,160);
        if(argc>1)compare_fixture(argv[1]);
        std::cout<<"feature tests passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"feature test failed: "<<e.what()<<'\n';return 1;}
}
