#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

	///////////////////////////////////////////////////////////////////////////////
	// Windows-compatible types for shared translation modules (A1314.c / A1644.c)
	//

	typedef uint8_t  BYTE;
	typedef uint8_t  UCHAR;
	typedef uint16_t USHORT;
	typedef uint32_t ULONG;
	typedef uint32_t DWORD;
	typedef int32_t  LONG;
	typedef uint8_t  BOOLEAN;
	typedef void     VOID;
	typedef void*    PVOID;
	typedef char*    PCHAR;
	typedef uint8_t* PUCHAR;

	#ifndef TRUE
		#define TRUE 1
	#endif
	#ifndef FALSE
		#define FALSE 0
	#endif

	#define RtlCopyMemory(dst, src, len) memcpy((dst), (src), (len))
	#define RtlZeroMemory(dst, len)      memset((dst), 0, (len))

	typedef enum _POWER_ACTION
	{
		PowerActionNone = 0,
		PowerActionSleep,
		PowerActionHibernate
	} POWER_ACTION;

	typedef struct _BRB
	{
		struct
		{
			PVOID Buffer;
			ULONG BufferSize;
		} BrbL2caAclTransfer;
	} BRB, *PBRB;

	typedef enum _APPLE_KB_MODEL
	{
		KbModelNone = 0,
		KbModelA1314, // Apple Wireless Keyboard (A1314, A1255, original BT)
		KbModelA1644  // Apple Magic Keyboard (A1644, A1843 USB & BT)
	} APPLE_KB_MODEL;

	// Resolved physical keyboard, used by --capture.
	typedef struct _CAPTURE_TARGET
	{
		char           SysName[128];
		char           SysDir[PATH_MAX];
		char           HidrawPath[PATH_MAX];
		APPLE_KB_MODEL Model;
		unsigned int   Bus;
		unsigned int   Vid;
		unsigned int   Pid;
	} CAPTURE_TARGET;

	///////////////////////////////////////////////////////////////////////////////
	// Macro definitions
	//

	#define DRIVERNAME "LinAppleKey"
	#define LINAPPLEKEY_VERSION "0.1.1.13"

	typedef enum _LOG_LEVEL
	{
		LogLevelError = 0,
		LogLevelWarn,
		LogLevelInfo,
		LogLevelDebug
	} LOG_LEVEL;

	VOID LogInit(VOID);
	VOID LogSetLevel(LOG_LEVEL level);
	VOID LogMessage(LOG_LEVEL level, const char* fmt, ...)
		__attribute__((format(printf, 2, 3)));
	VOID LogHex(LOG_LEVEL level, const char* text, const unsigned char* buffer, size_t length);

	#define LogError(...) LogMessage(LogLevelError, __VA_ARGS__)
	#define LogWarn(...)  LogMessage(LogLevelWarn, __VA_ARGS__)
	#define LogInfo(...)  LogMessage(LogLevelInfo, __VA_ARGS__)
	#define LogDebug(...) LogMessage(LogLevelDebug, __VA_ARGS__)

	#define DebugPrint(fmt, ...) LogDebug(fmt, ##__VA_ARGS__)

	#define DebugPrintBuffer(text, buffer, length) \
		KdPrintBuffer(text, (PUCHAR)(buffer), (ULONG)(length))

	///////////////////////////////////////////////////////////////////////////////
	// Globals

	extern DWORD g_dwSwapAltCmd;
	extern DWORD g_dwSwapFnCtrl;
	extern DWORD g_dwRightCmdAsMenu;
	extern DWORD g_dwSwapRightCmdAlt;
	extern DWORD g_dwSwapGraveBackslash;
	extern DWORD g_dwDirectPowerAction;
	extern BOOLEAN g_bPowerKeysEnabled;
	extern BOOLEAN g_bGrabEnabled;
	extern BOOLEAN g_bReplayMode;
	extern volatile sig_atomic_t g_StopRequested;

	enum HidCodes
	{
		HidKeyNone = 0x0, // No key pressed
		HidKeyErrOvf = 0x1, // Keyboard Error Roll Over - used for all slots if too many keys are pressed
		HidKeyPostFail = 0x2, // Keyboard POST Fail
		HidKeyErrorUndefined = 0x3, // Keyboard Error Undefined
		HidKeyB = 0x5,
		HidKeyP = 0x13,
		HidKeyS = 0x16,
		HidKeyT = 0x17,
		HidKeyZ = 0x1d,
		HidTab = 0x2b,
		HidGrave = 0x35, // the ` and ~ key
		HidF1 = 0x3a,
		HidF2 = 0x3b,
		HidF3 = 0x3c,
		HidF4 = 0x3d,
		HidF5 = 0x3e,
		HidF6 = 0x3f,
		HidF7 = 0x40,
		HidF8 = 0x41,
		HidF9 = 0x42,
		HidF10 = 0x43,
		HidF11 = 0x44,
		HidF12 = 0x45,
		HidNonUsBackslash = 0x64, // the ISO key that types \ and |
		HidF13 = 0x68,
		HidF14 = 0x69,
		HidF15 = 0x6a,
		HidF16 = 0x6b,
		HidF17 = 0x6c,
		HidF18 = 0x6d,
		HidF19 = 0x6e,
		HidF20 = 0x6f,
		HidF21 = 0x70,
		HidF22 = 0x71,
		HidF23 = 0x72,
		HidF24 = 0x73,
		HidDel = 0x4c,
		HidLeft = 0x50,
		HidHome = 0x4a,
		HidRight = 0x4f,
		HidEnd = 0x4d,
		HidUp = 0x52,
		HidPgUp = 0x4b,
		HidDown = 0x51,
		HidPgDown = 0x4e,
		HidEnter = 0x28,
		HidPrtScr = 0x46,
		HidScrLck = 0x47,
		HidPauseBreak = 0x48,
		HidInsert = 0x49,
		HidMenu = 0x65, // Keyboard Application (context menu) key
		HidLCtrlMask = 0x1,
		HidRCtrlMask = 0x10,
		HidLAltMask = 0x4,
		HidRAltMask = 0x40,
		HidLCmdMask = 0x8,
		HidRCmdMask = 0x80,
		HidLShiftMask = 0x2,
		HidRShiftMask = 0x20
	};

	// Bits of the virtual HID device's Consumer Control report (see Vhf.c).
	// Keep in sync with Vhf.c.
	#define VhfBitMute           0x01
	#define VhfBitVolumeUp       0x02
	#define VhfBitVolumeDown     0x04
	#define VhfBitBrightnessUp   0x08
	#define VhfBitBrightnessDown 0x10

	// Media keys in the Consumer Control report (0x12).
	#define ConsumerReportId  0x12
	#define ConsumerPlayPause 0x01
	#define ConsumerScanNext  0x08
	#define ConsumerScanPrev  0x10

	///////////////////////////////////////////////////////////////////////////////
	// Global functions

	typedef VOID (*VHF_EVENT_SINK)(PVOID context, uint16_t type, uint16_t code, int32_t value);

	typedef struct _VHF_DEVICE
	{
		int            Fd;
		VHF_EVENT_SINK Sink;
		PVOID          SinkContext;
		UCHAR          PrevModifiers;
		UCHAR          PrevKeys[6];
		UCHAR          PrevVhfBits;
		UCHAR          PrevConsumerBits;
	} VHF_DEVICE;

	typedef struct _A1314_STATE
	{
		BOOLEAN      FakeFnActive;
		BOOLEAN      RightCmdAsMenu;
		UCHAR        ConsumerBits;
		UCHAR        ConsumerBitsPrev;
		UCHAR        VhfBits;
		UCHAR        VhfBitsPrev;
		POWER_ACTION PowerAction;
		POWER_ACTION PowerActionPrev;
		UCHAR        TmpBuf[10];
		UCHAR        SpecialKey;
		UCHAR        SpecialKeyModifier;
	} A1314_STATE;

	typedef struct _A1644_STATE
	{
		BOOLEAN      FakeFnActive;
		UCHAR        ConsumerBits;
		UCHAR        ConsumerBitsPrev;
		UCHAR        VhfBits;
		UCHAR        VhfBitsPrev;
		POWER_ACTION PowerAction;
		POWER_ACTION PowerActionPrev;
	} A1644_STATE;

	typedef struct _TRANSLATOR_CONTEXT
	{
		APPLE_KB_MODEL Model;
		A1314_STATE    A1314;
		A1644_STATE    A1644;
		VHF_DEVICE     Vhf;
	} TRANSLATOR_CONTEXT;

	void KdPrintBuffer(const char* text, PUCHAR buffer, ULONG length);

	void A1314ResetState(A1314_STATE* state);
	void ProcessA1314Block(A1314_STATE* state, VHF_DEVICE* vhf, PBRB pbrb);

	void A1644ResetState(A1644_STATE* state);
	void ProcessA1644Buffer(A1644_STATE* state, VHF_DEVICE* vhf, BYTE* pbuf, ULONG size);

	VOID TranslatorInitialize(TRANSLATOR_CONTEXT* ctx, APPLE_KB_MODEL model);
	VOID HandleRawReportForContext(TRANSLATOR_CONTEXT* ctx, const BYTE* raw, ssize_t len);

	BOOLEAN FindCaptureTarget(const char* sysName, CAPTURE_TARGET* out);
	int     RunCapture(const char* path, const char* sysName, int durationSec);
	int     RunReplay(const char* path);

	VOID VhfInitialize(VHF_DEVICE* dev);
	int  VhfOpenUinputNode(VOID);
	int  VhfInit(VHF_DEVICE* dev);
	VOID VhfDeinit(VHF_DEVICE* dev);
	int  VhfGetFd(const VHF_DEVICE* dev);
	VOID VhfSetEventSink(VHF_DEVICE* dev, VHF_EVENT_SINK sink, PVOID context);
	VOID VhfSendReport(VHF_DEVICE* dev, UCHAR bits);
	VOID VhfSendConsumerReport(VHF_DEVICE* dev, UCHAR bits);
	VOID VhfSendKeyboardReport(VHF_DEVICE* dev, UCHAR modifiers, const UCHAR* keys6);
	VOID VhfSendPowerKey(VHF_DEVICE* dev, POWER_ACTION action);
	VOID VhfReleaseAll(VHF_DEVICE* dev);

	VOID PowerInitialize(VOID);
	VOID WinAppleKeyRequestPowerAction(VHF_DEVICE* dev, POWER_ACTION action);

#ifdef __cplusplus
}
#endif
