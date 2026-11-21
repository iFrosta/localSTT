#include "features.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace localstt {
namespace {
constexpr double pi = 3.141592653589793238462643383279502884;
struct Complex {
    double re = 0, im = 0;
    Complex operator+(Complex b) const { return {re + b.re, im + b.im}; }
    Complex operator-(Complex b) const { return {re - b.re, im - b.im}; }
    Complex operator*(Complex b) const { return {re*b.re - im*b.im, re*b.im + im*b.re}; }
};

// Decimation-in-time mixed-radix FFT. Plans and twiddles are immutable, while
// each call supplies its own workspace. For Whisper's 400-point transform the
// factors are 2,2,2,2,5,5; there is no power-of-two padding or spectral change.
class FftPlan {
    struct Stage { int radix, width; };
    int size_;
    std::vector<Stage> stages_;
    std::vector<Complex> twiddles_;

    void transform(Complex* output, const float* input, int stride, std::size_t stage, Complex* scratch) const {
        const auto [radix,width] = stages_[stage];
        if(width == 1) {
            for(int i=0;i<radix;++i) output[i] = {input[i*stride],0};
        } else {
            for(int i=0;i<radix;++i)
                transform(output+i*width,input+i*stride,stride*radix,stage+1,scratch);
        }
        if(radix==2) {
            for(int k=0;k<width;++k) {
                const Complex even=output[k];
                const Complex odd=output[k+width]*twiddles_[k*stride];
                output[k]=even+odd;
                output[k+width]=even-odd;
            }
            return;
        }
        // Child transforms are finished before this butterfly begins, so all
        // recursion levels safely share the caller-owned scratch workspace.
        for(int k=0;k<width;++k) {
            for(int i=0;i<radix;++i)scratch[i]=output[k+i*width];
            for(int q=0;q<radix;++q) {
                Complex sum=scratch[0];
                const int step=(k+q*width)*stride;
                int twiddle=step;
                for(int i=1;i<radix;++i) {
                    sum=sum+scratch[i]*twiddles_[twiddle];
                    twiddle+=step;
                    // Both operands are below size_, so one subtraction suffices.
                    if(twiddle>=size_)twiddle-=size_;
                }
                output[k+q*width]=sum;
            }
        }
    }
public:
    explicit FftPlan(int size):size_(size),twiddles_(size) {
        for(int n=size;n>1;) {
            int factor=2;
            while(factor*factor<=n && n%factor!=0)++factor;
            if(n%factor!=0)factor=n;
            n/=factor;
            stages_.push_back({factor,n});
        }
        for(int i=0;i<size;++i) {
            const double angle=-2*pi*i/size;
            twiddles_[i]={std::cos(angle),std::sin(angle)};
        }
    }
    void transform(const float* input,Complex* output,Complex* scratch) const {transform(output,input,1,0,scratch);}
};

struct Band {
    int begin=0;
    std::vector<float> weights;
};
struct FeaturePlan {
    int fft_size, mel_count;
    FftPlan fft;
    std::vector<float> window;
    std::vector<Band> bands;
    FeaturePlan(int n_fft,int n_mels):fft_size(n_fft),mel_count(n_mels),fft(n_fft),window(n_fft),bands(n_mels) {
        for(int i=0;i<n_fft;++i) window[i]=static_cast<float>(0.5-0.5*std::cos(2*pi*i/n_fft));
        std::vector<double> centers(n_mels+2);
        const double logstep=std::log(6.4)/27.0;
        for(int i=0;i<n_mels+2;++i) {
            const double mel=45.245640471924965*i/(n_mels+1);
            centers[i]=mel>=15.0?1000.0*std::exp(logstep*(mel-15.0)):(200.0/3)*mel;
        }
        const int bins=n_fft/2+1;
        for(int m=0;m<n_mels;++m) {
            auto& band=bands[m];
            bool found=false;
            for(int bin=0;bin<bins;++bin) {
                const double frequency=static_cast<double>(bin)*16000/n_fft;
                const double lower=(frequency-centers[m])/(centers[m+1]-centers[m]);
                const double upper=(centers[m+2]-frequency)/(centers[m+2]-centers[m+1]);
                const float weight=static_cast<float>(std::max(0.0,std::min(lower,upper))*2/(centers[m+2]-centers[m]));
                if(weight>0) {
                    if(!found){band.begin=bin;found=true;}
                    band.weights.push_back(weight);
                } else if(found) break;
            }
        }
    }
};

std::shared_ptr<const FeaturePlan> get_plan(int n_fft,int n_mels) {
    static std::mutex mutex;
    // Keep a small bounded cache: normal applications only need 80 and 128 mels.
    static std::vector<std::shared_ptr<const FeaturePlan>> cache;
    std::lock_guard lock(mutex);
    for(const auto& plan:cache)
        if(plan->fft_size==n_fft && plan->mel_count==n_mels)return plan;
    auto plan=std::make_shared<const FeaturePlan>(n_fft,n_mels);
    if(cache.size()>=8)cache.erase(cache.begin());
    cache.push_back(plan);
    return plan;
}

std::size_t reflected(std::int64_t index,std::size_t length) {
    if(length==1)return 0;
    const auto period=static_cast<std::int64_t>(2*(length-1));
    index%=period;
    if(index<0)index+=period;
    return static_cast<std::size_t>(index<static_cast<std::int64_t>(length)?index:period-index);
}
}

Features log_mel(const std::vector<float>& audio,int n_mels,int n_fft,int hop_length,int padding) {
    if(n_fft<2||n_fft>4096)throw std::invalid_argument("n_fft must be between 2 and 4096");
    if(n_mels<1||n_mels>1024)throw std::invalid_argument("n_mels must be between 1 and 1024");
    if(hop_length<1||padding<0)throw std::invalid_argument("hop_length must be positive and padding nonnegative");
    if(audio.size()>static_cast<std::size_t>(INT64_MAX/2-padding))throw std::length_error("Audio is too long");
    const std::size_t length=audio.size()+static_cast<std::size_t>(padding);
    if(length==0)throw std::invalid_argument("Cannot reflect-pad empty audio without tail padding");
    const auto adjusted_length=length-static_cast<std::size_t>(n_fft%2);
    const auto frames=adjusted_length/static_cast<std::size_t>(hop_length);
    if(frames==0)throw std::invalid_argument("Audio produces no feature frames");
    if(frames>std::numeric_limits<std::size_t>::max()/static_cast<std::size_t>(n_mels))throw std::length_error("Feature matrix is too large");
    for(float sample:audio)if(!std::isfinite(sample))throw std::invalid_argument("Audio samples must be finite");
    const auto plan=get_plan(n_fft,n_mels);
    Features result;
    result.n_mels=static_cast<std::size_t>(n_mels);
    result.frames=frames;
    result.data.resize(result.n_mels*frames);
    std::vector<float> windowed(n_fft),power(n_fft/2+1);
    std::vector<Complex> spectrum(n_fft),scratch(n_fft);
    float maximum=-std::numeric_limits<float>::infinity();
    for(std::size_t frame=0;frame<frames;++frame) {
        const auto origin=static_cast<std::int64_t>(frame*hop_length)-n_fft/2;
        for(int i=0;i<n_fft;++i) {
            const auto index=reflected(origin+i,length);
            // Multiplication happens in float32 before the NumPy-compatible FFT.
            windowed[i]=(index<audio.size()?audio[index]:0.0f)*plan->window[i];
        }
        plan->fft.transform(windowed.data(),spectrum.data(),scratch.data());
        for(std::size_t bin=0;bin<power.size();++bin) {
            // NumPy casts the FFT to complex64 before abs(z)**2.
            const float re=static_cast<float>(spectrum[bin].re),im=static_cast<float>(spectrum[bin].im);
            const float magnitude=std::hypot(re,im);
            power[bin]=magnitude*magnitude;
        }
        for(int mel=0;mel<n_mels;++mel) {
            const auto& band=plan->bands[mel];
            float energy=0;
            for(std::size_t i=0;i<band.weights.size();++i)energy+=band.weights[i]*power[band.begin+i];
            const float value=std::log10(std::max(energy,1e-10f));
            result.data[static_cast<std::size_t>(mel)*frames+frame]=value;
            maximum=std::max(maximum,value);
        }
    }
    const float floor=maximum-8.0f;
    for(float& value:result.data)value=(std::max(value,floor)+4.0f)/4.0f;
    return result;
}
}
