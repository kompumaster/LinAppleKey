#include "Driver.h"

// Apple Wireless Keyboard (A1314), connected over Bluetooth.
//
// Unlike the Magic Keyboard (A1644), which reports fixed 11-byte input
// reports, the A1314 emits two different kinds of L2CAP ACL transfers on
// the Bluetooth HID interrupt channel:
//
//   * 3-byte  "Apple vendor" special key reports. Only the low bits of the
//             last byte are used here: 0x8 = Eject, 0x10 = Fn.
//   * 10-byte regular keyboard reports ("A1 01 <modifier> <reserved>
//             <key1>..<key6>").
//
// Both are rewritten in place into a well formed 10-byte keyboard report so
// that the input layer can process them further up the stack. This also
// guarantees that KdPrintBuffer()/logic below never sees a stale buffer.


void A1314ResetState(A1314_STATE* state)
{
	state->FakeFnActive = 0;
	state->RightCmdAsMenu = FALSE;
	state->ConsumerBits = 0;
	state->ConsumerBitsPrev = 0;
	state->VhfBits = 0;
	state->VhfBitsPrev = 0;
	state->PowerAction = PowerActionNone;
	state->PowerActionPrev = PowerActionNone;
	RtlZeroMemory(state->TmpBuf, sizeof(state->TmpBuf));
	state->SpecialKey = 0;
	state->SpecialKeyModifier = 0;
}

void ProcessA1314Block(A1314_STATE* state, VHF_DEVICE* vhf, PBRB pbrb)
{
	PUCHAR buf = (PUCHAR)pbrb->BrbL2caAclTransfer.Buffer;
	ULONG size = pbrb->BrbL2caAclTransfer.BufferSize;
	UCHAR modifiers;
	UCHAR menuMask;

	DebugPrintBuffer("ProcessA1314Block(): <= ", buf, size);

	if (size == 3) // Apple vendor special key report (Eject/Fn)
	{
		state->SpecialKey = 0;
		state->SpecialKeyModifier = 0;

		if (buf[2] & 0x8) // Eject pressed
			state->SpecialKey = HidDel; // Map to Del

		if (g_dwSwapFnCtrl)
		{
			if (buf[2] & 0x10) // Fn pressed
				state->SpecialKeyModifier |= HidLCtrlMask; // Map to LCtrl
		}
		else
			state->FakeFnActive = (buf[2] & 0x10) != 0; // Set FakeFn state from the physical Fn state
	}
	else if (size == 10) // Regular keyboard report
	{
		RtlCopyMemory(state->TmpBuf, buf, 10);
		state->ConsumerBits = 0;
		state->VhfBits = 0;
		state->PowerAction = PowerActionNone;

		// On the right side one of the two modifiers acts as the Application
		// (context menu) key and the other one stays Right Alt:
		//   SwapRightCmdAlt=0 -> Right Command = menu, Right Option = Alt (default)
		//   SwapRightCmdAlt=1 -> Right Command = Alt,  Right Option = menu
		menuMask = g_dwSwapRightCmdAlt ? HidRAltMask : HidRCmdMask;
		state->RightCmdAsMenu = (g_dwRightCmdAsMenu && (state->TmpBuf[2] & menuMask)) != 0;
		if (state->RightCmdAsMenu)
			state->TmpBuf[2] &= ~menuMask; // Clear the modifier bit

		if (g_dwSwapFnCtrl)
		{
			// Physical LCtrl pressed (and alone) drives the fake Fn state
			if ((state->TmpBuf[2] & HidLCtrlMask) && !state->TmpBuf[4])
				state->FakeFnActive = TRUE;
			else if (!state->TmpBuf[4])
				state->FakeFnActive = FALSE;

			state->TmpBuf[2] &= ~HidLCtrlMask; // Clear LCtrl modifier
		}

		// Optionally process the Alt-Cmd swap. Left side only: the right-hand
		// modifiers are governed by RightCmdAsMenu / SwapRightCmdAlt below, where
		// one of them is the Application key and the other one stays Right Alt.
		if (g_dwSwapAltCmd)
		{
			if (state->TmpBuf[2] & HidLAltMask)
			{
				state->TmpBuf[2] &= ~HidLAltMask;
				state->TmpBuf[2] |= HidLCmdMask;
			}
			else if (state->TmpBuf[2] & HidLCmdMask)
			{
				state->TmpBuf[2] &= ~HidLCmdMask;
				state->TmpBuf[2] |= HidLAltMask;
			}
		}

		// Right Command becomes Alt, unless it has just become the menu key.
		// This has to run *after* the Alt/Cmd swap above: that one turns a left
		// Alt into the Super/Windows key, so converting Right Command into a left Alt
		// any earlier would give you Super+Tab instead of Alt+Tab. Left Alt is also
		// the only one that works everywhere - Right Alt is often mapped to AltGr/ISO_Level3_Shift.
		if (state->TmpBuf[2] & HidRCmdMask)
		{
			state->TmpBuf[2] &= ~HidRCmdMask;
			state->TmpBuf[2] |= HidLAltMask;
		}

		// Process FakeFn+[key] combination
		if (state->FakeFnActive && (state->TmpBuf[4] || state->TmpBuf[2]))
		{
			switch (state->TmpBuf[4])
			{
			case HidLeft: state->TmpBuf[4] = HidHome; break;
			case HidRight: state->TmpBuf[4] = HidEnd; break;
			case HidUp: state->TmpBuf[4] = HidPgUp; break;
			case HidDown: state->TmpBuf[4] = HidPgDown; break;
			case HidEnter: state->TmpBuf[4] = HidInsert; break;
			// Fn+F1/F2 -> Brightness Decrement/Increment via virtual HID device.
			case HidF1: state->VhfBits = VhfBitBrightnessDown; state->TmpBuf[4] = 0; break;
			case HidF2: state->VhfBits = VhfBitBrightnessUp; state->TmpBuf[4] = 0; break;
			case HidF3: state->TmpBuf[4] = HidTab; state->TmpBuf[2] |= HidLCmdMask; break; // Super+Tab (Task View / Overview)
			case HidF4: state->TmpBuf[4] = HidKeyZ; state->TmpBuf[2] |= HidLCmdMask; break; // Super+Z (Snap Layouts / Tiling)
			case HidF5: state->PowerAction = PowerActionSleep; state->TmpBuf[4] = 0; break; // System sleep
			case HidF6: state->PowerAction = PowerActionHibernate; state->TmpBuf[4] = 0; break; // Hibernate
			case HidF7: state->ConsumerBits = ConsumerScanPrev; state->TmpBuf[4] = 0; break; // Scan Previous Track
			case HidF8: state->ConsumerBits = ConsumerPlayPause; state->TmpBuf[4] = 0; break; // Play/Pause
			case HidF9: state->ConsumerBits = ConsumerScanNext; state->TmpBuf[4] = 0; break; // Scan Next Track
			case HidF10: state->VhfBits = VhfBitMute; state->TmpBuf[4] = 0; break; // Volume Mute
			case HidF11: state->VhfBits = VhfBitVolumeDown; state->TmpBuf[4] = 0; break; // Volume Down
			case HidF12: state->VhfBits = VhfBitVolumeUp; state->TmpBuf[4] = 0; break; // Volume Up
			case HidKeyP: state->TmpBuf[4] = HidPrtScr; break;
			case HidKeyB: state->TmpBuf[4] = HidPauseBreak; break;
			case HidKeyS: state->TmpBuf[4] = HidScrLck; break;
			default:
				// Map Fn+LCtrl to RCtrl; ignore all other Fn+[key] combinations.
				// NOTE: in SwapFnCtrl mode the logical Left Ctrl (i.e. the physical
				// Fn key) lives in SpecialKeyModifier, not in TmpBuf, because the
				// 10-byte reports keep overwriting TmpBuf - hence the OR below.
				if ((state->TmpBuf[2] | state->SpecialKeyModifier) & HidLCtrlMask)
				{
					state->SpecialKeyModifier &= ~HidLCtrlMask; // Clear the logical LCtrl
					state->TmpBuf[2] &= ~HidLCtrlMask; // Clear LCtrl
					state->TmpBuf[2] |= HidRCtrlMask; // Set RCtrl
				}
				else
					RtlZeroMemory(state->TmpBuf + 2, sizeof(state->TmpBuf) - 2);
				break;
			}
		}
	}

	// Volume/Mute/Brightness go to the virtual HID device (see Vhf.c). Level
	// triggered, so the cleared state is submitted on release as well.
	if (state->VhfBits || state->VhfBitsPrev)
	{
		if (state->VhfBits != state->VhfBitsPrev)
			VhfSendReport(vhf, state->VhfBits);

		state->VhfBitsPrev = state->VhfBits;
	}

	// Fn+F5 / Fn+F6 -> system sleep / hibernate. Edge triggered, see Power.c.
	if (state->PowerAction != state->PowerActionPrev)
	{
		if (state->PowerAction != PowerActionNone)
			WinAppleKeyRequestPowerAction(vhf, state->PowerAction);

		state->PowerActionPrev = state->PowerAction;
	}

	// The media keys are sent as a Consumer Control report instead of the usual
	// keyboard report. HID input reports are level triggered, not events, so the
	// previous state is re-sent (with the bits cleared) on release - otherwise
	// the OS would never see the button go up.
	if (state->ConsumerBits || state->ConsumerBitsPrev)
	{
		pbrb->BrbL2caAclTransfer.BufferSize = 3;
		buf[0] = 0xa1;
		buf[1] = ConsumerReportId;
		buf[2] = state->ConsumerBits;

		if (state->ConsumerBits != state->ConsumerBitsPrev)
			VhfSendConsumerReport(vhf, state->ConsumerBits);

		state->ConsumerBitsPrev = state->ConsumerBits;

		DebugPrintBuffer("ProcessA1314Block(): => consumer ", buf, 3);
		return;
	}

	// ISO keyboards disagree about these two: on the A1314 the key left of [1]
	// types [\] and the one left of [Z] types [`], while a PC ISO keyboard has
	// them the other way round. Swap them back unless the user turned it off.
	if (g_dwSwapGraveBackslash)
	{
		int i;
		for (i = 4; i < 10; i++)
		{
			if (state->TmpBuf[i] == HidGrave)
				state->TmpBuf[i] = HidNonUsBackslash;
			else if (state->TmpBuf[i] == HidNonUsBackslash)
				state->TmpBuf[i] = HidGrave;
		}
	}

	// Always emit a well formed 10-byte keyboard report upstream.
	pbrb->BrbL2caAclTransfer.BufferSize = 10;
	RtlZeroMemory(buf, 10);

	// Build the modifier byte. In SwapFnCtrl mode the physical Fn key (which is
	// the logical Left Ctrl) is tracked in SpecialKeyModifier rather than in
	// TmpBuf, because the 10-byte reports keep overwriting TmpBuf. That is also
	// why the logical Fn + logical Left Ctrl -> Right Ctrl translation has to be
	// done here as well: when no other key is pressed the switch above is not
	// even entered.
	modifiers = state->TmpBuf[2] | state->SpecialKeyModifier;
	if (state->FakeFnActive && (modifiers & HidLCtrlMask))
	{
		modifiers &= ~HidLCtrlMask; // Clear the logical Left Ctrl
		modifiers |= HidRCtrlMask; // Set Right Ctrl
	}

	buf[0] = 0xa1;
	buf[1] = 0x1;
	buf[2] = modifiers;
	buf[3] = state->TmpBuf[3];

	int k = 4;
	while (k < 10 && state->TmpBuf[k] != 0)
	{
		buf[k] = state->TmpBuf[k];
		k++;
	}

	// Append the keys generated by the driver itself into the first free slot
	if (k < 10 && state->SpecialKey)
		buf[k++] = state->SpecialKey; // Eject -> Del
	if (k < 10 && state->RightCmdAsMenu)
		buf[k++] = HidMenu; // Right Command -> Application (context menu) key

	VhfSendKeyboardReport(vhf, buf[2], &buf[4]);

	DebugPrintBuffer("ProcessA1314Block(): => ", buf, pbrb->BrbL2caAclTransfer.BufferSize);
}
