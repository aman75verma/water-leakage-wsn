import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path

FMT = "%(asctime)s %(levelname)-7s %(name)s %(message)s"


def _rot(path: Path) -> RotatingFileHandler:
    h = RotatingFileHandler(path, maxBytes=5_000_000, backupCount=5, encoding="utf-8")
    h.setFormatter(logging.Formatter(FMT))
    return h


def setup_logging(log_dir: Path) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    console = logging.StreamHandler()
    console.setFormatter(logging.Formatter(FMT))
    root.addHandler(console)
    root.addHandler(_rot(log_dir / "server.log"))

    # leak state transitions get their own file (and still propagate to console/server.log)
    logging.getLogger("leak.events").addHandler(_rot(log_dir / "events.log"))

    # per-request lines go to access.log only (2 units x 2 Hz would flood the console)
    access = logging.getLogger("uvicorn.access")
    access.handlers = [_rot(log_dir / "access.log")]
    access.propagate = False

