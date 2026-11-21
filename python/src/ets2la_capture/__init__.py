from typing import TYPE_CHECKING

from .frame_reader import Frame, FrameReader

if TYPE_CHECKING:
    from .synced_reader import SyncedFrame, SyncedReader

__version__ = "1.1.2"
__all__ = ["Frame", "FrameReader", "SyncedFrame", "SyncedReader", "__version__"]


def __getattr__(name):
    if name in ("SyncedFrame", "SyncedReader"):
        try:
            from . import synced_reader
        except ModuleNotFoundError as error:
            if error.name and error.name.split(".")[0] == "ets2la_telemetry":
                raise ImportError(
                    "SyncedReader needs the ets2la_telemetry Python package. "
                    "Install it from the telemetry repo first (pip install ./python), then reinstall ets2la_capture."
                ) from error
            raise
        return getattr(synced_reader, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")