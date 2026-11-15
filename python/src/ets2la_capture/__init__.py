from .frame_reader import Frame, FrameReader

__version__ = "1.1.0"
__all__ = ["Frame", "FrameReader", "SyncedFrame", "SyncedReader", "__version__"]


def __getattr__(name):
    if name in ("SyncedFrame", "SyncedReader"):
        from . import synced_reader

        return getattr(synced_reader, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")