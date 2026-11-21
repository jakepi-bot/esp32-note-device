# The Bluetooth note-sync server: the contract endpoint plus the phone's page.
#
# Build from the repository root (the compose file does this for you):
#   docker build -f deploy/bluetooth-sync/Dockerfile -t hermes-note-sync .
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    NOTES_DIR=/data/notes \
    VAULT_DIR=/data/vault \
    DEVICES_DIR=/data/gadget

WORKDIR /app

# The plugin directory doubles as the importable package (`hermes_gadget_plugin`),
# so the container runs exactly the code the gateway loads.
COPY pyproject.toml README.md NOTICE LICENSE THIRD_PARTY_NOTICES.md ./
COPY LICENSES ./LICENSES
COPY python ./python
COPY plugin ./plugin
COPY web ./web
COPY deploy/bluetooth-sync/serve.py ./serve.py

# FastAPI and uvicorn are the endpoint's only needs beyond the standard library;
# the OpenAI-compatible speech and chat calls use urllib.
RUN pip install --no-cache-dir --upgrade pip \
 && pip install --no-cache-dir "fastapi>=0.110,<1" "uvicorn>=0.29,<1" \
 && pip install --no-cache-dir --no-deps .

# Run as an ordinary user: /data is a bind mount, so make it match a normal host
# uid (1000) and the host's files stay readable and writable without sudo.
RUN useradd --uid 1000 --create-home --shell /usr/sbin/nologin app \
 && mkdir -p /data/notes /data/vault /data/gadget \
 && chown -R app:app /data /app
USER app

EXPOSE 8766

HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
  CMD python -c "import sys,urllib.request; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:8766/health', timeout=4).status == 200 else 1)"

ENTRYPOINT ["python", "/app/serve.py"]
