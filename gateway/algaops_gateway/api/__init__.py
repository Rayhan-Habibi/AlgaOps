"""Local HTTP API (FastAPI) + Server-Sent Events for the offline dashboard."""

from .events import EventBus
from .server import AppContext, create_app

__all__ = ["AppContext", "EventBus", "create_app"]
