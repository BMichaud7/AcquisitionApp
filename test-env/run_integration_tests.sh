#!/usr/bin/env bash
# Run AcquisitionApp integration tests.
# Starts broker, runs AcquisitionApp natively (no container needed for scanner),
# then runs e2e_test.py as the fake controller.
set -euo pipefail
cd "$(dirname "$0")"

BROKER_PORT=5673
BROKER_URL="amqp://localhost:${BROKER_PORT}"
TEST="${1:-}"

echo "=== Building sdr-acquisition:dev ==="
podman build --target runtime -t sdr-acquisition:dev ..

echo "=== Starting broker ==="
podman run -d --rm --name acq-broker \
    -e ARTEMIS_USER=sdr_ctrl \
    -e ARTEMIS_PASSWORD=test_password \
    -p "${BROKER_PORT}:5672" \
    apache/activemq-artemis:2.36.0

echo -n "Waiting for broker..."
for i in $(seq 1 30); do
    podman exec acq-broker \
        /var/lib/artemis-instance/bin/artemis check node --up \
        >/dev/null 2>&1 && break
    echo -n "."
    sleep 2
done
echo " ready"

echo "=== Starting AcquisitionApp ==="
podman run -d --rm --name sdr-acquisition \
    --network=host \
    -v "$(pwd)/config/scanner.xml:/etc/sdr-acquisition/scanner.xml:ro" \
    -e SDR_LOG_LEVEL=debug \
    -e SDR_ACQ_CONFIG=/etc/sdr-acquisition/scanner.xml \
    localhost/sdr-acquisition:dev
sleep 3

echo "=== Running integration tests ==="
pip install python-qpid-proton numpy --quiet

if [ -n "$TEST" ]; then
    python3 e2e_test.py --broker "$BROKER_URL" --test "$TEST"
else
    python3 e2e_test.py --broker "$BROKER_URL"
fi
RC=$?

echo "=== Tearing down ==="
podman stop sdr-acquisition acq-broker 2>/dev/null || true

exit $RC
