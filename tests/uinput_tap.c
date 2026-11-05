/*
 * Minimal uinput diagnostic.
 *
 * Creates a tiny virtual keyboard, waits two seconds, taps KEY_A, then
 * destroys the device. Run it with a text editor or terminal focused:
 *
 *   cc -O2 -o build/uinput_tap tests/uinput_tap.c
 *   ./build/uinput_tap
 *
 * If "a" appears, KWin accepts ordinary uinput keyboards and the problem
 * is in the LinAppleKey virtual device configuration. If nothing appears,
 * KWin is ignoring uinput devices in this session.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int emit(int fd, unsigned short type, unsigned short code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	return write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

int main(void)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
	{
		fprintf(stderr, "open /dev/uinput: %s\n", strerror(errno));
		return 1;
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	ioctl(fd, UI_SET_KEYBIT, KEY_A);

	struct uinput_setup usetup;
	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor = 0x1209;
	usetup.id.product = 0x0002;
	usetup.id.version = 1;
	snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "LinAppleKey uinput tap");

	if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 ||
	    ioctl(fd, UI_DEV_CREATE) < 0)
	{
		fprintf(stderr, "cannot create uinput device: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	fprintf(stderr, "focus a text field; tapping A in 2 seconds...\n");
	sleep(2);

	int failed = 0;
	failed |= emit(fd, EV_KEY, KEY_A, 1);
	failed |= emit(fd, EV_SYN, SYN_REPORT, 0);
	failed |= emit(fd, EV_KEY, KEY_A, 0);
	failed |= emit(fd, EV_SYN, SYN_REPORT, 0);

	sleep(1);
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);

	if (failed)
	{
		fprintf(stderr, "one or more writes failed: %s\n", strerror(errno));
		return 1;
	}

	fprintf(stderr, "done\n");
	return 0;
}
