# Deployment Guide

## Local Docker check

From the repository root, run:

```sh
docker build -t file-compression-tool .
docker run --rm -p 10000:10000 -e PORT=10000 file-compression-tool
```

Open `http://localhost:10000`. Select files to compress, then use the downloaded ZIP with List and Extract. All three features use relative `/api` requests on the same container origin. Stop the container with Ctrl+C.

## Render setup

1. In Render, connect your GitHub account and grant access to `shubharth2006/SE-Problem17-File-Compression`.
2. After the deployment files have been reviewed and merged into `main`, create a **Blueprint** from that repository and select its root `render.yaml`. This creates one Docker web service linked to `main`.
3. Confirm the service shows **After CI Checks Pass** for Auto-Deploy. The Blueprint sets `autoDeployTrigger: checksPass`; Render waits for GitHub checks on each new `main` commit. The existing GitHub Actions CI runs on pushes to `main`.
4. Watch the first build and deployment in Render, then open the assigned `onrender.com` URL. Verify the page and all three ZIP actions there before calling the deployment successful.

The container binds to `0.0.0.0` and uses Render's `PORT` value (defaulting to `10000` locally). `COMPRESSION_BINARY` points to the C++ executable bundled in the image. No secret is required for the existing local ZIP workflow. The Vite development proxy applies only when running Vite locally; in the container, WhiteNoise serves the built frontend and Gunicorn runs the existing Flask API on one port.

## Limits

Uploads and ZIP processing use temporary container storage. Files are returned to the browser and are not retained between requests or deployments. The backend's configured request, file-size, file-count, and execution-time limits still apply. A Render plan must provide enough memory and temporary disk space for the expected archives. The application currently has no user accounts or persistent file storage; set access and resource policies appropriate to the intended audience before public use.
