#!/bin/sh
set -e

echo "=== Building test image ==="
docker build -f Dockerfile.test -t tcbridge-tests .

echo ""
echo "=== Running tests ==="
docker run --rm tcbridge-tests
