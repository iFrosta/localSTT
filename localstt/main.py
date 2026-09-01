from __future__ import annotations

from .config import load_config
from .logging_setup import setup_logging
from .service import STTService
from .tray_app import LocalSTTTrayApp


def main() -> None:
    logger = setup_logging()
    config = load_config()
    service = STTService(config, logger)
    LocalSTTTrayApp(config, service).run()


if __name__ == "__main__":
    main()
