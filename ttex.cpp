#include "Dirac.h"
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstdint>
using namespace dirac;
static std::vector<float> gs;
static bool bad(float x){union{float f;uint32_t u;}v;v.f=x;return (v.u&0x7F800000u)==0x7F800000u;}
static void S(od::Inlet&in,float v){for(int i=0;i<FRAMELENGTH;i++)in.mBuf[i]=v;}
int main(){
  int N=48000; gs.assign(N,0.0f); for(int i=0;i<N;i++)gs[i]=0.4f*sinf(2*M_PI*330.0f*i/48000.0f);
  od::Sample s; s.mSampleCount=N; s.mChannelCount=1; s.mpData=gs.data();
  Dirac p; p.setSample(&s);
  S(p.mRateIn,6.0f);S(p.mGrainsIn,16);S(p.mLevelIn,0.8f);S(p.mGrainLenIn,0.03f);S(p.mPlayheadIn,0.3f);
  p.mFireIn.mConn=true;p.mHoldIn.mConn=true;p.mHoldIn.mBuf[0]=0;
  std::vector<float> o; int nf=0;
  for(int b=0;b<600;b++){
    float tex=0.5f*(1.0f+sinf(b*0.3f));       // Texture swept FAST every block, full range
    S(p.mTextureIn,tex);
    p.process(); for(int k=0;k<FRAMELENGTH;k++){o.push_back(p.mOutL.mBuf[k]); if(bad(p.mOutL.mBuf[k]))nf++;}
  }
  double maxd=0; for(size_t i=1;i<o.size();i++){double d=fabs(o[i]-o[i-1]); if(d>maxd)maxd=d;}
  printf("  Texture swept every block: maxDelta=%.4f  clicks(>0.05)=%s  non-finite=%d\n",
         maxd, maxd>0.05?"YES":"none", nf);
  printf("  [330Hz sine raw slope ~0.017; envelope-jump clicks would spike maxDelta]\n");
  return 0;
}
