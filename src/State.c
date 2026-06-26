#include "State.h"

BOOL ShouldDisableWebHelper(DWORD runningAppId, BOOL appMarkedRunning, BOOL liveGameProcess,
                            WEBHELPER_OVERRIDE overrideMode)
{
    switch (overrideMode)
    {
    case WEBHELPER_FORCE_ENABLE:
        return FALSE;

    case WEBHELPER_FORCE_DISABLE:
        return TRUE;

    case WEBHELPER_AUTO:
    default:
        /*
         * The most reliable signal that a Steam game is running is
         * the combination of RunningAppID != 0 and the Running flag
         * being set for that AppID.  Both are set atomically by
         * Steam when the user clicks "Play" and cleared when the
         * game exits.
         *
         * We no longer require liveGameProcess (checking for a non-
         * Steam descendant process) because:
         *   - On modern Steam the result is always TRUE due to
         *     "millennium" helper processes (Steam Deck UI), making
         *     the check useless.
         *   - On older Steam without millennium, the check could
         *     incorrectly return FALSE if no game process had
         *     spawned yet (race at launch) or if the game executable
         *     was removed/replaced after registration.
         *   - The registry-based RunningAppID + Running flag pair is
         *     the authoritative source of truth from Steam itself.
         */
        (void)liveGameProcess;
        return runningAppId != 0 && appMarkedRunning;
    }
}
