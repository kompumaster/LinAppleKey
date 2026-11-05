#include "Driver.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

///////////////////////////////////////////////////////////////////////////////
// Virtual HID / Input device (Linux uinput counterpart to Windows VHF)
//
// Exposes "LinAppleKey Virtual HID Device" with:
//   * Standard 104/105/JIS keyboard keys (Usage Page 0x07)
//   * Consumer Control keys: Mute, Volume Up/Down, Brightness Up/Down,
//     Play/Pause, Scan Next Track, Scan Previous Track (Usage Page 0x0C)
//   * Power keys: Sleep (KEY_SLEEP), Hibernate (KEY_SUSPEND)
//   * LEDs (LED_CAPSL, etc.) so the physical Caps Lock LED stays in sync.
///////////////////////////////////////////////////////////////////////////////

#define unk KEY_UNKNOWN

// Standard Linux kernel hid-input.c translation table for Usage Page 0x07.
static const unsigned short g_HidKeyboard[256] = {
	  0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
	 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
	  4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
	 27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
	 65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
	105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
	 72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
	191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
	115,114,unk,unk,unk,121,unk, 89, 93,124, 92, 94, 95,unk,unk,unk,
	122,123, 90, 91, 85,unk,unk,unk,unk,unk,unk,unk,111,unk,unk,unk,
	unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,
	unk,unk,unk,unk,unk,unk,179,180,unk,unk,unk,unk,unk,unk,unk,unk,
	unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,unk,
	unk,unk,unk,unk,unk,unk,unk,unk,111,unk,unk,unk,unk,unk,unk,unk,
	 29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
	150,158,159,128,136,177,178,176,142,152,173,140,unk,unk,unk,unk
};

static const unsigned short g_ModifierKeycodes[8] = {
	KEY_LEFTCTRL,   // bit 0: 0x01 HidLCtrlMask
	KEY_LEFTSHIFT,  // bit 1: 0x02 HidLShiftMask
	KEY_LEFTALT,    // bit 2: 0x04 HidLAltMask
	KEY_LEFTMETA,   // bit 3: 0x08 HidLCmdMask
	KEY_RIGHTCTRL,  // bit 4: 0x10 HidRCtrlMask
	KEY_RIGHTSHIFT, // bit 5: 0x20 HidRShiftMask
	KEY_RIGHTALT,   // bit 6: 0x40 HidRAltMask
	KEY_RIGHTMETA   // bit 7: 0x80 HidRCmdMask
};

static void EmitEvent(VHF_DEVICE* dev, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event ev;

	if (dev->Sink)
	{
		dev->Sink(dev->SinkContext, type, code, value);
		return;
	}

	if (dev->Fd < 0)
		return;

	if (type == EV_KEY)
		DebugPrint("uinput key %u %d\n", (unsigned)code, value);

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;

	ssize_t written = write(dev->Fd, &ev, sizeof(ev));
	if (written != (ssize_t)sizeof(ev))
	{
		if (written < 0)
			LogError("Vhf: write(uinput) failed: %s", strerror(errno));
		else
			LogError("Vhf: short write(uinput): %zd of %zu bytes",
				written, sizeof(ev));
	}
}

static void EmitSyn(VHF_DEVICE* dev)
{
	EmitEvent(dev, EV_SYN, SYN_REPORT, 0);
}

static BOOLEAN KeyInList(UCHAR hidKey, const UCHAR* list6)
{
	for (int i = 0; i < 6; i++)
	{
		if (list6[i] == hidKey)
			return TRUE;
	}
	return FALSE;
}

VOID VhfInitialize(VHF_DEVICE* dev)
{
	memset(dev, 0, sizeof(*dev));
	dev->Fd = -1;
}

int VhfOpenUinputNode(VOID)
{
	int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0)
		return fd;

	int firstErr = errno;
	fd = open("/dev/input/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0)
		return fd;

	if (errno == ENOENT)
		errno = firstErr;
	return -1;
}

int VhfInit(VHF_DEVICE* dev)
{
	struct uinput_setup usetup;
	int fd;

	if (dev->Fd >= 0)
		VhfDeinit(dev);

	fd = VhfOpenUinputNode();
	if (fd < 0)
	{
		int err = errno;
		LogError("cannot open /dev/uinput: %s", strerror(err));
		if (err == EACCES || err == EPERM)
			LogWarn("hint: run as root or grant access to /dev/uinput");
		return -1;
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	ioctl(fd, UI_SET_EVBIT, EV_REP);
	ioctl(fd, UI_SET_EVBIT, EV_LED);

	ioctl(fd, UI_SET_LEDBIT, LED_NUML);
	ioctl(fd, UI_SET_LEDBIT, LED_CAPSL);
	ioctl(fd, UI_SET_LEDBIT, LED_SCROLLL);
	ioctl(fd, UI_SET_LEDBIT, LED_COMPOSE);
	ioctl(fd, UI_SET_LEDBIT, LED_KANA);

	// Standard keyboard usages
	for (int i = 0; i < 256; i++)
	{
		unsigned short code = g_HidKeyboard[i];
		if (code != 0 && code != KEY_UNKNOWN)
			ioctl(fd, UI_SET_KEYBIT, code);
	}

	// Modifiers
	for (int i = 0; i < 8; i++)
		ioctl(fd, UI_SET_KEYBIT, g_ModifierKeycodes[i]);

	// Consumer Control & Power keys
	ioctl(fd, UI_SET_KEYBIT, KEY_MUTE);
	ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEUP);
	ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN);
	ioctl(fd, UI_SET_KEYBIT, KEY_BRIGHTNESSUP);
	ioctl(fd, UI_SET_KEYBIT, KEY_BRIGHTNESSDOWN);
	ioctl(fd, UI_SET_KEYBIT, KEY_PLAYPAUSE);
	ioctl(fd, UI_SET_KEYBIT, KEY_NEXTSONG);
	ioctl(fd, UI_SET_KEYBIT, KEY_PREVIOUSSONG);
	ioctl(fd, UI_SET_KEYBIT, KEY_SLEEP);
	ioctl(fd, UI_SET_KEYBIT, KEY_SUSPEND);
	ioctl(fd, UI_SET_KEYBIT, KEY_COMPOSE);

	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor  = 0x1209; // pid.codes, not Apple: avoid ID collisions
	usetup.id.product = 0xA131;
	usetup.id.version = 0x0001;
	snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "LinAppleKey Virtual HID Device");

	if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0)
	{
		// Fallback for older kernels without UI_DEV_SETUP
		struct uinput_user_dev udev;
		memset(&udev, 0, sizeof(udev));
		snprintf(udev.name, UINPUT_MAX_NAME_SIZE, "LinAppleKey Virtual HID Device");
		udev.id.bustype = BUS_USB;
		udev.id.vendor  = 0x1209;
		udev.id.product = 0xA131;
		udev.id.version = 0x0001;
		if (write(fd, &udev, sizeof(udev)) < 0)
		{
			LogError("failed to configure uinput device: %s", strerror(errno));
			close(fd);
			return -1;
		}
	}

	if (ioctl(fd, UI_DEV_CREATE) < 0)
	{
		LogError("UI_DEV_CREATE failed: %s", strerror(errno));
		close(fd);
		return -1;
	}

	dev->Fd = fd;
	DebugPrint("VhfInit(): created 'LinAppleKey Virtual HID Device'\n");
	return 0;
}

VOID VhfDeinit(VHF_DEVICE* dev)
{
	if (dev->Fd >= 0)
	{
		VhfReleaseAll(dev);
		ioctl(dev->Fd, UI_DEV_DESTROY);
		close(dev->Fd);
		dev->Fd = -1;
	}
}

int VhfGetFd(const VHF_DEVICE* dev)
{
	return dev->Fd;
}

VOID VhfSetEventSink(VHF_DEVICE* dev, VHF_EVENT_SINK sink, PVOID context)
{
	dev->Sink = sink;
	dev->SinkContext = context;
}

// Level triggered: sends Volume/Mute/Brightness key states when bits change.
VOID VhfSendReport(VHF_DEVICE* dev, UCHAR bits)
{
	UCHAR changed = dev->PrevVhfBits ^ bits;
	if (!changed)
		return;

	if (changed & VhfBitMute)
		EmitEvent(dev, EV_KEY, KEY_MUTE, (bits & VhfBitMute) ? 1 : 0);
	if (changed & VhfBitVolumeUp)
		EmitEvent(dev, EV_KEY, KEY_VOLUMEUP, (bits & VhfBitVolumeUp) ? 1 : 0);
	if (changed & VhfBitVolumeDown)
		EmitEvent(dev, EV_KEY, KEY_VOLUMEDOWN, (bits & VhfBitVolumeDown) ? 1 : 0);
	if (changed & VhfBitBrightnessUp)
		EmitEvent(dev, EV_KEY, KEY_BRIGHTNESSUP, (bits & VhfBitBrightnessUp) ? 1 : 0);
	if (changed & VhfBitBrightnessDown)
		EmitEvent(dev, EV_KEY, KEY_BRIGHTNESSDOWN, (bits & VhfBitBrightnessDown) ? 1 : 0);

	EmitSyn(dev);
	dev->PrevVhfBits = bits;
}

// Level triggered: sends Play/Pause, Scan Next Track, Scan Previous Track.
VOID VhfSendConsumerReport(VHF_DEVICE* dev, UCHAR bits)
{
	UCHAR changed = dev->PrevConsumerBits ^ bits;
	if (!changed)
		return;

	if (changed & ConsumerPlayPause)
		EmitEvent(dev, EV_KEY, KEY_PLAYPAUSE, (bits & ConsumerPlayPause) ? 1 : 0);
	if (changed & ConsumerScanNext)
		EmitEvent(dev, EV_KEY, KEY_NEXTSONG, (bits & ConsumerScanNext) ? 1 : 0);
	if (changed & ConsumerScanPrev)
		EmitEvent(dev, EV_KEY, KEY_PREVIOUSSONG, (bits & ConsumerScanPrev) ? 1 : 0);

	EmitSyn(dev);
	dev->PrevConsumerBits = bits;
}

static BOOLEAN IsKeyboardErrorUsage(UCHAR key)
{
	return key == HidKeyErrOvf ||
	       key == HidKeyPostFail ||
	       key == HidKeyErrorUndefined;
}

// Translates a 6KRO HID keyboard report (modifiers + 6 key slots) into Linux
// input events.
//
// Synthetic combinations like Fn+F3 -> Super+Tab and Fn+F4 -> Super+Z set the
// modifier bit and the key in the very same HID report. Emitting newly pressed
// modifiers in their own SYN frame before pressing keys guarantees that
// Wayland compositors (KWin, Mutter) see the modifier active first.
VOID VhfSendKeyboardReport(VHF_DEVICE* dev, UCHAR modifiers, const UCHAR* keys6)
{
	UCHAR modDiff = dev->PrevModifiers ^ modifiers;
	BOOLEAN modPressed = FALSE;
	BOOLEAN keyChanged = FALSE;
	BOOLEAN modReleased = FALSE;
	BOOLEAN errorReport = FALSE;

	for (int i = 0; i < 6; i++)
	{
		if (IsKeyboardErrorUsage(keys6[i]))
		{
			errorReport = TRUE;
			break;
		}
	}

	// 1. Newly pressed modifiers first
	for (int bit = 0; bit < 8; bit++)
	{
		UCHAR mask = (UCHAR)(1u << bit);
		if ((modDiff & mask) && (modifiers & mask))
		{
			EmitEvent(dev, EV_KEY, g_ModifierKeycodes[bit], 1);
			modPressed = TRUE;
		}
	}
	if (modPressed)
		EmitSyn(dev);

	if (errorReport)
	{
		// The key array is invalid. Keep the previous key state and trust
		// only the modifier byte; the next valid report releases keys that
		// were let go meanwhile.
		for (int bit = 0; bit < 8; bit++)
		{
			UCHAR mask = (UCHAR)(1u << bit);
			if ((modDiff & mask) && !(modifiers & mask))
			{
				EmitEvent(dev, EV_KEY, g_ModifierKeycodes[bit], 0);
				modReleased = TRUE;
			}
		}
		if (modReleased)
			EmitSyn(dev);

		dev->PrevModifiers = modifiers;
		return;
	}

	// 2. Released regular keys
	for (int i = 0; i < 6; i++)
	{
		UCHAR oldKey = dev->PrevKeys[i];
		if (oldKey != HidKeyNone && !KeyInList(oldKey, keys6))
		{
			unsigned short code = g_HidKeyboard[oldKey];
			if (code != 0 && code != KEY_UNKNOWN)
			{
				EmitEvent(dev, EV_KEY, code, 0);
				keyChanged = TRUE;
			}
		}
	}

	// 3. Newly pressed regular keys
	for (int i = 0; i < 6; i++)
	{
		UCHAR newKey = keys6[i];
		if (newKey != HidKeyNone && !KeyInList(newKey, dev->PrevKeys))
		{
			unsigned short code = g_HidKeyboard[newKey];
			if (code != 0 && code != KEY_UNKNOWN)
			{
				EmitEvent(dev, EV_KEY, code, 1);
				keyChanged = TRUE;
			}
		}
	}
	if (keyChanged)
		EmitSyn(dev);

	// 4. Newly released modifiers last
	for (int bit = 0; bit < 8; bit++)
	{
		UCHAR mask = (UCHAR)(1u << bit);
		if ((modDiff & mask) && !(modifiers & mask))
		{
			EmitEvent(dev, EV_KEY, g_ModifierKeycodes[bit], 0);
			modReleased = TRUE;
		}
	}
	if (modReleased)
		EmitSyn(dev);

	dev->PrevModifiers = modifiers;
	RtlCopyMemory(dev->PrevKeys, keys6, 6);
}

// Edge-triggered tap for Sleep / Hibernate keys.
VOID VhfSendPowerKey(VHF_DEVICE* dev, POWER_ACTION action)
{
	uint16_t code = 0;

	if (action == PowerActionSleep)
		code = KEY_SLEEP;
	else if (action == PowerActionHibernate)
		code = KEY_SUSPEND;

	if (!code)
		return;

	EmitEvent(dev, EV_KEY, code, 1);
	EmitSyn(dev);
	EmitEvent(dev, EV_KEY, code, 0);
	EmitSyn(dev);
}

VOID VhfReleaseAll(VHF_DEVICE* dev)
{
	UCHAR emptyKeys[6] = { 0 };

	if (dev->PrevVhfBits)
		VhfSendReport(dev, 0);
	if (dev->PrevConsumerBits)
		VhfSendConsumerReport(dev, 0);
	if (dev->PrevModifiers || dev->PrevKeys[0] || dev->PrevKeys[1] || dev->PrevKeys[2] ||
	    dev->PrevKeys[3] || dev->PrevKeys[4] || dev->PrevKeys[5])
		VhfSendKeyboardReport(dev, 0, emptyKeys);
}
