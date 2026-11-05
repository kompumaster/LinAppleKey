#include "Driver.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

///////////////////////////////////////////////////////////////////////////////
// Globals Initialisation
//
// Defaults match WinAppleKey.inf (all enabled by default).
///////////////////////////////////////////////////////////////////////////////

DWORD g_dwSwapAltCmd = 1;
DWORD g_dwSwapFnCtrl = 1;
DWORD g_dwRightCmdAsMenu = 1;
DWORD g_dwSwapRightCmdAlt = 1;
DWORD g_dwSwapGraveBackslash = 1;
DWORD g_dwDirectPowerAction = 0;
BOOLEAN g_bPowerKeysEnabled = TRUE;
BOOLEAN g_bGrabEnabled = TRUE;
BOOLEAN g_bReplayMode = FALSE;

volatile sig_atomic_t g_StopRequested = 0;
static volatile sig_atomic_t g_ReloadRequested = 0;
static volatile sig_atomic_t g_ToggleGrabRequested = 0;
static const char* g_ConfigPath = "/etc/linapplekey.conf";

///////////////////////////////////////////////////////////////////////////////
// Supported Apple Keyboard Hardware IDs (matching WinAppleKey.inf)
///////////////////////////////////////////////////////////////////////////////

#define BUS_USB_HID       0x0003
#define BUS_BLUETOOTH_HID 0x0005

#define VID_APPLE_USB     0x05AC
#define VID_APPLE_BT_SIG  0x004C

static APPLE_KB_MODEL ClassifyAppleKeyboard(unsigned int bus, unsigned int vid, unsigned int pid)
{
	// Magic Keyboard A1644 / A1843 (USB or Bluetooth)
	if ((vid == VID_APPLE_USB || vid == VID_APPLE_BT_SIG) &&
	    (pid == 0x0267 || pid == 0x026C))
	{
		if (bus == BUS_USB_HID || bus == BUS_BLUETOOTH_HID)
			return KbModelA1644;
	}

	// Wireless Keyboard A1314 / A1255 / original (Bluetooth)
	if (bus == BUS_BLUETOOTH_HID && vid == VID_APPLE_USB)
	{
		switch (pid)
		{
		case 0x0255: // A1314 ANSI (2011)
		case 0x0256: // A1314 ISO  (2011)
		case 0x0257: // A1314 JIS  (2011)
		case 0x0239: // A1255 ANSI (2009)
		case 0x023A: // A1255 ISO  (2009)
		case 0x023B: // A1255 JIS  (2009)
		case 0x022C: // Wireless Keyboard ANSI (2007)
		case 0x022D: // Wireless Keyboard ISO  (2007)
		case 0x022E: // Wireless Keyboard JIS  (2007)
		case 0x0208: // Wireless Keyboard ANSI (original)
		case 0x0209: // Wireless Keyboard ISO  (original)
		case 0x020A: // Wireless Keyboard JIS  (original)
			return KbModelA1314;
		default:
			break;
		}
	}

	return KbModelNone;
}

///////////////////////////////////////////////////////////////////////////////
// Tracked Physical Keyboard Instance
///////////////////////////////////////////////////////////////////////////////

#define MAX_KEYBOARDS  4
#define MAX_EVENT_FDS  4
#define MAX_POLL_FDS   (1 + MAX_KEYBOARDS * (2 + MAX_EVENT_FDS))
#define GRAB_RETRY_INTERVAL_SEC 1.0

enum
{
	PFD_UINPUT = 0,
	PFD_NETLINK,
	PFD_HIDRAW,
	PFD_EVDEV
};

typedef struct _KB_DEVICE
{
	BOOLEAN        Active;
	APPLE_KB_MODEL Model;
	TRANSLATOR_CONTEXT Translation;
	unsigned int   Bus;
	unsigned int   Vid;
	unsigned int   Pid;
	char           SysName[128];   // e.g. "0005:05AC:0256.0005"
	char           HidrawPath[PATH_MAX]; // e.g. "/dev/hidraw4"
	int            HidrawFd;
	int            EventFds[MAX_EVENT_FDS];
	int            EventFdCount;
} KB_DEVICE;

static KB_DEVICE g_Devices[MAX_KEYBOARDS];

///////////////////////////////////////////////////////////////////////////////
// Utility & Logging
///////////////////////////////////////////////////////////////////////////////

// Debug output is unlimited on a terminal but rate-limited when journald
// or another pipe is the destination, so -v cannot flood the journal.
#define LOG_DEBUG_RATE   50.0
#define LOG_DEBUG_BURST 100.0

static LOG_LEVEL g_LogLevel = LogLevelInfo;
static BOOLEAN g_LogDebugUnlimited = FALSE;
static BOOLEAN g_LogJournal = FALSE;
static BOOLEAN g_LogRateStarted = FALSE;
static BOOLEAN g_LogInitDone = FALSE;
static struct timespec g_LogRateLast;
static struct timespec g_LogSummaryLast;
static BOOLEAN g_LogSummaryStarted = FALSE;
static double g_LogTokens = LOG_DEBUG_BURST;
static unsigned long g_LogSuppressed = 0;

static const char* LogLevelName(LOG_LEVEL level)
{
	switch (level)
	{
	case LogLevelError: return "ERROR";
	case LogLevelWarn:  return "WARN";
	case LogLevelInfo:  return "INFO";
	default:            return "DEBUG";
	}
}

static void LogPrintPrefix(LOG_LEVEL level)
{
	struct timespec now;
	struct tm tm;
	char stamp[32];

	if (g_LogJournal)
	{
		fprintf(stderr, DRIVERNAME " [%s] ", LogLevelName(level));
		return;
	}

	clock_gettime(CLOCK_REALTIME, &now);
	localtime_r(&now.tv_sec, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, DRIVERNAME " %s.%03ld [%s] ",
		stamp, now.tv_nsec / 1000000L, LogLevelName(level));
}

static double TimespecDiffSec(const struct timespec* a, const struct timespec* b)
{
	return (double)(a->tv_sec - b->tv_sec) +
	       (double)(a->tv_nsec - b->tv_nsec) / 1000000000.0;
}

static BOOLEAN LogAllowDebug(VOID)
{
	struct timespec now;

	if (g_LogDebugUnlimited)
		return TRUE;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!g_LogRateStarted)
	{
		g_LogRateStarted = TRUE;
		g_LogRateLast = now;
	}

	g_LogTokens += TimespecDiffSec(&now, &g_LogRateLast) * LOG_DEBUG_RATE;
	g_LogRateLast = now;
	if (g_LogTokens > LOG_DEBUG_BURST)
		g_LogTokens = LOG_DEBUG_BURST;

	if (g_LogTokens < 1.0)
	{
		g_LogSuppressed++;
		return FALSE;
	}

	g_LogTokens -= 1.0;

	if (g_LogSuppressed > 0)
	{
		BOOLEAN print = !g_LogSummaryStarted;
		if (!print)
			print = TimespecDiffSec(&now, &g_LogSummaryLast) >= 1.0;

		if (print)
		{
			LogPrintPrefix(LogLevelDebug);
			fprintf(stderr, "debug rate limit: %lu line(s) suppressed\n", g_LogSuppressed);
			g_LogSuppressed = 0;
			g_LogSummaryLast = now;
			g_LogSummaryStarted = TRUE;
		}
	}

	return TRUE;
}

static BOOLEAN LogAllow(LOG_LEVEL level)
{
	if (level > g_LogLevel)
		return FALSE;
	if (level == LogLevelDebug)
		return LogAllowDebug();
	return TRUE;
}

static void LogFlushSuppressed(VOID)
{
	if (g_LogSuppressed > 0)
	{
		LogPrintPrefix(LogLevelDebug);
		fprintf(stderr, "debug rate limit: %lu line(s) suppressed\n", g_LogSuppressed);
		g_LogSuppressed = 0;
	}
}

VOID LogInit(VOID)
{
	const char* journal;

	if (g_LogInitDone)
		return;

	g_LogInitDone = TRUE;
	g_LogLevel = LogLevelInfo;
	g_LogDebugUnlimited = isatty(STDERR_FILENO) != 0;

	// systemd sets JOURNAL_STREAM when stderr goes to the journal; journald
	// already adds its own timestamp.
	journal = getenv("JOURNAL_STREAM");
	g_LogJournal = (journal && *journal) != 0;
	atexit(LogFlushSuppressed);
}

VOID LogSetLevel(LOG_LEVEL level)
{
	g_LogLevel = level;
}

VOID LogMessage(LOG_LEVEL level, const char* fmt, ...)
{
	char message[16384];
	va_list ap;
	size_t len;

	if (!LogAllow(level))
		return;

	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);

	len = strlen(message);
	LogPrintPrefix(level);
	fputs(message, stderr);
	if (len == 0 || message[len - 1] != '\n')
		fputc('\n', stderr);
}

VOID LogHex(LOG_LEVEL level, const char* text, const unsigned char* buffer, size_t length)
{
	if (!LogAllow(level))
		return;

	LogPrintPrefix(level);
	fputs(text, stderr);
	for (size_t i = 0; i < length; i++)
		fprintf(stderr, "%02X ", buffer[i]);
	fputc('\n', stderr);
}

void KdPrintBuffer(const char* text, PUCHAR buffer, ULONG length)
{
	LogHex(LogLevelDebug, text, buffer, (size_t)length);
}

static void SleepMs(int ms)
{
	struct timespec ts;
	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

// Joins path components without snprintf(), so the compiler can see that
// nothing is truncated and -Wformat-truncation stays useful elsewhere.
static BOOLEAN BuildPath(char* dst, size_t dstSize, const char* prefix, const char* suffix)
{
	size_t prefixLen = strlen(prefix);
	size_t suffixLen = strlen(suffix);

	if (prefixLen + suffixLen + 1 > dstSize)
		return FALSE;

	memcpy(dst, prefix, prefixLen);
	memcpy(dst + prefixLen, suffix, suffixLen + 1);
	return TRUE;
}

static BOOLEAN BuildPath3(char* dst, size_t dstSize, const char* a, const char* b, const char* c)
{
	size_t aLen = strlen(a);
	size_t bLen = strlen(b);
	size_t cLen = strlen(c);

	if (aLen + bLen + cLen + 1 > dstSize)
		return FALSE;

	memcpy(dst, a, aLen);
	memcpy(dst + aLen, b, bLen);
	memcpy(dst + aLen + bLen, c, cLen + 1);
	return TRUE;
}

static BOOLEAN CopyString(char* dst, size_t dstSize, const char* src)
{
	size_t len = strlen(src);

	if (len + 1 > dstSize)
		return FALSE;

	memcpy(dst, src, len + 1);
	return TRUE;
}

static void SetReason(char* buf, size_t size, const char* fmt, ...)
{
	if (!buf || size == 0)
		return;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, size, fmt, ap);
	va_end(ap);
}

static void SignalHandler(int sig)
{
	if (sig == SIGHUP)
		g_ReloadRequested = 1;
	else if (sig == SIGUSR1)
		g_ToggleGrabRequested = 1;
	else
		g_StopRequested = 1;
}

///////////////////////////////////////////////////////////////////////////////
// Configuration File Reader (/etc/linapplekey.conf)
///////////////////////////////////////////////////////////////////////////////

static char* TrimWhitespace(char* s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;
	char* end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

static BOOLEAN ParseConfigNumber(const char* text, DWORD* out)
{
	if (!text || !*text || *text == '-')
		return FALSE;

	errno = 0;
	char* end = NULL;
	unsigned long value = strtoul(text, &end, 0);

	if (errno == ERANGE || end == text || *end != '\0' || value > 0xFFFFFFFFUL)
		return FALSE;

	*out = (DWORD)value;
	return TRUE;
}

static void ReadDriverConfig(const char* path)
{
	FILE* fp = fopen(path, "r");
	if (!fp)
	{
		LogWarn("config %s: %s (using defaults)", path, strerror(errno));
		return;
	}

	struct stat st;
	if (fstat(fileno(fp), &st) == 0 && (st.st_mode & S_IWOTH))
		LogWarn("warning: %s is world-writable", path);

	char line[256];
	int lineNo = 0;

	while (fgets(line, sizeof(line), fp))
	{
		lineNo++;

		// fgets() splits a too-long line into several chunks. Discard the
		// rest so the tail is not parsed as another key.
		if (!strchr(line, '\n') && !feof(fp))
		{
			int c;
			while ((c = fgetc(fp)) != '\n' && c != EOF)
				;
			LogWarn("config %s:%d: line too long (ignored)", path, lineNo);
			if (c == EOF)
				break;
			continue;
		}

		char* comment = strchr(line, '#');
		if (comment)
			*comment = '\0';
		comment = strchr(line, ';');
		if (comment)
			*comment = '\0';

		char* p = TrimWhitespace(line);
		if (!*p)
			continue;

		char* eq = strchr(p, '=');
		if (!eq)
		{
			LogWarn("config %s:%d: malformed line '%s' (ignored)", path, lineNo, p);
			continue;
		}

		*eq = '\0';
		char* key = TrimWhitespace(p);
		char* val = TrimWhitespace(eq + 1);

		if (!*key || !*val)
		{
			LogWarn("config %s:%d: empty key or value (ignored)", path, lineNo);
			continue;
		}

		DWORD num = 0;
		if (!ParseConfigNumber(val, &num))
		{
			LogWarn("config %s:%d: invalid value '%s' for '%s' (ignored)", path, lineNo, val, key);
			continue;
		}

		enum
		{
			CfgUnknown = 0,
			CfgSwapAltCmd,
			CfgSwapFnCtrl,
			CfgRightCmdAsMenu,
			CfgSwapRightCmdAlt,
			CfgSwapGraveBackslash,
			CfgDirectPowerAction,
			CfgPowerKeys,
			CfgGrab
		} cfg = CfgUnknown;

		if (strcasecmp(key, "SwapAltCmd") == 0)
			cfg = CfgSwapAltCmd;
		else if (strcasecmp(key, "SwapFnCtrl") == 0)
			cfg = CfgSwapFnCtrl;
		else if (strcasecmp(key, "RightCmdAsMenu") == 0)
			cfg = CfgRightCmdAsMenu;
		else if (strcasecmp(key, "SwapRightCmdAlt") == 0)
			cfg = CfgSwapRightCmdAlt;
		else if (strcasecmp(key, "SwapGraveBackslash") == 0)
			cfg = CfgSwapGraveBackslash;
		else if (strcasecmp(key, "DirectPowerAction") == 0)
			cfg = CfgDirectPowerAction;
		else if (strcasecmp(key, "PowerKeys") == 0)
			cfg = CfgPowerKeys;
		else if (strcasecmp(key, "Grab") == 0)
			cfg = CfgGrab;

		if (cfg == CfgUnknown)
		{
			LogWarn("config %s:%d: unknown key '%s' (ignored)", path, lineNo, key);
			continue;
		}

		// Every current option is a boolean. Keep the old value on a bad
		// one instead of silently turning it off.
		if (num > 1)
		{
			LogWarn("config %s:%d: value %lu for '%s' must be 0 or 1 (ignored)",
				path, lineNo, (unsigned long)num, key);
			continue;
		}

		switch (cfg)
		{
		case CfgSwapAltCmd:         g_dwSwapAltCmd = num; break;
		case CfgSwapFnCtrl:         g_dwSwapFnCtrl = num; break;
		case CfgRightCmdAsMenu:     g_dwRightCmdAsMenu = num; break;
		case CfgSwapRightCmdAlt:    g_dwSwapRightCmdAlt = num; break;
		case CfgSwapGraveBackslash: g_dwSwapGraveBackslash = num; break;
		case CfgDirectPowerAction:  g_dwDirectPowerAction = num; break;
		case CfgPowerKeys:          g_bPowerKeysEnabled = (num != 0) ? TRUE : FALSE; break;
		case CfgGrab:               g_bGrabEnabled = (num != 0) ? TRUE : FALSE; break;
		case CfgUnknown:            break;
		}
	}

	fclose(fp);

	DebugPrint("Settings: SwapFnCtrl=%u SwapAltCmd=%u RightCmdAsMenu=%u SwapRightCmdAlt=%u SwapGraveBackslash=%u DirectPowerAction=%u Grab=%u PowerKeys=%u\n",
		g_dwSwapFnCtrl, g_dwSwapAltCmd, g_dwRightCmdAsMenu,
		g_dwSwapRightCmdAlt, g_dwSwapGraveBackslash, g_dwDirectPowerAction,
		(unsigned)g_bGrabEnabled, (unsigned)g_bPowerKeysEnabled);
}

///////////////////////////////////////////////////////////////////////////////
// Device Discovery and Exclusive Grab
///////////////////////////////////////////////////////////////////////////////

// Waits until no key is pressed on the evdev node before grabbing it, so
// starting the service by hitting [Enter] on the Apple keyboard itself never
// leaves [Enter] stuck in the compositor. Returns FALSE if keys are still
// held after the short wait: grabbing then is worse than not grabbing.
static BOOLEAN WaitForKeysReleased(int evfd)
{
	uint8_t keybits[(KEY_MAX + 7) / 8];

	for (int attempt = 0; attempt < 25; attempt++)
	{
		memset(keybits, 0, sizeof(keybits));
		if (ioctl(evfd, EVIOCGKEY(sizeof(keybits)), keybits) < 0)
			return FALSE;

		BOOLEAN anyPressed = FALSE;
		for (size_t i = 0; i < sizeof(keybits); i++)
		{
			if (keybits[i] != 0)
			{
				anyPressed = TRUE;
				break;
			}
		}

		if (!anyPressed)
			return TRUE;

		SleepMs(20);
	}

	return FALSE;
}

static void CloseKbDevice(KB_DEVICE* dev)
{
	if (!dev->Active)
		return;

	LogInfo("detached %s (%s)", dev->SysName, dev->HidrawPath);

	for (int i = 0; i < dev->EventFdCount; i++)
	{
		if (dev->EventFds[i] >= 0)
		{
			ioctl(dev->EventFds[i], EVIOCGRAB, 0);
			close(dev->EventFds[i]);
			dev->EventFds[i] = -1;
		}
	}
	dev->EventFdCount = 0;

	if (dev->HidrawFd >= 0)
	{
		close(dev->HidrawFd);
		dev->HidrawFd = -1;
	}

	if (dev->Model == KbModelA1314)
		A1314ResetState(&dev->Translation.A1314);
	else if (dev->Model == KbModelA1644)
		A1644ResetState(&dev->Translation.A1644);

	VhfDeinit(&dev->Translation.Vhf);
	dev->Active = FALSE;
}

static BOOLEAN IsDeviceAlreadyTracked(const char* sysName)
{
	for (int i = 0; i < MAX_KEYBOARDS; i++)
	{
		if (g_Devices[i].Active && strcmp(g_Devices[i].SysName, sysName) == 0)
			return TRUE;
	}
	return FALSE;
}

static BOOLEAN ParseHidUevent(const char* sysDir, unsigned int* bus, unsigned int* vid, unsigned int* pid)
{
	char path[PATH_MAX];
	if (!BuildPath(path, sizeof(path), sysDir, "/uevent"))
		return FALSE;

	FILE* fp = fopen(path, "r");
	if (!fp)
		return FALSE;

	BOOLEAN found = FALSE;
	char line[256];
	while (fgets(line, sizeof(line), fp))
	{
		if (strncmp(line, "HID_ID=", 7) == 0)
		{
			if (sscanf(line + 7, "%x:%x:%x", bus, vid, pid) == 3)
			{
				found = TRUE;
				break;
			}
		}
	}

	fclose(fp);
	return found;
}

static BOOLEAN FindHidrawDevNode(const char* sysDir, char* outPath, size_t outSize)
{
	char hidrawDir[PATH_MAX];
	if (!BuildPath(hidrawDir, sizeof(hidrawDir), sysDir, "/hidraw"))
		return FALSE;

	DIR* d = opendir(hidrawDir);
	if (!d)
		return FALSE;

	BOOLEAN found = FALSE;
	struct dirent* ent;
	while ((ent = readdir(d)) != NULL)
	{
		if (strncmp(ent->d_name, "hidraw", 6) == 0)
		{
			if (BuildPath(outPath, outSize, "/dev/", ent->d_name))
				found = TRUE;
			break;
		}
	}

	closedir(d);
	return found;
}

// Collects /dev/input/event* paths belonging to a HID device without
// opening them. Used both by --list-devices and by the grab path.
static int CollectEventPaths(const char* sysDir, char paths[][PATH_MAX], int maxPaths)
{
	char inputDir[PATH_MAX];
	if (!BuildPath(inputDir, sizeof(inputDir), sysDir, "/input"))
		return 0;

	DIR* d = opendir(inputDir);
	if (!d)
		return 0;

	int count = 0;
	struct dirent* ent;
	while ((ent = readdir(d)) != NULL && count < maxPaths)
	{
		if (strncmp(ent->d_name, "input", 5) != 0)
			continue;

		char subInputDir[PATH_MAX];
		if (!BuildPath3(subInputDir, sizeof(subInputDir), inputDir, "/", ent->d_name))
			continue;

		DIR* sub = opendir(subInputDir);
		if (!sub)
			continue;

		struct dirent* subEnt;
		while ((subEnt = readdir(sub)) != NULL && count < maxPaths)
		{
			if (strncmp(subEnt->d_name, "event", 5) != 0)
				continue;

			if (BuildPath(paths[count], sizeof(paths[0]), "/dev/input/", subEnt->d_name))
				count++;
		}

		closedir(sub);
	}

	closedir(d);
	return count;
}

static int GrabDeviceEventNodes(const char* sysDir, int* outFds, int maxFds,
                                char* reason, size_t reasonSize)
{
	char eventPaths[MAX_EVENT_FDS][PATH_MAX];
	int limit = (maxFds < MAX_EVENT_FDS) ? maxFds : MAX_EVENT_FDS;
	int pathCount = CollectEventPaths(sysDir, eventPaths, limit);
	int openedFds[MAX_EVENT_FDS];
	int count = 0;

	if (pathCount == 0)
	{
		SetReason(reason, reasonSize, "no evdev event nodes");
		return 0;
	}

	// All-or-nothing: if one node cannot be grabbed, release the rest and
	// leave the physical keyboard alone. Half-grabbed input is worse than
	// no grab at all.
	for (int i = 0; i < pathCount; i++)
	{
		const char* evPath = eventPaths[i];

		int evfd = open(evPath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (evfd < 0)
			evfd = open(evPath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (evfd < 0)
		{
			SetReason(reason, reasonSize, "cannot open %s: %s", evPath, strerror(errno));
			goto fail;
		}

		if (!WaitForKeysReleased(evfd))
		{
			SetReason(reason, reasonSize, "keys still held on %s", evPath);
			close(evfd);
			goto fail;
		}

		if (ioctl(evfd, EVIOCGRAB, 1) < 0)
		{
			SetReason(reason, reasonSize, "EVIOCGRAB %s failed: %s", evPath, strerror(errno));
			close(evfd);
			goto fail;
		}

		openedFds[count++] = evfd;
	}

	for (int i = 0; i < count; i++)
	{
		outFds[i] = openedFds[i];
		DebugPrint("Grabbed %s\n", eventPaths[i]);
	}
	if (reason && reasonSize)
		reason[0] = '\0';
	return count;

fail:
	for (int i = 0; i < count; i++)
	{
		ioctl(openedFds[i], EVIOCGRAB, 0);
		close(openedFds[i]);
	}
	return 0;
}

static char g_LastAttachErrorName[128];
static char g_LastAttachErrorReason[256];
static BOOLEAN g_AttachErrorReported = FALSE;

static void ReportAttachError(const char* sysName, const char* reason)
{
	if (strcmp(g_LastAttachErrorName, sysName) == 0 &&
	    strcmp(g_LastAttachErrorReason, reason) == 0)
		return;

	LogError("%s: %s", sysName, reason);
	if (strstr(reason, "Permission denied") || strstr(reason, "Operation not permitted"))
		LogWarn("hint: run as root or grant access to /dev/hidraw* and /dev/uinput");
	else if (strstr(reason, "hidraw node not found") || strstr(reason, "no evdev"))
		LogWarn("hint: check Bluetooth/USB connection and udev; try --doctor");
	else if (strstr(reason, "keys still held"))
		LogWarn("hint: release all keys and retry");
	g_AttachErrorReported = TRUE;
	CopyString(g_LastAttachErrorName, sizeof(g_LastAttachErrorName), sysName);
	CopyString(g_LastAttachErrorReason, sizeof(g_LastAttachErrorReason), reason);
}

static void ClearAttachError(const char* sysName)
{
	if (strcmp(g_LastAttachErrorName, sysName) == 0)
		g_LastAttachErrorName[0] = '\0';
}

static BOOLEAN TryAttachHidDevice(const char* sysName)
{
	if (IsDeviceAlreadyTracked(sysName))
		return FALSE;

	char sysDir[PATH_MAX];
	if (!BuildPath(sysDir, sizeof(sysDir), "/sys/bus/hid/devices/", sysName))
		return FALSE;

	unsigned int bus = 0, vid = 0, pid = 0;
	if (!ParseHidUevent(sysDir, &bus, &vid, &pid))
		return FALSE;

	APPLE_KB_MODEL model = ClassifyAppleKeyboard(bus, vid, pid);
	if (model == KbModelNone)
		return FALSE;

	// Find a free device slot
	KB_DEVICE* slot = NULL;
	for (int i = 0; i < MAX_KEYBOARDS; i++)
	{
		if (!g_Devices[i].Active)
		{
			slot = &g_Devices[i];
			break;
		}
	}
	if (!slot)
	{
		ReportAttachError(sysName, "maximum of 4 keyboards reached");
		return FALSE;
	}

	TRANSLATOR_CONTEXT translation;
	TranslatorInitialize(&translation, model);
	if (VhfInit(&translation.Vhf) < 0)
	{
		ReportAttachError(sysName, "cannot create virtual keyboard (/dev/uinput)");
		memset(slot, 0, sizeof(*slot));
		return FALSE;
	}

	// When a Bluetooth keyboard just connected, udev may still be creating
	// /dev/hidrawX and /dev/input/eventY. Retry briefly.
	char hidrawPath[PATH_MAX] = { 0 };
	int hidrawFd = -1;
	int eventFds[MAX_EVENT_FDS];
	int eventFdCount = 0;
	char reason[256] = "hidraw node not found";

	for (int retry = 0; retry < 25 && !g_StopRequested; retry++)
	{
		if (FindHidrawDevNode(sysDir, hidrawPath, sizeof(hidrawPath)))
		{
			hidrawFd = open(hidrawPath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
			if (hidrawFd < 0)
				hidrawFd = open(hidrawPath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (hidrawFd < 0)
			{
				SetReason(reason, sizeof(reason), "cannot open %s: %s",
					hidrawPath, strerror(errno));
				if (errno == EACCES || errno == EPERM)
					break;
			}
		}
		else
		{
			SetReason(reason, sizeof(reason), "hidraw node not found");
		}

		if (hidrawFd >= 0)
		{
			if (!g_bGrabEnabled)
			{
				// --no-grab / Grab=0: read hidraw and inject, but leave
				// the physical keyboard bound to the normal input stack.
				eventFdCount = 0;
				break;
			}

			eventFdCount = GrabDeviceEventNodes(sysDir, eventFds, MAX_EVENT_FDS,
				reason, sizeof(reason));
			if (eventFdCount > 0)
				break;

			close(hidrawFd);
			hidrawFd = -1;
		}

		SleepMs(20);
	}

	if (g_StopRequested || hidrawFd < 0 || (g_bGrabEnabled && eventFdCount == 0))
	{
		if (!g_StopRequested)
			ReportAttachError(sysName, reason);
		for (int i = 0; i < eventFdCount; i++)
		{
			ioctl(eventFds[i], EVIOCGRAB, 0);
			close(eventFds[i]);
		}
		if (hidrawFd >= 0)
			close(hidrawFd);
		VhfDeinit(&translation.Vhf);
		memset(slot, 0, sizeof(*slot));
		return FALSE;
	}

	memset(slot, 0, sizeof(*slot));
	slot->Translation = translation;
	slot->Active = TRUE;
	slot->Model = model;
	slot->Bus = bus;
	slot->Vid = vid;
	slot->Pid = pid;

	if (!CopyString(slot->SysName, sizeof(slot->SysName), sysName) ||
	    !CopyString(slot->HidrawPath, sizeof(slot->HidrawPath), hidrawPath))
	{
		ReportAttachError(sysName, "internal path copy failed");
		for (int i = 0; i < eventFdCount; i++)
		{
			ioctl(eventFds[i], EVIOCGRAB, 0);
			close(eventFds[i]);
		}
		close(hidrawFd);
		VhfDeinit(&translation.Vhf);
		memset(slot, 0, sizeof(*slot));
		return FALSE;
	}

	slot->HidrawFd = hidrawFd;
	slot->EventFdCount = eventFdCount;
	for (int i = 0; i < MAX_EVENT_FDS; i++)
		slot->EventFds[i] = (i < eventFdCount) ? eventFds[i] : -1;

	ClearAttachError(sysName);

	LogInfo("attached %s (%s, model=%s, VID=%04X PID=%04X, grab=%s)",
		slot->SysName, slot->HidrawPath,
		(model == KbModelA1314) ? "A1314" : "A1644",
		vid, pid, g_bGrabEnabled ? "on" : "off");

	return TRUE;
}

static void ScanHidDevices(void)
{
	DIR* d = opendir("/sys/bus/hid/devices");
	if (!d)
		return;

	struct dirent* ent;
	while (!g_StopRequested && (ent = readdir(d)) != NULL)
	{
		if (ent->d_name[0] == '.')
			continue;
		TryAttachHidDevice(ent->d_name);
	}

	closedir(d);
}

// Switches between exclusive evdev grab and --no-grab at runtime without
// dropping hidraw. Used by SIGUSR1 and by SIGHUP after a config reload.
static void ApplyGrabMode(BOOLEAN enable)
{
	g_bGrabEnabled = enable;

	for (int i = 0; i < MAX_KEYBOARDS; i++)
	{
		KB_DEVICE* dev = &g_Devices[i];
		if (!dev->Active)
			continue;

		// Never leave injected keys held while the grab state changes.
		VhfReleaseAll(&dev->Translation.Vhf);
		A1314ResetState(&dev->Translation.A1314);
		A1644ResetState(&dev->Translation.A1644);

		if (enable)
		{
			if (dev->EventFdCount > 0)
				continue;

			int fds[MAX_EVENT_FDS];
			char reason[256] = { 0 };
			char sysDir[PATH_MAX];
			if (!BuildPath(sysDir, sizeof(sysDir), "/sys/bus/hid/devices/", dev->SysName))
				continue;

			int count = GrabDeviceEventNodes(sysDir, fds, MAX_EVENT_FDS,
				reason, sizeof(reason));
			if (count <= 0)
			{
				LogWarn("grab deferred for %s: %s", dev->SysName, reason);
				continue;
			}

			dev->EventFdCount = count;
			for (int j = 0; j < MAX_EVENT_FDS; j++)
				dev->EventFds[j] = (j < count) ? fds[j] : -1;

			LogInfo("grab enabled for %s", dev->SysName);
		}
		else
		{
			for (int j = 0; j < dev->EventFdCount; j++)
			{
				if (dev->EventFds[j] >= 0)
				{
					ioctl(dev->EventFds[j], EVIOCGRAB, 0);
					close(dev->EventFds[j]);
					dev->EventFds[j] = -1;
				}
			}
			dev->EventFdCount = 0;
			LogInfo("grab disabled for %s", dev->SysName);
		}
	}

	LogInfo("grab %s", enable ? "enabled" : "disabled");
}

// A grab can be deferred when keys are still held. Retry periodically so
// remapping resumes without another SIGUSR1 or SIGHUP.
static void RetryDeferredGrabs(VOID)
{
	static BOOLEAN started = FALSE;
	static struct timespec last;
	struct timespec now;

	if (!g_bGrabEnabled)
		return;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (started && TimespecDiffSec(&now, &last) < GRAB_RETRY_INTERVAL_SEC)
		return;
	started = TRUE;
	last = now;

	for (int i = 0; i < MAX_KEYBOARDS; i++)
	{
		KB_DEVICE* dev = &g_Devices[i];
		if (!dev->Active || dev->EventFdCount > 0)
			continue;

		char sysDir[PATH_MAX];
		if (!BuildPath(sysDir, sizeof(sysDir), "/sys/bus/hid/devices/", dev->SysName))
			continue;

		int fds[MAX_EVENT_FDS];
		char reason[256] = { 0 };
		int count = GrabDeviceEventNodes(sysDir, fds, MAX_EVENT_FDS,
			reason, sizeof(reason));
		if (count <= 0)
		{
			DebugPrint("grab still deferred for %s: %s\n", dev->SysName, reason);
			continue;
		}

		dev->EventFdCount = count;
		for (int j = 0; j < MAX_EVENT_FDS; j++)
			dev->EventFds[j] = (j < count) ? fds[j] : -1;
		LogInfo("grab enabled for %s", dev->SysName);
	}
}

///////////////////////////////////////////////////////////////////////////////
// --list-devices (read-only, no root and no /dev access required)
///////////////////////////////////////////////////////////////////////////////

static const char* ModelName(APPLE_KB_MODEL model)
{
	switch (model)
	{
	case KbModelA1314: return "A1314 (Apple Wireless Keyboard)";
	case KbModelA1644: return "A1644 (Apple Magic Keyboard)";
	default:           return "unknown";
	}
}

static const char* BusName(unsigned int bus)
{
	switch (bus)
	{
	case BUS_USB_HID:       return "USB";
	case BUS_BLUETOOTH_HID: return "Bluetooth";
	default:                return "other";
	}
}

static void PrintNodeInfo(const char* label, const char* path)
{
	struct stat st;
	if (stat(path, &st) != 0)
	{
		printf("  %-7s %s (missing)\n", label, path);
		return;
	}

	printf("  %-7s %s (mode %04o, uid %u, gid %u%s)\n",
		label, path, (unsigned)(st.st_mode & 0777),
		(unsigned)st.st_uid, (unsigned)st.st_gid,
		access(path, R_OK) == 0 ? ", readable" : ", not readable");
}

static int ListSupportedDevices(void)
{
	DIR* d = opendir("/sys/bus/hid/devices");
	if (!d)
	{
		LogError("cannot open /sys/bus/hid/devices: %s", strerror(errno));
		return 1;
	}

	int found = 0;
	struct dirent* ent;

	printf("Supported Apple keyboards:\n");

	while ((ent = readdir(d)) != NULL)
	{
		if (ent->d_name[0] == '.')
			continue;

		char sysDir[PATH_MAX];
		if (!BuildPath(sysDir, sizeof(sysDir), "/sys/bus/hid/devices/", ent->d_name))
			continue;

		unsigned int bus = 0, vid = 0, pid = 0;
		if (!ParseHidUevent(sysDir, &bus, &vid, &pid))
			continue;

		APPLE_KB_MODEL model = ClassifyAppleKeyboard(bus, vid, pid);
		if (model == KbModelNone)
			continue;

		found++;

		printf("\n%s\n", sysDir);
		printf("  model:  %s\n", ModelName(model));
		printf("  bus:    %s (0x%04X)\n", BusName(bus), bus);
		printf("  ids:    %04X:%04X\n", vid, pid);

		char hidrawPath[PATH_MAX];
		if (FindHidrawDevNode(sysDir, hidrawPath, sizeof(hidrawPath)))
			PrintNodeInfo("hidraw:", hidrawPath);
		else
			printf("  hidraw: not found\n");

		char eventPaths[MAX_EVENT_FDS][PATH_MAX];
		int eventCount = CollectEventPaths(sysDir, eventPaths, MAX_EVENT_FDS);
		if (eventCount == 0)
			printf("  evdev:  none\n");
		for (int i = 0; i < eventCount; i++)
			PrintNodeInfo("evdev:", eventPaths[i]);
	}

	closedir(d);

	if (!found)
		printf("  none\n");

	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Capture target resolution (shared by --capture)
///////////////////////////////////////////////////////////////////////////////

static BOOLEAN FillCaptureTarget(const char* sysName, const char* sysDir, CAPTURE_TARGET* out)
{
	unsigned int bus = 0, vid = 0, pid = 0;
	if (!ParseHidUevent(sysDir, &bus, &vid, &pid))
		return FALSE;

	APPLE_KB_MODEL model = ClassifyAppleKeyboard(bus, vid, pid);
	if (model == KbModelNone)
		return FALSE;

	char hidrawPath[PATH_MAX];
	if (!FindHidrawDevNode(sysDir, hidrawPath, sizeof(hidrawPath)))
		return FALSE;

	memset(out, 0, sizeof(*out));
	out->Model = model;
	out->Bus = bus;
	out->Vid = vid;
	out->Pid = pid;

	if (!CopyString(out->SysName, sizeof(out->SysName), sysName))
		return FALSE;
	if (!CopyString(out->SysDir, sizeof(out->SysDir), sysDir))
		return FALSE;
	if (!CopyString(out->HidrawPath, sizeof(out->HidrawPath), hidrawPath))
		return FALSE;

	return TRUE;
}

BOOLEAN FindCaptureTarget(const char* sysName, CAPTURE_TARGET* out)
{
	if (!out)
		return FALSE;

	memset(out, 0, sizeof(*out));

	if (sysName)
	{
		char sysDir[PATH_MAX];
		if (!BuildPath(sysDir, sizeof(sysDir), "/sys/bus/hid/devices/", sysName))
			return FALSE;
		return FillCaptureTarget(sysName, sysDir, out);
	}

	DIR* d = opendir("/sys/bus/hid/devices");
	if (!d)
		return FALSE;

	BOOLEAN found = FALSE;
	struct dirent* ent;

	while ((ent = readdir(d)) != NULL)
	{
		if (ent->d_name[0] == '.')
			continue;

		char sysDir[PATH_MAX];
		if (!BuildPath(sysDir, sizeof(sysDir), "/sys/bus/hid/devices/", ent->d_name))
			continue;

		if (FillCaptureTarget(ent->d_name, sysDir, out))
		{
			found = TRUE;
			break;
		}
	}

	closedir(d);
	return found;
}

static int OpenUeventNetlinkSocket(void)
{
	int sock = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (sock < 0)
		return -1;

	struct sockaddr_nl addr;
	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = (uint32_t)getpid();
	addr.nl_groups = 1; // Kernel multicast group for uevents

	if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		int err = errno;
		close(sock);
		errno = err;
		return -1;
	}

	return sock;
}

static int CheckSysfs(void)
{
	DIR* d = opendir("/sys/bus/hid/devices");
	if (!d)
	{
		LogError("cannot access /sys/bus/hid/devices: %s", strerror(errno));
		return -1;
	}
	closedir(d);
	return 0;
}

static int ProbeUinput(void)
{
	int fd = VhfOpenUinputNode();
	if (fd < 0)
	{
		LogError("cannot access /dev/uinput: %s", strerror(errno));
		LogWarn("hint: run as root or grant access; load the module: sudo modprobe uinput");
		return -1;
	}
	close(fd);
	return 0;
}

static int RunDoctor(void)
{
	printf("LinAppleKey doctor\n");
	printf("  uid=%u euid=%u\n", (unsigned)getuid(), (unsigned)geteuid());

	if (access(g_ConfigPath, R_OK) == 0)
		printf("  config: %s (readable)\n", g_ConfigPath);
	else
		printf("  config: %s (%s; defaults will be used)\n", g_ConfigPath, strerror(errno));

	if (CheckSysfs() == 0)
		printf("  sysfs: /sys/bus/hid/devices (readable)\n");

	int fd = VhfOpenUinputNode();
	if (fd >= 0)
	{
		printf("  uinput: /dev/uinput (accessible)\n");
		close(fd);
	}
	else
	{
		printf("  uinput: %s\n", strerror(errno));
	}

	int nl = OpenUeventNetlinkSocket();
	if (nl >= 0)
	{
		printf("  netlink: uevent socket (OK)\n");
		close(nl);
	}
	else
	{
		printf("  netlink: uevent socket failed: %s\n", strerror(errno));
	}

	ListSupportedDevices();
	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Raw HID Report Dispatch (mirrors InternalIoctlComplete in WinAppleKey)
///////////////////////////////////////////////////////////////////////////////

VOID HandleRawReportForContext(TRANSLATOR_CONTEXT* ctx, const BYTE* raw, ssize_t len)
{
	if (len <= 0)
		return;

	if (ctx->Model == KbModelA1314)
	{
		BYTE l2capBuf[16];
		BRB brb;

		// Linux hidraw strips the 0xA1 Bluetooth HIDP DATA_INPUT header byte:
		//   * Report ID 0x11 (2 bytes: 11 <bits>) -> 3-byte L2CAP report (A1 11 <bits>)
		//   * Report ID 0x01 (9 bytes: 01 <mod> <res> <k1..k6>) -> 10-byte L2CAP report
		//   * Report ID 0x47 (2 bytes: battery strength) -> ignore
		if (len == 2 && (raw[0] == 0x11 || raw[0] == 0x13))
		{
			l2capBuf[0] = 0xA1;
			l2capBuf[1] = raw[0];
			l2capBuf[2] = raw[1];
			brb.BrbL2caAclTransfer.Buffer = l2capBuf;
			brb.BrbL2caAclTransfer.BufferSize = 3;
			ProcessA1314Block(&ctx->A1314, &ctx->Vhf, &brb);
		}
		else if (len == 9 && raw[0] == 0x01)
		{
			l2capBuf[0] = 0xA1;
			RtlCopyMemory(&l2capBuf[1], raw, 9);
			brb.BrbL2caAclTransfer.Buffer = l2capBuf;
			brb.BrbL2caAclTransfer.BufferSize = 10;
			ProcessA1314Block(&ctx->A1314, &ctx->Vhf, &brb);
		}
		else if ((len == 3 || len == 10) && raw[0] == 0xA1)
		{
			RtlCopyMemory(l2capBuf, raw, (size_t)len);
			brb.BrbL2caAclTransfer.Buffer = l2capBuf;
			brb.BrbL2caAclTransfer.BufferSize = (ULONG)len;
			ProcessA1314Block(&ctx->A1314, &ctx->Vhf, &brb);
		}
	}
	else if (ctx->Model == KbModelA1644)
	{
		BYTE buf[16];
		if (len == 10 && raw[0] == 0x01)
		{
			RtlCopyMemory(buf, raw, 10);
			DebugPrintBuffer("ProcessA1644Buffer(): <= ", buf, 10);
			ProcessA1644Buffer(&ctx->A1644, &ctx->Vhf, buf + 1, 9);
			DebugPrintBuffer("ProcessA1644Buffer(): => ", buf, 10);
		}
		else if (len == 11 && raw[0] == 0xA1)
		{
			RtlCopyMemory(buf, raw, 11);
			DebugPrintBuffer("ProcessA1644Buffer(): <= ", buf, 11);
			ProcessA1644Buffer(&ctx->A1644, &ctx->Vhf, buf + 2, 9);
			DebugPrintBuffer("ProcessA1644Buffer(): => ", buf, 11);
		}
		else if (len == 9)
		{
			RtlCopyMemory(buf, raw, 9);
			DebugPrintBuffer("ProcessA1644Buffer(): <= ", buf, 9);
			ProcessA1644Buffer(&ctx->A1644, &ctx->Vhf, buf, 9);
			DebugPrintBuffer("ProcessA1644Buffer(): => ", buf, 9);
		}
	}
}

static void HandleRawReport(KB_DEVICE* dev, const BYTE* raw, ssize_t len)
{
	HandleRawReportForContext(&dev->Translation, raw, len);
}

VOID TranslatorInitialize(TRANSLATOR_CONTEXT* ctx, APPLE_KB_MODEL model)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->Model = model;
	A1314ResetState(&ctx->A1314);
	A1644ResetState(&ctx->A1644);
	VhfInitialize(&ctx->Vhf);
}

// Forward Caps Lock LED state from this device's virtual keyboard to its
// physical evdev nodes.
static void HandleUinputEvents(KB_DEVICE* dev)
{
	int uinputFd = VhfGetFd(&dev->Translation.Vhf);
	struct input_event ev;

	if (uinputFd < 0)
		return;

	while (read(uinputFd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
	{
		if (ev.type != EV_LED)
			continue;

		// The Apple keyboards have a Caps Lock LED only. Forwarding
		// NumLock/ScrollLock/Compose/Kana to them is meaningless and can
		// put some keyboard firmware into a numeric-keypad mode.
		if (ev.code != LED_CAPSL)
			continue;

		for (int j = 0; j < dev->EventFdCount; j++)
		{
			int evfd = dev->EventFds[j];
			if (evfd >= 0)
			{
				struct input_event syn;
				memset(&syn, 0, sizeof(syn));
				syn.type = EV_SYN;
				syn.code = SYN_REPORT;
				if (write(evfd, &ev, sizeof(ev)) >= 0)
					(void)write(evfd, &syn, sizeof(syn));
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// Entry Point
///////////////////////////////////////////////////////////////////////////////

static BOOLEAN ParseDuration(const char* text, int* out)
{
	char* end = NULL;
	errno = 0;
	unsigned long value = strtoul(text, &end, 10);

	if (errno != 0 || end == text || *end != '\0' || value == 0 || value > 86400)
		return FALSE;

	*out = (int)value;
	return TRUE;
}

static void PrintUsage(const char* prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"Options:\n"
		"  -c, --config <path>   Path to configuration file\n"
		"                        (default: /etc/linapplekey.conf)\n"
		"  -v, --verbose         Verbose debug logging (raw and translated HID reports)\n"
		"      --list-devices    List supported Apple keyboards and exit\n"
		"      --doctor          Diagnose permissions, uinput, netlink and devices\n"
		"      --capture <file>  Save raw hidraw reports, without grabbing input\n"
		"      --capture-device <name>\n"
		"                        sysfs HID name for --capture (default: first found)\n"
		"      --capture-duration <sec>\n"
		"                        Stop --capture after this many seconds\n"
		"      --replay <file>   Replay a capture, print events, exit\n"
		"      --duration <sec>  Run for this many seconds, then exit cleanly\n"
		"      --start-delay <sec>\n"
		"                        Wait before scanning/grabbing the keyboard\n"
		"      --no-grab         Do not EVIOCGRAB; physical keyboard keeps working\n"
		"      --no-power        Ignore Fn+F5 / Fn+F6 (sleep / hibernate)\n"
		"      --self-test       Create the virtual keyboard and tap 1 2 3 after 10 s\n"
		"  -V, --version         Print version and exit\n"
		"  -h, --help            Show this help\n",
		prog);
}

enum
{
	OPT_LIST_DEVICES = 1000,
	OPT_DOCTOR,
	OPT_CAPTURE,
	OPT_CAPTURE_DEVICE,
	OPT_CAPTURE_DURATION,
	OPT_REPLAY,
	OPT_DURATION,
	OPT_START_DELAY,
	OPT_NO_GRAB,
	OPT_NO_POWER,
	OPT_SELF_TEST
};

static const struct option LongOptions[] =
{
	{ "config",           required_argument, NULL, 'c' },
	{ "verbose",          no_argument,       NULL, 'v' },
	{ "version",          no_argument,       NULL, 'V' },
	{ "help",             no_argument,       NULL, 'h' },
	{ "list-devices",     no_argument,       NULL, OPT_LIST_DEVICES },
	{ "doctor",           no_argument,       NULL, OPT_DOCTOR },
	{ "capture",          required_argument, NULL, OPT_CAPTURE },
	{ "capture-device",   required_argument, NULL, OPT_CAPTURE_DEVICE },
	{ "capture-duration", required_argument, NULL, OPT_CAPTURE_DURATION },
	{ "replay",           required_argument, NULL, OPT_REPLAY },
	{ "duration",         required_argument, NULL, OPT_DURATION },
	{ "start-delay",      required_argument, NULL, OPT_START_DELAY },
	{ "no-grab",          no_argument,       NULL, OPT_NO_GRAB },
	{ "no-power",         no_argument,       NULL, OPT_NO_POWER },
	{ "self-test",        no_argument,       NULL, OPT_SELF_TEST },
	{ NULL, 0, NULL, 0 }
};

int main(int argc, char** argv)
{
	LogInit();

	int runDurationSec = 0;
	int captureDurationSec = 0;
	int startDelaySec = 0;
	BOOLEAN doctor = FALSE;
	const char* capturePath = NULL;
	const char* captureDevice = NULL;
	const char* replayPath = NULL;
	BOOLEAN noGrabOverride = FALSE;
	BOOLEAN noPowerOverride = FALSE;
	BOOLEAN selfTest = FALSE;
	int opt;

	while ((opt = getopt_long(argc, argv, "c:vhV", LongOptions, NULL)) != -1)
	{
		switch (opt)
		{
		case 'c':
			g_ConfigPath = optarg;
			break;

		case 'v':
			LogSetLevel(LogLevelDebug);
			break;

		case 'V':
			printf("LinAppleKey %s\n", LINAPPLEKEY_VERSION);
			return 0;

		case 'h':
			PrintUsage(argv[0]);
			return 0;

		case OPT_LIST_DEVICES:
			return ListSupportedDevices();

		case OPT_DOCTOR:
			doctor = TRUE;
			break;

		case OPT_CAPTURE:
			capturePath = optarg;
			break;

		case OPT_CAPTURE_DEVICE:
			captureDevice = optarg;
			break;

		case OPT_CAPTURE_DURATION:
			if (!ParseDuration(optarg, &captureDurationSec))
			{
				LogError("invalid --capture-duration value '%s' (expected 1..86400 seconds)", optarg);
				return 2;
			}
			break;

		case OPT_REPLAY:
			replayPath = optarg;
			break;

		case OPT_DURATION:
			if (!ParseDuration(optarg, &runDurationSec))
			{
				LogError("invalid --duration value '%s' (expected 1..86400 seconds)", optarg);
				return 2;
			}
			break;

		case OPT_START_DELAY:
			if (!ParseDuration(optarg, &startDelaySec))
			{
				LogError("invalid --start-delay value '%s' (expected 1..86400 seconds)", optarg);
				return 2;
			}
			break;

		case OPT_NO_GRAB:
			noGrabOverride = TRUE;
			break;

		case OPT_NO_POWER:
			noPowerOverride = TRUE;
			break;

		case OPT_SELF_TEST:
			selfTest = TRUE;
			break;

		default:
			PrintUsage(argv[0]);
			return 2;
		}
	}

	if (optind < argc)
	{
		LogError("unexpected argument '%s'", argv[optind]);
		PrintUsage(argv[0]);
		return 2;
	}

	if (doctor)
		return RunDoctor();

	if (capturePath && replayPath)
	{
		LogError("--capture and --replay are mutually exclusive");
		return 2;
	}
	if (!capturePath && captureDevice)
	{
		LogError("--capture-device requires --capture");
		return 2;
	}
	if (!capturePath && captureDurationSec > 0)
	{
		LogError("--capture-duration requires --capture");
		return 2;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SignalHandler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	if (selfTest)
	{
		VHF_DEVICE vhf;
		VhfInitialize(&vhf);
		if (VhfInit(&vhf) < 0)
			return 1;

		LogInfo("self-test: focus a text field; tapping 1 2 3 in 10 seconds");
		sleep(10);

		if (g_StopRequested)
		{
			VhfDeinit(&vhf);
			return 0;
		}

		static const UCHAR tapKeys[] = { 0x1E, 0x1F, 0x20 }; // HID 1, 2, 3

		for (size_t i = 0; i < sizeof(tapKeys); i++)
		{
			UCHAR keys[6] = { tapKeys[i], 0, 0, 0, 0, 0 };
			UCHAR released[6] = { 0, 0, 0, 0, 0, 0 };

			VhfSendKeyboardReport(&vhf, 0, keys);
			usleep(50000);
			VhfSendKeyboardReport(&vhf, 0, released);
			usleep(500000);
		}

		VhfDeinit(&vhf);
		LogInfo("self-test: done");
		return 0;
	}

	if (capturePath)
	{
		int duration = (captureDurationSec > 0) ? captureDurationSec : runDurationSec;
		return RunCapture(capturePath, captureDevice, duration);
	}

	if (replayPath)
	{
		// Replay needs the same Swap* settings as a live run, but never
		// touches /dev/uinput, hidraw, evdev or systemctl.
		ReadDriverConfig(g_ConfigPath);
		if (noGrabOverride)
			g_bGrabEnabled = FALSE;
		if (noPowerOverride)
			g_bPowerKeysEnabled = FALSE;
		return RunReplay(replayPath);
	}

	PowerInitialize();
	ReadDriverConfig(g_ConfigPath);
	if (noGrabOverride)
		g_bGrabEnabled = FALSE;
	if (noPowerOverride)
		g_bPowerKeysEnabled = FALSE;

	if (CheckSysfs() < 0)
		return 1;
	if (ProbeUinput() < 0)
		return 1;

	if (startDelaySec > 0)
	{
		LogInfo("waiting %d second(s) before scanning/grabbing", startDelaySec);
		for (int i = 0; i < startDelaySec * 10 && !g_StopRequested; i++)
			SleepMs(100);

		if (g_StopRequested)
			return 0;
	}

	int netlinkFd = OpenUeventNetlinkSocket();
	if (netlinkFd < 0)
		LogWarn("warning: cannot open uevent netlink socket: %s; hotplug disabled", strerror(errno));

	ScanHidDevices();

	int activeDevices = 0;
	for (int i = 0; i < MAX_KEYBOARDS; i++)
	{
		if (g_Devices[i].Active)
			activeDevices++;
	}

	if (activeDevices == 0 && !g_AttachErrorReported)
	{
		if (netlinkFd >= 0)
			LogInfo("no supported Apple keyboard found; waiting for hotplug");
		else
			LogWarn("no supported Apple keyboard found; hotplug is disabled, restart after connecting it");
	}

	struct timespec deadline;
	BOOLEAN hasDeadline = FALSE;
	if (runDurationSec > 0)
	{
		clock_gettime(CLOCK_MONOTONIC, &deadline);
		deadline.tv_sec += runDurationSec;
		hasDeadline = TRUE;
		LogInfo("running for %d second(s)", runDurationSec);
	}

	while (!g_StopRequested)
	{
		if (g_ReloadRequested)
		{
			g_ReloadRequested = 0;
			ReadDriverConfig(g_ConfigPath);
			if (noGrabOverride)
				g_bGrabEnabled = FALSE;
			if (noPowerOverride)
				g_bPowerKeysEnabled = FALSE;
			ApplyGrabMode(g_bGrabEnabled);
		}

		if (g_ToggleGrabRequested)
		{
			g_ToggleGrabRequested = 0;
			ApplyGrabMode(!g_bGrabEnabled);
		}

		if (g_bGrabEnabled)
			RetryDeferredGrabs();

		struct pollfd pfds[MAX_POLL_FDS];
		int pfdDeviceIdx[MAX_POLL_FDS];
		int pfdKind[MAX_POLL_FDS];
		nfds_t nfds = 0;

		if (netlinkFd >= 0)
		{
			pfds[nfds].fd = netlinkFd;
			pfds[nfds].events = POLLIN;
			pfdDeviceIdx[nfds] = -1;
			pfdKind[nfds] = PFD_NETLINK;
			nfds++;
		}

		for (int i = 0; i < MAX_KEYBOARDS; i++)
		{
			KB_DEVICE* dev = &g_Devices[i];
			if (!dev->Active)
				continue;

			int uinputFd = VhfGetFd(&dev->Translation.Vhf);
			if (uinputFd >= 0)
			{
				pfds[nfds].fd = uinputFd;
				pfds[nfds].events = POLLIN;
				pfdDeviceIdx[nfds] = i;
				pfdKind[nfds] = PFD_UINPUT;
				nfds++;
			}

			pfds[nfds].fd = dev->HidrawFd;
			pfds[nfds].events = POLLIN;
			pfdDeviceIdx[nfds] = i;
			pfdKind[nfds] = PFD_HIDRAW;
			nfds++;

			for (int j = 0; j < dev->EventFdCount; j++)
			{
				int evfd = dev->EventFds[j];
				if (evfd >= 0)
				{
					pfds[nfds].fd = evfd;
					pfds[nfds].events = POLLIN;
					pfdDeviceIdx[nfds] = i;
					pfdKind[nfds] = PFD_EVDEV;
					nfds++;
				}
			}
		}

		int timeoutMs = 2000;
		if (hasDeadline)
		{
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);

			long long remainingMs =
				(long long)(deadline.tv_sec - now.tv_sec) * 1000LL +
				(long long)(deadline.tv_nsec - now.tv_nsec) / 1000000LL;

			if (remainingMs <= 0)
				break;
			if (remainingMs < timeoutMs)
				timeoutMs = (int)remainingMs;
		}

		int ready = poll(pfds, nfds, timeoutMs);
		if (ready < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}

		BOOLEAN needRescan = FALSE;

		for (nfds_t k = 0; k < nfds; k++)
		{
			if (!pfds[k].revents)
				continue;

			int idx = pfdDeviceIdx[k];
			int kind = pfdKind[k];

			if (kind == PFD_UINPUT)
			{
				if (idx < 0 || !g_Devices[idx].Active)
					continue;
				if (pfds[k].revents & (POLLERR | POLLHUP | POLLNVAL))
				{
					CloseKbDevice(&g_Devices[idx]);
					continue;
				}
				if (pfds[k].revents & POLLIN)
					HandleUinputEvents(&g_Devices[idx]);
			}
			else if (kind == PFD_NETLINK)
			{
				char nlbuf[4096];
				ssize_t n;
				while ((n = recv(netlinkFd, nlbuf, sizeof(nlbuf) - 1, 0)) > 0)
				{
					// recv() does not NUL-terminate; strstr() must not
					// walk past the received bytes.
					nlbuf[n] = '\0';
					if (strstr(nlbuf, "hid") || strstr(nlbuf, "input"))
						needRescan = TRUE;
				}
			}
			else if (idx >= 0 && g_Devices[idx].Active)
			{
				if (kind == PFD_HIDRAW)
				{
					if (pfds[k].revents & (POLLERR | POLLHUP | POLLNVAL))
					{
						CloseKbDevice(&g_Devices[idx]);
						continue;
					}
					if (pfds[k].revents & POLLIN)
					{
						BYTE raw[64];
						ssize_t n = read(pfds[k].fd, raw, sizeof(raw));
						if (n <= 0)
						{
							if (n < 0 && (errno == EAGAIN || errno == EINTR))
								continue;
							CloseKbDevice(&g_Devices[idx]);
						}
						else if (g_bGrabEnabled && g_Devices[idx].EventFdCount > 0)
						{
							HandleRawReport(&g_Devices[idx], raw, n);
						}
						else
						{
							// Passthrough: the physical keyboard is live, so
							// injecting the same report would duplicate input.
							DebugPrintBuffer("passthrough: <= ", raw, n);
						}
					}
				}
				else if (kind == PFD_EVDEV)
				{
					// Drain grabbed evdev node so its kernel queue never fills
					if (pfds[k].revents & (POLLERR | POLLHUP | POLLNVAL))
					{
						CloseKbDevice(&g_Devices[idx]);
						continue;
					}
					if (pfds[k].revents & POLLIN)
					{
						struct input_event drain[16];
						while (read(pfds[k].fd, drain, sizeof(drain)) > 0)
							;
					}
				}
			}
		}

		if (needRescan)
			ScanHidDevices();
	}

	for (int i = 0; i < MAX_KEYBOARDS; i++)
		CloseKbDevice(&g_Devices[i]);

	if (netlinkFd >= 0)
		close(netlinkFd);

	return 0;
}
