/*
 * uinput capability probe.
 *
 * Creates several virtual keyboards with increasing capability sets and taps
 * the digit matching each variant. Run with a scratch text field focused:
 *
 *   cc -O2 -o build/uinput_probe tests/uinput_probe.c
 *   ./build/uinput_probe
 *
 * The digits that appear in the text field identify which capability set
 * KWin accepts. Example: "1 2 3 4" means variants 5-7 were ignored.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const unsigned short StandardKeys[] =
{
	KEY_ESC,
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB,
	KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
	KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_ENTER,
	KEY_LEFTCTRL, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
	KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
	KEY_LEFTSHIFT, KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
	KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_RIGHTSHIFT,
	KEY_KPASTERISK, KEY_LEFTALT, KEY_SPACE, KEY_CAPSLOCK,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
	KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
	KEY_SYSRQ, KEY_SCROLLLOCK, KEY_PAUSE,
	KEY_HOME, KEY_UP, KEY_PAGEUP, KEY_LEFT, KEY_RIGHT, KEY_END,
	KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE,
	KEY_RIGHTCTRL, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA
};

static const unsigned short MediaKeys[] =
{
	KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP,
	KEY_BRIGHTNESSDOWN, KEY_BRIGHTNESSUP,
	KEY_PLAYPAUSE, KEY_STOPCD, KEY_PREVIOUSSONG, KEY_NEXTSONG
};

static const unsigned short PowerKeys[] =
{
	KEY_SLEEP, KEY_SUSPEND, KEY_COMPOSE
};

static const unsigned short Digits[] =
{
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0
};

static volatile int g_reader_stop = 0;

static void* reader_thread(void* arg)
{
	int fd = *(int*)arg;
	struct input_event ev;

	while (!g_reader_stop)
	{
		struct pollfd p;
		p.fd = fd;
		p.events = POLLIN;
		p.revents = 0;

		if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN))
		{
			while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
				;
		}
	}

	return NULL;
}

static void set_key(int fd, unsigned short code)
{
	ioctl(fd, UI_SET_KEYBIT, code);
}

static int emit(int fd, unsigned short type, unsigned short code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	return write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

static int create_device(int variant)
{
	int open_mode = (variant >= 8) ? O_RDWR : O_WRONLY;
	int fd = open("/dev/uinput", open_mode | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
	{
		fprintf(stderr, "open /dev/uinput: %s\n", strerror(errno));
		return -1;
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);

	if (variant >= 3)
	{
		ioctl(fd, UI_SET_EVBIT, EV_REP);
		ioctl(fd, UI_SET_EVBIT, EV_LED);
		ioctl(fd, UI_SET_LEDBIT, LED_NUML);
		ioctl(fd, UI_SET_LEDBIT, LED_CAPSL);
		ioctl(fd, UI_SET_LEDBIT, LED_SCROLLL);
		ioctl(fd, UI_SET_LEDBIT, LED_COMPOSE);
		ioctl(fd, UI_SET_LEDBIT, LED_KANA);
	}

	// Every variant must be able to type its own digit.
	set_key(fd, Digits[variant - 1]);

	if (variant >= 2)
	{
		for (size_t i = 0; i < sizeof(StandardKeys) / sizeof(StandardKeys[0]); i++)
			set_key(fd, StandardKeys[i]);
	}

	if (variant >= 4)
	{
		for (size_t i = 0; i < sizeof(MediaKeys) / sizeof(MediaKeys[0]); i++)
			set_key(fd, MediaKeys[i]);
	}

	if (variant >= 5)
	{
		for (size_t i = 0; i < sizeof(PowerKeys) / sizeof(PowerKeys[0]); i++)
			set_key(fd, PowerKeys[i]);
	}

	if (variant >= 6)
	{
		// Superset of everything LinAppleKey registers, including KEY_UNKNOWN.
		for (unsigned short k = 1; k < 256; k++)
			set_key(fd, k);
	}

	struct uinput_setup usetup;
	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor = 0x1209;
	usetup.id.product = (variant == 7) ? 0xA131 : (0x0100 + variant);
	usetup.id.version = 1;

	if (variant == 7)
		snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "LinAppleKey Virtual HID Device");
	else
		snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "LAK uinput probe %d", variant);

	if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 ||
	    ioctl(fd, UI_DEV_CREATE) < 0)
	{
		fprintf(stderr, "variant %d: cannot create device: %s\n", variant, strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

static void tap_digit(int fd, int variant)
{
	unsigned short key = Digits[variant - 1];

	if (variant == 10)
		usleep(200000);
	else
		sleep(2);

	fprintf(stderr, "variant %d: tapping %d\n", variant, variant);
	emit(fd, EV_KEY, key, 1);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	emit(fd, EV_KEY, key, 0);
	emit(fd, EV_SYN, SYN_REPORT, 0);
	sleep(1);
}

static void destroy_device(int fd)
{
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
}

int main(void)
{
	fprintf(stderr,
		"Focus a scratch text field. Each variant waits 2s, then taps its number.\n"
		"Variants: 1=minimal, 2=standard keys, 3=+LED/REP, 4=+media,\n"
		"          5=+sleep/suspend, 6=+all keycodes, 7=daemon name/IDs,\n"
		"          8=variant 6 opened O_RDWR instead of O_WRONLY,\n"
		"          9=variant 8 plus a reader thread on the uinput fd,\n"
		"          10=variant 8 with only a 200ms wait\n");

	for (int variant = 1; variant <= 10; variant++)
	{
		int fd = create_device(variant);
		if (fd < 0)
			continue;

		pthread_t reader;
		int reader_active = 0;

		if (variant == 9)
		{
			g_reader_stop = 0;
			if (pthread_create(&reader, NULL, reader_thread, &fd) == 0)
				reader_active = 1;
		}

		tap_digit(fd, variant);

		if (reader_active)
		{
			g_reader_stop = 1;
			pthread_join(reader, NULL);
		}

		destroy_device(fd);
	}

	fprintf(stderr, "probe done\n");
	return 0;
}
