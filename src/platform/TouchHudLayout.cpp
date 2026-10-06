#include "platform/TouchHudLayout.h"

namespace touchHud
{
// The one program-wide definition behind the header's extern. This was an
// inline variable in the header until DsInput.cpp's include of the header
// ended up inside its anonymous namespace, which gave DsInput's translation
// unit a private _GLOBAL__N_1::touchHud:: copy that nothing ever wrote --
// legally const-foldable, so the touch hit-test froze on the default
// column while the drawing followed the real variable (2026-10 hardware
// report: buttons drawn swapped, still tappable on the old side). The
// extern plus this single definition keeps every translation unit on one
// object and turns a future namespace capture into a link error.
bool swappedSides = false;
}
