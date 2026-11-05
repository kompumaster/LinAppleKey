#include "Driver.h"



void A1644ResetState(A1644_STATE* state)
{
	state->FakeFnActive = 0;
	state->ConsumerBits = 0;
	state->ConsumerBitsPrev = 0;
	state->VhfBits = 0;
	state->VhfBitsPrev = 0;
	state->PowerAction = PowerActionNone;
	state->PowerActionPrev = PowerActionNone;
}

// Apple Magic Keyboard (A1644), on both the Bluetooth and the wired (USB)
// transports. Driver.c hands over the same 9-byte view of the report for both:
//
//   [0]    modifier byte (bit 0 = LCtrl, 1 = LShift, 2 = LAlt, 3 = LGUI,
//          4 = RCtrl, 5 = RShift, 6 = RAlt, 7 = RGUI)
//   [1]    reserved
//   [2-7]  six key slots (key1..key6)
//   [8]    Apple vendor "special key" bits (0x1 = Eject, 0x2 = Fn)
//
// The buffer is modified in place and emitted upstream through the virtual
// HID device.

void ProcessA1644Buffer(A1644_STATE* state, VHF_DEVICE* vhf, BYTE* buf, ULONG size)
{
	BYTE* pModifier = &buf[0];
	BYTE* pKey1 = &buf[2];
	BYTE* pSpecialKey = &buf[8];
	BOOLEAN ejectPressed = (*pSpecialKey & 0x1) != 0;
	BOOLEAN rightCmdAsMenu = FALSE;
	UCHAR menuMask;

	state->ConsumerBits = 0;
	state->VhfBits = 0;
	state->PowerAction = PowerActionNone;

	// SwapFnCtrl mode
	if (g_dwSwapFnCtrl)
	{
		// Physical LCtrl pressed
		if (*pModifier & HidLCtrlMask)
		{
			if (!*pKey1) // And is it pressed alone?
				state->FakeFnActive = TRUE;

			*pModifier &= ~HidLCtrlMask; // Clear LCtrl modifier
		}
		else // Physical LCtrl not pressed
		{
			if (!*pKey1) // Only unset FakeFnActive when there is no other key still being pressed
				state->FakeFnActive = FALSE;
		}

		// Physical Fn pressed?
		if (*pSpecialKey & 0x2)
			*pModifier |= HidLCtrlMask; // Set LCtrl modifier
		else
			*pModifier &= ~HidLCtrlMask; // Clear LCtrl modifier
	}
	else // Not SwapFnCtrl mode
		state->FakeFnActive = (*pSpecialKey & 0x2) != 0; // Set FakeFnActive state based on physical Fn key state

	*pSpecialKey = 0; // Clear special key

	// On the right side one of the two modifiers acts as the Application
	// (context menu) key and the other one stays Right Alt:
	//   SwapRightCmdAlt=0 -> Right Command = menu, Right Option = Alt (default)
	//   SwapRightCmdAlt=1 -> Right Command = Alt,  Right Option = menu
	menuMask = g_dwSwapRightCmdAlt ? HidRAltMask : HidRCmdMask;
	rightCmdAsMenu = (g_dwRightCmdAsMenu && (*pModifier & menuMask)) != 0;
	if (rightCmdAsMenu)
		*pModifier &= ~menuMask; // Clear the modifier bit

	// Optionally process the Alt-Cmd swap. Left side only: the right-hand
	// modifiers are governed by RightCmdAsMenu / SwapRightCmdAlt above, where one
	// of them is the Application key and the other one stays Right Alt.
	if (g_dwSwapAltCmd)
	{
		if (*pModifier & HidLAltMask)
		{
			*pModifier &= ~HidLAltMask;
			*pModifier |= HidLCmdMask;
		}
		else if (*pModifier & HidLCmdMask)
		{
			*pModifier &= ~HidLCmdMask;
			*pModifier |= HidLAltMask;
		}
	}

	// Right Command becomes Alt, unless it has just become the menu key. After
	// the swap above, not before: that one turns a left Alt into the Super/Windows key.
	if (*pModifier & HidRCmdMask)
	{
		*pModifier &= ~HidRCmdMask;
		*pModifier |= HidLAltMask;
	}

	// Process FakeFn+[key] combination
	if (state->FakeFnActive && (*pKey1 || *pModifier))
	{
		switch (*pKey1)
		{
		case HidLeft: *pKey1 = HidHome; break;
		case HidRight: *pKey1 = HidEnd; break;
		case HidUp: *pKey1 = HidPgUp; break;
		case HidDown: *pKey1 = HidPgDown; break;
		case HidEnter: *pKey1 = HidInsert; break;
		case HidF1: state->VhfBits = VhfBitBrightnessDown; *pKey1 = 0; break;
		case HidF2: state->VhfBits = VhfBitBrightnessUp; *pKey1 = 0; break;
		case HidF3: *pKey1 = HidTab; *pModifier |= HidLCmdMask; break; // Super+Tab (Task View)
		case HidF4: *pKey1 = HidKeyZ; *pModifier |= HidLCmdMask; break; // Super+Z (Snap Layouts)
		case HidF5: state->PowerAction = PowerActionSleep; *pKey1 = 0; break; // System sleep
		case HidF6: state->PowerAction = PowerActionHibernate; *pKey1 = 0; break; // Hibernate
		case HidF7: state->ConsumerBits = ConsumerScanPrev; *pKey1 = 0; break;
		case HidF8: state->ConsumerBits = ConsumerPlayPause; *pKey1 = 0; break;
		case HidF9: state->ConsumerBits = ConsumerScanNext; *pKey1 = 0; break;
		case HidF10: state->VhfBits = VhfBitMute; *pKey1 = 0; break;
		case HidF11: state->VhfBits = VhfBitVolumeDown; *pKey1 = 0; break;
		case HidF12: state->VhfBits = VhfBitVolumeUp; *pKey1 = 0; break;
		case HidKeyP: *pKey1 = HidPrtScr; break;
		case HidKeyB: *pKey1 = HidPauseBreak; break;
		case HidKeyS: *pKey1 = HidScrLck; break;
		default:
			if (*pModifier & HidLCtrlMask) // Map Fn+LCtrl to RCtrl
			{
				*pModifier &= ~HidLCtrlMask; // Clear LCtrl
				*pModifier |= HidRCtrlMask; // Set RCtrl
			}
			else
				RtlZeroMemory(buf, size); // Ignore all other Fn+[key] combinations
			break;
		}
	}

	// Volume/Mute/Brightness go to the virtual HID device (see Vhf.c).
	if (state->VhfBits || state->VhfBitsPrev)
	{
		if (state->VhfBits != state->VhfBitsPrev)
			VhfSendReport(vhf, state->VhfBits);

		state->VhfBitsPrev = state->VhfBits;
	}

	// Media keys (Play/Pause, Next, Previous).
	if (state->ConsumerBits || state->ConsumerBitsPrev)
	{
		if (state->ConsumerBits != state->ConsumerBitsPrev)
			VhfSendConsumerReport(vhf, state->ConsumerBits);

		state->ConsumerBitsPrev = state->ConsumerBits;
	}

	// ISO keyboards disagree about these two: on the A1314/A1644 the key left of [1]
	// types [\] and the one left of [Z] types [`], while a PC ISO keyboard has
	// them the other way round. Swap them back unless the user turned it off.
	if (g_dwSwapGraveBackslash)
	{
		for (int i = 2; i < 8; i++)
		{
			if (buf[i] == HidGrave)
				buf[i] = HidNonUsBackslash;
			else if (buf[i] == HidNonUsBackslash)
				buf[i] = HidGrave;
		}
	}

	// Append the keys generated by the driver itself into the first free slot
	if (ejectPressed)
	{
		for (int i = 2; i < 8; i++)
		{
			if (!buf[i])
			{
				buf[i] = HidDel; // Eject -> Del
				break;
			}
		}
	}

	if (rightCmdAsMenu)
	{
		for (int i = 2; i < 8; i++)
		{
			if (!buf[i])
			{
				buf[i] = HidMenu; // Right Command -> Application (context menu) key
				break;
			}
		}
	}

	// Fn+F5 / Fn+F6 -> system sleep / hibernate. Edge triggered, see Power.c.
	if (state->PowerAction != state->PowerActionPrev)
	{
		if (state->PowerAction != PowerActionNone)
			WinAppleKeyRequestPowerAction(vhf, state->PowerAction);

		state->PowerActionPrev = state->PowerAction;
	}

	VhfSendKeyboardReport(vhf, buf[0], &buf[2]);
}
