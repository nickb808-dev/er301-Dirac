#include "Dirac.h"
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstdint>
using namespace dirac;
static std::vector<float> gs;
static bool bad(float x){union{float f;uint32_t u;}v;v.f=x;return (v.u&0x7F800000u)==0x7F800000u;}
static void S(od::Inlet&in,float v){for(int i=0;i<FRAMELENGTH;i++)in.mBuf[i]=v;}
static void run(float bin,double&lr_diff,double&hi_ratio,int&nf){
  int N=48000; gs.assign(N,0.0f); for(int i=0;i<N;i++)gs[i]=0.3f*(sinf(2*M_PI*300.0f*i/48000.0f)+0.5f*sinf(2*M_PI*6000.0f*i/48000.0f));
  od::Sample s; s.mSampleCount=N; s.mChannelCount=1; s.mpData=gs.data();
  Dirac p; p.setSample(&s);
  S(p.mRateIn,5.0f);S(p.mGrainsIn,12);S(p.mLevelIn,0.8f);S(p.mGrainLenIn,0.04f);
  S(p.mSprdIn,1.0f);S(p.mPlayheadIn,0.3f);S(p.mBinauralIn,bin);S(p.mTextureIn,0.5f);
  p.mFireIn.mConn=true;p.mHoldIn.mConn=true;p.mHoldIn.mBuf[0]=0;
  double sd=0,sl=0; int n=0;
  for(int b=0;b<300;b++){p.process();for(int k=0;k<FRAMELENGTH;k++){float L=p.mOutL.mBuf[k],R=p.mOutR.mBuf[k];
    if(bad(L)||bad(R))nf++; if(b>50){sd+=(L-R)*(L-R); sl+=L*L+R*R; n++;}}}
  lr_diff=sqrt(sd/n); hi_ratio=sqrt(sl/n);
}
int main(){
  int nf=0; double d0,e0,d1,e1;
  run(0.0f,d0,e0,nf); run(1.0f,d1,e1,nf);
  printf("  binaural 0: RMS(L-R)=%.4f  totalRMS=%.4f\n", d0,e0);
  printf("  binaural 1: RMS(L-R)=%.4f  totalRMS=%.4f  (L/R should decorrelate MORE)\n", d1,e1);
  printf("  decorrelation increase: %.2fx   non-finite: %d\n", d1/std::max(1e-6,d0), nf);
  return 0;
}
