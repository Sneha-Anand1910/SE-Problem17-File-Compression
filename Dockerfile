FROM node:22-bookworm-slim AS frontend-build
WORKDIR /app/frontend
COPY frontend/package*.json ./
RUN npm ci
COPY frontend/ ./
RUN npm run build

FROM debian:bookworm-slim AS cpp-build
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ make zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY CMakeLists.txt ./
COPY src/ src/
COPY tests/smoke_test.cpp tests/performance_test.cpp tests/
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target file-compress --parallel 2

FROM python:3.13-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends zlib1g \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY backend/requirements.txt backend/requirements.txt
RUN pip install --no-cache-dir -r backend/requirements.txt 'gunicorn>=23,<24' 'whitenoise>=6.9,<7'
COPY backend/app.py backend/__init__.py backend/
COPY deployment_wsgi.py ./
COPY --from=frontend-build /app/frontend/dist/ frontend/dist/
COPY --from=cpp-build /app/build/file-compress bin/file-compress
RUN useradd --create-home --uid 10001 appuser
USER appuser
ENV COMPRESSION_BINARY=/app/bin/file-compress \
    PYTHONUNBUFFERED=1
EXPOSE 10000
CMD ["sh", "-c", "exec gunicorn --bind 0.0.0.0:${PORT:-10000} --workers 1 --threads 4 --timeout 120 deployment_wsgi:application"]
