#include "Driver.h"

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

///////////////////////////////////////////////////////////////////////////////
// Fn+F5 -> system sleep, Fn+F6 -> hibernate.
//
// Must only be called on the key-down transition: a HID report is level
// triggered, so firing while held would put the machine straight back to
// sleep the moment it woke up.
//
// By default (DirectPowerAction = 0), emits native KEY_SLEEP / KEY_SUSPEND
// input events on the virtual HID device so the desktop environment (KDE
// PowerDevil, GNOME, systemd-logind) handles screen locking and inhibitors.
// When DirectPowerAction = 1, invokes "systemctl suspend/hibernate --no-block"
// asynchronously in a child process.
///////////////////////////////////////////////////////////////////////////////

VOID PowerInitialize(VOID)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_NOCLDWAIT;
	sigaction(SIGCHLD, &sa, NULL);
}

VOID WinAppleKeyRequestPowerAction(VHF_DEVICE* dev, POWER_ACTION action)
{
	if (action == PowerActionNone)
		return;

	if (!g_bPowerKeysEnabled)
	{
		DebugPrint("Power: action %d suppressed by --no-power\n", (int)action);
		return;
	}

	// Replay only logs events through the sink; never touch the real
	// power manager.
	if (g_bReplayMode)
	{
		VhfSendPowerKey(dev, action);
		return;
	}

	DebugPrint("Power: requested action %d (DirectPowerAction=%u)\n",
		(int)action, (unsigned)g_dwDirectPowerAction);

	// KEY_SLEEP works for suspend on Linux desktops. There is no equally
	// reliable native hibernate key, so hibernate always goes through
	// systemctl (the service runs as root).
	if (!g_dwDirectPowerAction && action == PowerActionSleep)
	{
		VhfSendPowerKey(dev, action);
		return;
	}

	if (!g_dwDirectPowerAction)
		DebugPrint("Power: hibernate via systemctl (native KEY_SUSPEND is unreliable)\n");

	const char* subcmd = (action == PowerActionHibernate) ? "hibernate" : "suspend";
	pid_t pid = fork();
	if (pid == 0)
	{
		char* const argv[] = { "systemctl", (char*)subcmd, "--no-block", NULL };

		// Do not search PATH as root.
		execv("/usr/bin/systemctl", argv);
		execv("/bin/systemctl", argv);
		_exit(127);
	}
}
