from __future__ import annotations

from . import preflight, settings_window, winui
from .config import load_config
from .logging_setup import setup_logging
from .service import STTService
from .tray_app import LocalSTTTrayApp


def main() -> None:
    # Has to happen before anything creates a window, or Windows stretches every bitmap
    # the app draws on a scaled display.
    winui.enable_dpi_awareness()

    logger = setup_logging()
    config = load_config()

    if preflight.should_run():
        logger.info("running the self-test: this device has not passed one yet")
        report = preflight.run(config, logger)
        preflight.save(report)
        if report.blocking_failures:
            names = ", ".join(check.name for check in report.blocking_failures)
            logger.error("LocalSTT cannot start on this device: %s", names)
            settings_window.show_report_blocking(config, logger)
            return

    service = STTService(config, logger)
    LocalSTTTrayApp(config, service).run()


if __name__ == "__main__":
    main()
