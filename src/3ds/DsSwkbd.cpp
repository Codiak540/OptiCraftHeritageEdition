// DsSwkbd.cpp -- launches the system software keyboard for one text field.
#ifdef CTR_PLATFORM

#include "3ds/DsSwkbd.h"

#include <3ds.h>

#include <cstddef>
#include <vector>

#include "platform/Log.h"

namespace
{
// swkbd counts its limit in UTF-16 code units while the result it writes is
// UTF-8: one BMP unit is at most 3 bytes and a surrogate pair (2 units) is 4,
// so 4 bytes per unit covers anything the applet can hand back. The slack is
// for the terminator even if the applet filled the buffer to the brim -- the
// text is read with strlen semantics, so a missing NUL would read past the
// end.
constexpr std::size_t kBytesPerCodeUnit = 4;
constexpr std::size_t kTerminatorSlack = 8;

// SwkbdState::max_text_len is a u16.
constexpr int kMaxTextLength = 0xFFFF;
} // namespace

DsSwkbdResult dsSwkbdOpen(const std::string& initial, int maxLength)
{
	DsSwkbdResult result;

	if (maxLength < 1)
		maxLength = 1;
	if (maxLength > kMaxTextLength)
		maxLength = kMaxTextLength;

	SwkbdState swkbd;
	swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, maxLength);
	// Accept everything: the field enforces its own rules anyway
	// (ChatAllowedCharacters plus the maxStringLength clamp, applied on the
	// result), and rejecting input inside the applet would only stop the
	// player from editing a value the game is going to trim itself.
	swkbdSetValidation(&swkbd, SWKBD_ANYTHING, 0, 0);
	swkbdSetFeatures(&swkbd,
	                 SWKBD_DEFAULT_QWERTY | SWKBD_DARKEN_TOP_SCREEN | SWKBD_ALLOW_HOME);
	// Cancel on the left (its own button label, discarding the text), OK on
	// the right (submitting it) -- the dialog layout the player expects.
	swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
	swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "OK", true);
	if (!initial.empty())
		swkbdSetInitialText(&swkbd, initial.c_str());

	std::vector<char> buffer(
		static_cast<std::size_t>(maxLength) * kBytesPerCodeUnit + kTerminatorSlack, '\0');
	swkbdInputText(&swkbd, buffer.data(), buffer.size());

	// The button identifier comes back through swkbdGetResult as the
	// dialog's own result codes (a two-button dialog reports D1_CLICK0 for
	// the left button, D1_CLICK1 for the right one).
	switch (swkbdGetResult(&swkbd))
	{
	case SWKBD_D1_CLICK1:
		result.outcome = DsSwkbdOutcome::Confirmed;
		result.text.assign(buffer.data());
		return result;
	case SWKBD_D1_CLICK0:
		result.outcome = DsSwkbdOutcome::Cancelled;
		return result;
	case SWKBD_HOMEPRESSED:
	case SWKBD_RESETPRESSED:
	case SWKBD_POWERPRESSED:
		result.outcome = DsSwkbdOutcome::Cancelled;
		return result;
	default:
		// SWKBD_NONE / SWKBD_INVALID_INPUT / SWKBD_OUTOFMEM: no dialog. One
		// line here, then the caller pins the session to the fallback panel
		// instead of retrying the launch for every field it meets.
		MC_LOG_WARN("input",
			"3ds: software keyboard unavailable (swkbd result %d)\n",
			static_cast<int>(swkbdGetResult(&swkbd)));
		result.outcome = DsSwkbdOutcome::Unavailable;
		return result;
	}
}

#endif // CTR_PLATFORM
