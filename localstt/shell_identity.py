"""The name and icon Windows puts on LocalSTT's notifications.

A tray balloon becomes a toast on Windows 10 and 11, and the toast is attributed to
whatever the process's AppUserModelID resolves to. A Python process that never sets one
falls back to its executable, so every notification arrives labelled "Python" with the
Python icon. Claiming an ID of our own and registering a display name for it under
`HKCU\\Software\\Classes\\AppUserModelId` fixes both, without a Start menu entry.
"""

from __future__ import annotations

import os

APP_ID = "iFrosta.LocalSTT"
DISPLAY_NAME = "LocalSTT"
REGISTRY_KEY = rf"Software\Classes\AppUserModelId\{APP_ID}"


def apply(logger) -> bool:
    """Best effort. A wrong name on a toast is not worth refusing to start over."""
    if os.name != "nt":
        return False
    # Registered first: the name has to resolve by the time the first balloon is sent,
    # and the process id is what points Windows at the registration.
    registered = _register(logger)
    claimed = _claim(logger)
    return registered and claimed


def _claim(logger) -> bool:
    """Must run before any window exists -- the shell reads the id as one is created."""
    import ctypes

    try:
        result = ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(
            ctypes.c_wchar_p(APP_ID)
        )
    except (AttributeError, OSError) as exc:
        logger.debug("could not set the AppUserModelID: %s", exc)
        return False
    if result != 0:  # an HRESULT, and S_OK is the only success
        logger.debug("SetCurrentProcessExplicitAppUserModelID returned 0x%08x", result & 0xFFFFFFFF)
        return False
    return True


def _register(logger) -> bool:
    """Rewritten on every start, so a moved or regenerated icon is picked up."""
    import winreg

    from . import branding

    try:
        icon = branding.icon_file()
    except Exception as exc:  # a missing icon should not cost us the name as well
        logger.debug("notification icon unavailable: %s", exc)
        icon = None

    try:
        with winreg.CreateKeyEx(
            winreg.HKEY_CURRENT_USER, REGISTRY_KEY, 0, winreg.KEY_SET_VALUE
        ) as key:
            winreg.SetValueEx(key, "DisplayName", 0, winreg.REG_SZ, DISPLAY_NAME)
            if icon is not None:
                winreg.SetValueEx(key, "IconUri", 0, winreg.REG_SZ, str(icon))
    except OSError as exc:
        logger.debug("could not register the notification identity: %s", exc)
        return False
    return True


def unregister(logger) -> bool:
    """Used by the uninstaller; leaving the key behind is untidy, not harmful."""
    if os.name != "nt":
        return False
    import winreg

    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, REGISTRY_KEY)
    except FileNotFoundError:
        return True
    except OSError as exc:
        logger.debug("could not remove the notification identity: %s", exc)
        return False
    return True
