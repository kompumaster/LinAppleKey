#include "Driver.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

///////////////////////////////////////////////////////////////////////////////
// Capture: read raw hidraw reports, never grab or inject anything.
// Replay: feed a capture through the same A1314/A1644 translation code and
// print the resulting input events, without touching /dev at all.
///////////////////////////////////////////////////////////////////////////////

typedef struct _KEY_NAME
{
	uint16_t    Code;
	const char* Name;
} KEY_NAME;

#define K(c) { c, #c }

static const KEY_NAME g_KeyNames[] =
{
	K(KEY_ESC), K(KEY_1), K(KEY_2), K(KEY_3), K(KEY_4), K(KEY_5),
	K(KEY_6), K(KEY_7), K(KEY_8), K(KEY_9), K(KEY_0),
	K(KEY_MINUS), K(KEY_EQUAL), K(KEY_BACKSPACE), K(KEY_TAB),
	K(KEY_Q), K(KEY_W), K(KEY_E), K(KEY_R), K(KEY_T), K(KEY_Y),
	K(KEY_U), K(KEY_I), K(KEY_O), K(KEY_P),
	K(KEY_LEFTBRACE), K(KEY_RIGHTBRACE), K(KEY_ENTER),
	K(KEY_LEFTCTRL), K(KEY_A), K(KEY_S), K(KEY_D), K(KEY_F), K(KEY_G),
	K(KEY_H), K(KEY_J), K(KEY_K), K(KEY_L),
	K(KEY_SEMICOLON), K(KEY_APOSTROPHE), K(KEY_GRAVE),
	K(KEY_LEFTSHIFT), K(KEY_BACKSLASH), K(KEY_Z), K(KEY_X), K(KEY_C),
	K(KEY_V), K(KEY_B), K(KEY_N), K(KEY_M),
	K(KEY_COMMA), K(KEY_DOT), K(KEY_SLASH), K(KEY_RIGHTSHIFT),
	K(KEY_KPASTERISK), K(KEY_LEFTALT), K(KEY_SPACE), K(KEY_CAPSLOCK),
	K(KEY_F1), K(KEY_F2), K(KEY_F3), K(KEY_F4), K(KEY_F5), K(KEY_F6),
	K(KEY_F7), K(KEY_F8), K(KEY_F9), K(KEY_F10), K(KEY_F11), K(KEY_F12),
	K(KEY_SYSRQ), K(KEY_SCROLLLOCK), K(KEY_PAUSE),
	K(KEY_HOME), K(KEY_UP), K(KEY_PAGEUP), K(KEY_LEFT),
	K(KEY_RIGHT), K(KEY_END), K(KEY_DOWN), K(KEY_PAGEDOWN),
	K(KEY_INSERT), K(KEY_DELETE),
	K(KEY_MUTE), K(KEY_VOLUMEDOWN), K(KEY_VOLUMEUP),
	K(KEY_PLAYPAUSE), K(KEY_STOPCD), K(KEY_PREVIOUSSONG), K(KEY_NEXTSONG),
	K(KEY_BRIGHTNESSDOWN), K(KEY_BRIGHTNESSUP),
	K(KEY_SLEEP), K(KEY_SUSPEND),
	K(KEY_102ND), K(KEY_COMPOSE),
	K(KEY_LEFTMETA), K(KEY_RIGHTMETA), K(KEY_RIGHTCTRL), K(KEY_RIGHTALT)
};

#undef K

static const char* KeyName(uint16_t code)
{
	for (size_t i = 0; i < sizeof(g_KeyNames) / sizeof(g_KeyNames[0]); i++)
	{
		if (g_KeyNames[i].Code == code)
			return g_KeyNames[i].Name;
	}
	return NULL;
}

static void LogEvent(PVOID context, uint16_t type, uint16_t code, int32_t value)
{
	(void)context;
	if (type == EV_SYN)
	{
		printf("SYN\n");
		return;
	}

	if (type == EV_KEY)
	{
		const char* name = KeyName(code);
		if (name)
			printf("%s %d\n", name, value);
		else
			printf("KEY_%u %d\n", (unsigned)code, value);
		return;
	}

	printf("EV_%u %u %d\n", (unsigned)type, (unsigned)code, value);
}

static double ElapsedMs(const struct timespec* start)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	return (double)(now.tv_sec - start->tv_sec) * 1000.0 +
	       (double)(now.tv_nsec - start->tv_nsec) / 1000000.0;
}

static const char* ModelTag(APPLE_KB_MODEL model)
{
	return (model == KbModelA1314) ? "A1314" : "A1644";
}

int RunCapture(const char* path, const char* sysName, int durationSec)
{
	CAPTURE_TARGET target;
	if (!FindCaptureTarget(sysName, &target))
	{
		LogError("capture: no supported Apple keyboard with hidraw found%s%s",
			sysName ? ": " : "", sysName ? sysName : "");
		return 1;
	}

	int fd = open(target.HidrawPath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
	{
		LogError("capture: cannot open %s: %s", target.HidrawPath, strerror(errno));
		return 1;
	}

	int outFd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (outFd < 0)
	{
		LogError("capture: cannot write %s: %s", path, strerror(errno));
		close(fd);
		return 1;
	}

	// A capture contains everything typed on the keyboard; never leave it
	// world-readable, even if the file already existed with a wider mode.
	(void)fchmod(outFd, 0600);

	FILE* fp = fdopen(outFd, "w");
	if (!fp)
	{
		LogError("capture: cannot open %s for writing: %s", path, strerror(errno));
		close(outFd);
		close(fd);
		return 1;
	}

	fprintf(fp, "# LinAppleKey capture v1\n");
	fprintf(fp, "DEVICE %s\n", target.SysName);
	fprintf(fp, "MODEL %s\n", ModelTag(target.Model));
	fprintf(fp, "BUS %04X\n", target.Bus);
	fprintf(fp, "IDS %04X:%04X\n", target.Vid, target.Pid);
	fprintf(fp, "# time_ms direction length hex...\n");
	fflush(fp);

	LogInfo("capturing %s (%s, %s) -> %s",
		target.SysName, target.HidrawPath, ModelTag(target.Model), path);
	LogInfo("press keys on the keyboard; Ctrl+C stops capture");

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);

	int reports = 0;

	while (!g_StopRequested)
	{
		int timeoutMs = 1000;

		if (durationSec > 0)
		{
			double remainingMs = (double)durationSec * 1000.0 - ElapsedMs(&start);
			if (remainingMs <= 0)
				break;
			if (remainingMs < timeoutMs)
				timeoutMs = (int)remainingMs;
		}

		struct pollfd pfd;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		int ready = poll(&pfd, 1, timeoutMs);
		if (ready < 0)
		{
			if (errno == EINTR)
				continue;
			LogError("capture: poll failed: %s", strerror(errno));
			break;
		}
		if (ready == 0)
			continue;

		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
		{
			LogError("capture: hidraw closed or failed");
			break;
		}
		if (!(pfd.revents & POLLIN))
			continue;

		BYTE raw[64];
		ssize_t n = read(fd, raw, sizeof(raw));
		if (n < 0)
		{
			if (errno == EAGAIN || errno == EINTR)
				continue;
			LogError("capture: read failed: %s", strerror(errno));
			break;
		}
		if (n == 0)
			break;

		fprintf(fp, "%.3f R %zd", ElapsedMs(&start), n);
		for (ssize_t i = 0; i < n; i++)
			fprintf(fp, " %02X", raw[i]);
		fprintf(fp, "\n");
		fflush(fp);
		reports++;
	}

	fclose(fp);
	close(fd);

	LogInfo("captured %d report(s) to %s", reports, path);
	return 0;
}

int RunReplay(const char* path)
{
	FILE* fp = fopen(path, "r");
	if (!fp)
	{
		LogError("replay: cannot open %s: %s", path, strerror(errno));
		return 1;
	}

	APPLE_KB_MODEL model = KbModelNone;
	int reports = 0;
	int errors = 0;
	char line[1024];

	TRANSLATOR_CONTEXT ctx;
	TranslatorInitialize(&ctx, KbModelNone);
	VhfSetEventSink(&ctx.Vhf, LogEvent, NULL);

	// Never fork systemctl from a replay, even if the config says so.
	g_dwDirectPowerAction = 0;
	g_bReplayMode = TRUE;

	while (fgets(line, sizeof(line), fp))
	{
		if (g_StopRequested)
		{
			errors++;
			break;
		}

		char* p = line;
		while (isspace((unsigned char)*p))
			p++;

		if (*p == '\0' || *p == '#')
			continue;

		if (strncmp(p, "MODEL", 5) == 0 && isspace((unsigned char)p[5]))
		{
			char name[16];
			if (sscanf(p + 5, "%15s", name) == 1)
			{
				if (strcasecmp(name, "A1314") == 0)
					model = KbModelA1314;
				else if (strcasecmp(name, "A1644") == 0)
					model = KbModelA1644;
				else
				{
					LogError("replay: unknown model '%s'", name);
					errors++;
				}

				ctx.Model = model;
			}
			continue;
		}

		if (strncmp(p, "DEVICE", 6) == 0 || strncmp(p, "BUS", 3) == 0 ||
		    strncmp(p, "IDS", 3) == 0)
			continue;

		if (model == KbModelNone)
		{
			LogError("replay: report before MODEL header");
			errors++;
			break;
		}

		char* end = NULL;
		(void)strtod(p, &end);
		if (end == p)
		{
			LogError("replay: cannot parse line: %s", line);
			errors++;
			continue;
		}

		p = end;
		while (isspace((unsigned char)*p))
			p++;

		char direction = *p;
		if (direction != 'R' && direction != 'W')
		{
			LogError("replay: bad direction in line: %s", line);
			errors++;
			continue;
		}
		p++;

		while (isspace((unsigned char)*p))
			p++;

		long len = strtol(p, &end, 10);
		if (end == p || len < 1 || len > 64)
		{
			LogError("replay: bad length in line: %s", line);
			errors++;
			continue;
		}
		p = end;

		BYTE raw[64];
		int got = 0;

		for (long i = 0; i < len; i++)
		{
			while (isspace((unsigned char)*p))
				p++;
			if (*p == '\0')
				break;

			unsigned long value = strtoul(p, &end, 16);
			if (end == p || value > 0xFF)
				break;

			raw[i] = (BYTE)value;
			p = end;
			got++;
		}

		if (got != (int)len)
		{
			LogError("replay: expected %ld bytes, got %d: %s", len, got, line);
			errors++;
			continue;
		}

		if (direction != 'R')
			continue;

		HandleRawReportForContext(&ctx, raw, len);
		reports++;
	}

	fclose(fp);
	VhfSetEventSink(&ctx.Vhf, NULL, NULL);
	g_bReplayMode = FALSE;

	if (model == KbModelNone)
	{
		LogError("replay: no MODEL header in %s", path);
		return 1;
	}

	LogInfo("replayed %d report(s) from %s (model %s)",
		reports, path, ModelTag(model));

	return (reports > 0 && errors == 0) ? 0 : 1;
}
