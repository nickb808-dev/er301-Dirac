/* DiracHeadDisplay.cpp — see header.  Modeled on core/graphics/GranularHeadDisplay.cpp. */

#include "DiracHeadDisplay.h"
#include <od/graphics/constants.h>

namespace dirac {

DiracHeadDisplay::DiracHeadDisplay(Dirac *head, int left,
                                             int bottom, int width, int height)
    : od::HeadDisplay(head, left, bottom, width, height)
{
    mSampleView.setMarkVisible(true);
    mSampleView.setMarkColor(GRAY7);
    setPointerLabel("P");
    for (int i = 0; i < kSlots; i++) { mGlow[i] = 0.0f; mGlowSrc[i] = 0; }
}

DiracHeadDisplay::~DiracHeadDisplay()
{
}

void DiracHeadDisplay::draw(od::FrameBuffer &fb)
{
    Dirac *h = grainHead();
    if (h == 0)
        return;

    od::Sample *pSample = h->getSample();
    if (pSample == 0)
    {
        od::HeadDisplay::draw(fb);
        return;
    }

    const int H = mHeight / 2;

    // Shade the analysis window currently being grabbed at the playhead.
    const int pos = h->getPosition();   // od::Head::mCurrentIndex (the playhead)
    mSampleView.setMarkedRegion(pos, pos + 512);

    // Base waveform + playhead marker.
    od::HeadDisplay::draw(fb);

    // A mini tick per grain at its source position. Active grains are held at
    // full height (well, envelope-scaled with a floor); after a grain ends its
    // tick fades over a few frames so short/triggered grains stay visible.
    int G = h->getGrainCount();
    if (G > kSlots) G = kSlots;
    for (int i = 0; i < G; i++)
    {
        if (h->isGrainActive(i))
        {
            // envelope-scaled but floored so it's always clearly visible
            float e = h->getGrainEnvelope(i);
            if (e < 0.35f) e = 0.35f;
            mGlow[i]    = e;
            mGlowSrc[i] = h->getGrainSrcIndex(i);
        }

        if (mGlow[i] > 0.04f)
        {
            int hgt = int(mGlow[i] * float(H));
            if (hgt < 3) hgt = 3;
            mSampleView.drawMiniPosition(fb, mGlowSrc[i], hgt, 0);
            mGlow[i] *= 0.85f;   // fade after the grain ends
        }
    }
}

} // namespace dirac
