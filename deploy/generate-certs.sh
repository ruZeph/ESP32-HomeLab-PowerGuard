#!/usr/bin/env bash
#
# Generate the CA and server certificate that put the PowerGuard shutdown API
# behind TLS.
#
# Run this on the host that serves the shutdown API (192.168.0.89 by default).
# It writes:
#
#   out/powerguard-ca.crt      the CA to pin on the ESP32
#   out/powerguard-server.crt  the leaf certificate for the reverse proxy
#   out/powerguard-server.key  the leaf private key, keep this secret
#   out/powerguard-ca.key      the CA private key, keep this secret
#
# Nothing in out/ should be committed except powerguard-ca.crt, which is public.

set -euo pipefail

HOST_IP="${1:-192.168.0.89}"
DAYS_LEAF="${2:-825}"
OUT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/out"

mkdir -p "$OUT_DIR"

if [[ -f "$OUT_DIR/powerguard-ca.crt" ]]; then
    echo "A CA already exists in $OUT_DIR. Remove out/ first if you really want to replace it."
    exit 1
fi

echo "Generating CA ..."
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
    -keyout "$OUT_DIR/powerguard-ca.key" \
    -out "$OUT_DIR/powerguard-ca.crt" \
    -subj "/CN=PowerGuard Shutdown CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"

echo "Generating server certificate for $HOST_IP ..."
openssl req -newkey rsa:2048 -sha256 -nodes \
    -keyout "$OUT_DIR/powerguard-server.key" \
    -out "$OUT_DIR/powerguard-server.csr" \
    -subj "/CN=$HOST_IP" \
    -addext "subjectAltName=IP:$HOST_IP" \
    -addext "extendedKeyUsage=serverAuth"

openssl x509 -req \
    -in "$OUT_DIR/powerguard-server.csr" \
    -CA "$OUT_DIR/powerguard-ca.crt" \
    -CAkey "$OUT_DIR/powerguard-ca.key" \
    -CAcreateserial \
    -out "$OUT_DIR/powerguard-server.crt" \
    -days "$DAYS_LEAF" \
    -sha256 \
    -copy_extensions copy

rm -f "$OUT_DIR/powerguard-server.csr"

echo
echo "Done. Next steps:"
echo "  1. Copy out/powerguard-ca.crt to main/certs/poweroff_ca.pem in the firmware repo."
echo "  2. Point the reverse proxy at out/powerguard-server.{crt,key}."
echo "  3. After the proxy is verified, change shutdown_url to https://$HOST_IP/ in config.c."
echo
echo "Keep powerguard-ca.key and powerguard-server.key off the network and out of git."
