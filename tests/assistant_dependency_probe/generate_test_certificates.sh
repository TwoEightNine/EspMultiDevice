#!/usr/bin/env bash
set -euo pipefail

probe_dir="$(cd "$(dirname "$0")" && pwd)"
output_dir="$probe_dir/generated"
server_name="${1:-esp-media-probe.local}"
mkdir -p "$output_dir"

if [[ "$server_name" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  san="IP:$server_name"
else
  san="DNS:$server_name"
fi

openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 30 \
  -subj '/CN=ESP Media Stage 0 Test CA' \
  -keyout "$output_dir/ca.key" -out "$output_dir/ca.crt"
openssl req -newkey rsa:2048 -sha256 -nodes \
  -subj "/CN=$server_name" -addext "subjectAltName=$san" \
  -keyout "$output_dir/server.key" -out "$output_dir/server.csr"
openssl x509 -req -sha256 -days 30 \
  -in "$output_dir/server.csr" \
  -CA "$output_dir/ca.crt" -CAkey "$output_dir/ca.key" -CAcreateserial \
  -copy_extensions copy -out "$output_dir/server.crt"
openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 30 \
  -subj '/CN=ESP Media Stage 0 Wrong CA' \
  -keyout "$output_dir/wrong-ca.key" -out "$output_dir/wrong-ca.crt"

echo "generated test-only certificates in $output_dir"
echo "copy ProbeConfig.example.h to ProbeConfig.h and paste ca.crt and wrong-ca.crt into it"
