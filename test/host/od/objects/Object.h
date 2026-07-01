// Host-test stub of od/objects/Object.h
#pragma once
#include <od/config.h>
namespace od {
struct Port {
    const char *mName;
    float mBuf[FRAMELENGTH];
    explicit Port(const char *n) : mName(n) { for (int i = 0; i < FRAMELENGTH; ++i) mBuf[i] = 0.0f; }
    float *buffer() { return mBuf; }
};
using Inlet  = Port;
using Outlet = Port;
struct Object {
    virtual ~Object() {}
    virtual void process() {}
    void addInput(Port &) {}
    void addOutput(Port &) {}
};
} // namespace od
