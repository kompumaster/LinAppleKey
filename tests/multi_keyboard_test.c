/*
 * Multi-keyboard isolation test.
 *
 * Two A1314 translator contexts feed events into two independent sinks.
 * The test checks that a report/release on one keyboard never affects the
 * other one, including media, power and rollover reports.
 *
 * Driver.c is linked with main renamed to linapplekey_main by the Makefile.
 */

#include "Driver.h"

#include <linux/input.h>

typedef struct TEST_EVENT
{
	uint16_t type;
	uint16_t code;
	int32_t value;
} TEST_EVENT;

typedef struct TEST_CTX
{
	const char* name;
	int count;
	TEST_EVENT events[128];
} TEST_CTX;

static void LogEvent(PVOID context, uint16_t type, uint16_t code, int32_t value)
{
	TEST_CTX* ctx = (TEST_CTX*)context;

	if (ctx->count < (int)(sizeof(ctx->events) / sizeof(ctx->events[0])))
	{
		ctx->events[ctx->count].type = type;
		ctx->events[ctx->count].code = code;
		ctx->events[ctx->count].value = value;
	}
	ctx->count++;
}

static void InitContext(TEST_CTX* test, TRANSLATOR_CONTEXT* tr, const char* name, APPLE_KB_MODEL model)
{
	memset(test, 0, sizeof(*test));
	test->name = name;
	TranslatorInitialize(tr, model);
	VhfSetEventSink(&tr->Vhf, LogEvent, test);
}

static void Feed(TRANSLATOR_CONTEXT* tr, const BYTE* raw, size_t len)
{
	HandleRawReportForContext(tr, raw, (ssize_t)len);
}

static int CheckEvents(const TEST_CTX* ctx, const TEST_EVENT* expected, int count)
{
	if (ctx->count != count)
	{
		fprintf(stderr, "FAIL %s: expected %d events, got %d\n",
			ctx->name, count, ctx->count);
		return 1;
	}

	for (int i = 0; i < count; i++)
	{
		if (ctx->events[i].type != expected[i].type ||
		    ctx->events[i].code != expected[i].code ||
		    ctx->events[i].value != expected[i].value)
		{
			fprintf(stderr,
				"FAIL %s event %d: expected type=%u code=%u value=%d, got type=%u code=%u value=%d\n",
				ctx->name, i,
				expected[i].type, expected[i].code, expected[i].value,
				ctx->events[i].type, ctx->events[i].code, ctx->events[i].value);
			return 1;
		}
	}

	return 0;
}

static int TestIndependentKeys(void)
{
	TRANSLATOR_CONTEXT a, b;
	TEST_CTX ta, tb;

	InitContext(&ta, &a, "a", KbModelA1314);
	InitContext(&tb, &b, "b", KbModelA1314);

	static const BYTE a_down[] = { 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0 };
	static const BYTE b_down[] = { 0x01, 0x00, 0x00, 0x05, 0, 0, 0, 0, 0 };
	static const BYTE release[] = { 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0 };

	static const TEST_EVENT a_expected[] = {
		{ EV_KEY, KEY_A, 1 }, { EV_SYN, SYN_REPORT, 0 },
		{ EV_KEY, KEY_A, 0 }, { EV_SYN, SYN_REPORT, 0 },
	};
	static const TEST_EVENT b_expected[] = {
		{ EV_KEY, KEY_B, 1 }, { EV_SYN, SYN_REPORT, 0 },
		{ EV_KEY, KEY_B, 0 }, { EV_SYN, SYN_REPORT, 0 },
	};

	Feed(&a, a_down, sizeof(a_down));
	Feed(&b, b_down, sizeof(b_down));
	Feed(&a, release, sizeof(release));

	if (CheckEvents(&tb, b_expected, 2) != 0)
		return 1;

	Feed(&b, release, sizeof(release));

	if (CheckEvents(&ta, a_expected, 4) != 0)
		return 1;
	if (CheckEvents(&tb, b_expected, 4) != 0)
		return 1;

	return 0;
}

static int TestSameKey(void)
{
	TRANSLATOR_CONTEXT a, b;
	TEST_CTX ta, tb;

	InitContext(&ta, &a, "a", KbModelA1314);
	InitContext(&tb, &b, "b", KbModelA1314);

	static const BYTE down[] = { 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0 };
	static const BYTE release[] = { 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0 };
	static const TEST_EVENT expected[] = {
		{ EV_KEY, KEY_A, 1 }, { EV_SYN, SYN_REPORT, 0 },
		{ EV_KEY, KEY_A, 0 }, { EV_SYN, SYN_REPORT, 0 },
	};

	Feed(&a, down, sizeof(down));
	Feed(&b, down, sizeof(down));
	Feed(&a, release, sizeof(release));

	if (CheckEvents(&tb, expected, 2) != 0)
		return 1;

	Feed(&b, release, sizeof(release));

	if (CheckEvents(&ta, expected, 4) != 0)
		return 1;
	if (CheckEvents(&tb, expected, 4) != 0)
		return 1;

	return 0;
}

static int TestMedia(void)
{
	TRANSLATOR_CONTEXT a, b;
	TEST_CTX ta, tb;

	InitContext(&ta, &a, "a", KbModelA1314);
	InitContext(&tb, &b, "b", KbModelA1314);

	static const BYTE ctrl[]    = { 0x01, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0 };
	static const BYTE ctrl_f10[] = { 0x01, 0x01, 0x00, 0x43, 0, 0, 0, 0, 0 };
	static const BYTE release[] = { 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0 };
	static const TEST_EVENT expected[] = {
		{ EV_KEY, KEY_MUTE, 1 }, { EV_SYN, SYN_REPORT, 0 },
		{ EV_KEY, KEY_MUTE, 0 }, { EV_SYN, SYN_REPORT, 0 },
	};

	Feed(&a, ctrl, sizeof(ctrl));
	Feed(&a, ctrl_f10, sizeof(ctrl_f10));
	Feed(&a, ctrl, sizeof(ctrl));
	Feed(&a, release, sizeof(release));

	if (CheckEvents(&ta, expected, 4) != 0)
		return 1;
	if (CheckEvents(&tb, expected, 0) != 0)
		return 1;

	return 0;
}

static int TestPower(void)
{
	TRANSLATOR_CONTEXT a, b;
	TEST_CTX ta, tb;

	InitContext(&ta, &a, "a", KbModelA1314);
	InitContext(&tb, &b, "b", KbModelA1314);

	static const BYTE ctrl[]   = { 0x01, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0 };
	static const BYTE ctrl_f5[] = { 0x01, 0x01, 0x00, 0x3e, 0, 0, 0, 0, 0 };
	static const BYTE release[] = { 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0 };
	static const TEST_EVENT expected[] = {
		{ EV_KEY, KEY_SLEEP, 1 }, { EV_SYN, SYN_REPORT, 0 },
		{ EV_KEY, KEY_SLEEP, 0 }, { EV_SYN, SYN_REPORT, 0 },
	};

	Feed(&a, ctrl, sizeof(ctrl));
	Feed(&a, ctrl_f5, sizeof(ctrl_f5));
	Feed(&a, ctrl, sizeof(ctrl));
	Feed(&a, release, sizeof(release));

	if (CheckEvents(&ta, expected, 4) != 0)
		return 1;
	if (CheckEvents(&tb, expected, 0) != 0)
		return 1;

	return 0;
}

static int TestA1644Independent(void)
{
	TRANSLATOR_CONTEXT a, b;
	TEST_CTX ta, tb;

	InitContext(&ta, &a, "a1644", KbModelA1644);
	InitContext(&tb, &b, "b1644", KbModelA1644);

	static const BYTE a_down[]  = { 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0, 0 };
	static const BYTE b_down[]  = { 0x01, 0x00, 0x00, 0x05, 0, 0, 0, 0, 0, 0 };
	static const BYTE release[]  = { 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0 };
	static const TEST_EVENT a_expected[] = {
		{ EV_KEY, KEY_A, 1 }, { EV_SYN, SYN_REPORT, 0 },
		{ EV_KEY, KEY_A, 0 }, { EV_SYN, SYN_REPORT, 0 },
	};
	static const TEST_EVENT b_expected[] = {
		{ EV_KEY, KEY_B, 1 }, { EV_SYN, SYN_REPORT, 0 },
		{ EV_KEY, KEY_B, 0 }, { EV_SYN, SYN_REPORT, 0 },
	};

	Feed(&a, a_down, sizeof(a_down));
	Feed(&b, b_down, sizeof(b_down));
	Feed(&a, release, sizeof(release));

	if (CheckEvents(&tb, b_expected, 2) != 0)
		return 1;

	Feed(&b, release, sizeof(release));

	if (CheckEvents(&ta, a_expected, 4) != 0)
		return 1;
	if (CheckEvents(&tb, b_expected, 4) != 0)
		return 1;

	return 0;
}

static int TestRolloverIsolation(void)
{
	TRANSLATOR_CONTEXT a, b;
	TEST_CTX ta, tb;

	InitContext(&ta, &a, "a", KbModelA1314);
	InitContext(&tb, &b, "b", KbModelA1314);

	static const BYTE a_down[]  = { 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0 };
	static const BYTE b_down[]  = { 0x01, 0x00, 0x00, 0x05, 0, 0, 0, 0, 0 };
	static const BYTE rollover[] = { 0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
	static const BYTE release[]  = { 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0 };
	static const TEST_EVENT a_expected[] = {
		{ EV_KEY, KEY_A, 1 }, { EV_SYN, SYN_REPORT, 0 },
		{ EV_KEY, KEY_A, 0 }, { EV_SYN, SYN_REPORT, 0 },
	};
	static const TEST_EVENT b_expected[] = {
		{ EV_KEY, KEY_B, 1 }, { EV_SYN, SYN_REPORT, 0 },
		{ EV_KEY, KEY_B, 0 }, { EV_SYN, SYN_REPORT, 0 },
	};

	Feed(&a, a_down, sizeof(a_down));
	Feed(&a, rollover, sizeof(rollover));
	Feed(&b, b_down, sizeof(b_down));
	Feed(&a, release, sizeof(release));
	Feed(&b, release, sizeof(release));

	if (CheckEvents(&ta, a_expected, 4) != 0)
		return 1;
	if (CheckEvents(&tb, b_expected, 4) != 0)
		return 1;

	return 0;
}

int main(void)
{
	g_bReplayMode = TRUE;
	g_dwDirectPowerAction = 0;
	g_bPowerKeysEnabled = TRUE;

	if (TestIndependentKeys() != 0 ||
	    TestSameKey() != 0 ||
	    TestA1644Independent() != 0 ||
	    TestMedia() != 0 ||
	    TestPower() != 0 ||
	    TestRolloverIsolation() != 0)
		return 1;

	fprintf(stderr, "multi-keyboard test OK\n");
	return 0;
}
