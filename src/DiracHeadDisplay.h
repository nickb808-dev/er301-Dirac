/* DiracHeadDisplay.h (v0.2.1) — waveform + playhead + per-grain "fired" ticks.
 *
 * Subclass of od::HeadDisplay (so it draws the sample waveform and the playhead
 * marker for free), extended to draw a mini tick per ACTIVE grain at the sample
 * position it was pulled from, with height = the grain's current envelope.
 * Mirrors core's GranularHeadDisplay, but reads Dirac's grain
 * accessors instead of GranularHead's. */

#pragma once

#include <od/graphics/sampling/HeadDisplay.h>
#include <Dirac.h>

namespace dirac {

class DiracHeadDisplay : public od::HeadDisplay
{
public:
    DiracHeadDisplay(Dirac *head, int left, int bottom,
                          int width, int height);
    virtual ~DiracHeadDisplay();

#ifndef SWIGLUA
    virtual void draw(od::FrameBuffer &fb);

    Dirac *grainHead()
    {
        return (Dirac *)mpHead;
    }

    // Per-slot "afterglow": grains are often far shorter than a UI frame, so we
    // hold each grain's tick at full height while active and let it fade over a
    // few frames after it ends — otherwise short/triggered grains never coincide
    // with a redraw and are invisible.
    static constexpr int kSlots = 64;   // ≥ the object's grain pool
    float mGlow[kSlots];
    int   mGlowSrc[kSlots];
#endif
};

} // namespace dirac
