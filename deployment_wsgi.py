"""Serve the existing Flask API and built Vite assets from one WSGI app."""

from whitenoise import WhiteNoise

from backend.app import app

application = WhiteNoise(app, root="/app/frontend/dist", index_file=True)
