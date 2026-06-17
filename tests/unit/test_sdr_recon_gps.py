#!/usr/bin/env python3
# ========================================================================
# Project: OpenRFStack
# Author:  Brendan Michaud
# Year:    2026
# Part of OpenRFStack (https://github.com/OpenRFStack)
#
# Licensed under the Personal Use License.
# Do not use for commercial, organizational, or military purposes.
# ========================================================================
"""
test_sdr_recon_gps.py — Unit tests for sdr_recon.py GPS caching logic.

Tests _on_gps(), get_gps(), and the GPS stamping in _capture() metadata.
No AMQP broker required — uses object.__new__ to isolate the GPS methods.
"""
import json
import sys
import threading
import types
import unittest
from pathlib import Path
from unittest.mock import MagicMock

# Stub proton so sdr_recon can be imported outside the container.
_proton       = types.ModuleType("proton")
_handlers     = types.ModuleType("proton.handlers")
_reactor      = types.ModuleType("proton.reactor")

class _MessagingHandler:
    def on_start(self, event): pass
    def on_connection_opened(self, event): pass
    def on_message(self, event): pass
    def on_transport_error(self, event): pass
    def on_connection_error(self, event): pass

_handlers.MessagingHandler = _MessagingHandler
_reactor.Container         = MagicMock
_proton.handlers           = _handlers
_proton.reactor            = _reactor

for _name, _mod in [("proton", _proton),
                    ("proton.handlers", _handlers),
                    ("proton.reactor", _reactor)]:
    sys.modules[_name] = _mod

# Make sdr_recon importable from src/
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "src"))
import sdr_recon


def _make_sess() -> sdr_recon.ReconSession:
    """Return a ReconSession with only GPS attributes initialised (no AMQP)."""
    sess = object.__new__(sdr_recon.ReconSession)
    sess._gps_fix  = None
    sess._gps_lock = threading.Lock()
    return sess


class TestGpsCacheNone(unittest.TestCase):
    def test_get_gps_returns_none_before_first_fix(self):
        sess = _make_sess()
        self.assertIsNone(sess.get_gps())


class TestGpsCacheOnGps(unittest.TestCase):
    def setUp(self):
        self.sess = _make_sess()
        self.fix = {
            "latitude_deg":  45.5231,
            "longitude_deg": -122.6765,
            "altitude_m":    52.3,
        }

    def test_on_gps_stores_fix(self):
        self.sess._on_gps(self.fix)
        self.assertIsNotNone(self.sess.get_gps())

    def test_get_gps_returns_correct_lat(self):
        self.sess._on_gps(self.fix)
        self.assertAlmostEqual(self.sess.get_gps()["latitude_deg"], 45.5231)

    def test_get_gps_returns_correct_lon(self):
        self.sess._on_gps(self.fix)
        self.assertAlmostEqual(self.sess.get_gps()["longitude_deg"], -122.6765)

    def test_get_gps_returns_correct_alt(self):
        self.sess._on_gps(self.fix)
        self.assertAlmostEqual(self.sess.get_gps()["altitude_m"], 52.3)

    def test_second_fix_overwrites_first(self):
        first = {"latitude_deg": 10.0, "longitude_deg": 20.0, "altitude_m": 0.0}
        second = {"latitude_deg": 51.5074, "longitude_deg": -0.1278, "altitude_m": 11.0}
        self.sess._on_gps(first)
        self.sess._on_gps(second)
        result = self.sess.get_gps()
        self.assertAlmostEqual(result["latitude_deg"],  51.5074)
        self.assertAlmostEqual(result["longitude_deg"], -0.1278)
        self.assertAlmostEqual(result["altitude_m"],    11.0)

    def test_negative_altitude_stored_correctly(self):
        fix = {"latitude_deg": 0.0, "longitude_deg": 0.0, "altitude_m": -5.5}
        self.sess._on_gps(fix)
        self.assertAlmostEqual(self.sess.get_gps()["altitude_m"], -5.5)

    def test_zero_coordinates_stored_correctly(self):
        fix = {"latitude_deg": 0.0, "longitude_deg": 0.0, "altitude_m": 0.0}
        self.sess._on_gps(fix)
        result = self.sess.get_gps()
        self.assertAlmostEqual(result["latitude_deg"],  0.0)
        self.assertAlmostEqual(result["longitude_deg"], 0.0)
        self.assertAlmostEqual(result["altitude_m"],    0.0)

    def test_high_precision_coords(self):
        lat = 37.123456789012345
        lon = -122.987654321098765
        self.sess._on_gps({"latitude_deg": lat, "longitude_deg": lon, "altitude_m": 0.0})
        result = self.sess.get_gps()
        self.assertAlmostEqual(result["latitude_deg"],  lat, places=9)
        self.assertAlmostEqual(result["longitude_deg"], lon, places=9)


class TestGpsThreadSafety(unittest.TestCase):
    """Verify _on_gps / get_gps don't race under concurrent access."""

    def test_concurrent_writes_do_not_corrupt(self):
        sess = _make_sess()
        errors = []

        def writer(i):
            try:
                for _ in range(500):
                    sess._on_gps({"latitude_deg": float(i), "longitude_deg": float(-i),
                                  "altitude_m": float(i * 10)})
            except Exception as e:
                errors.append(e)

        def reader():
            try:
                for _ in range(500):
                    sess.get_gps()  # must not raise
            except Exception as e:
                errors.append(e)

        threads = [threading.Thread(target=writer, args=(i,)) for i in range(4)]
        threads += [threading.Thread(target=reader) for _ in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertFalse(errors, f"Thread errors: {errors}")


class TestGpsMetaBuilding(unittest.TestCase):
    """Test the gps_meta dict that _capture() builds for metadata files."""

    @staticmethod
    def _build_gps_meta(fix: dict | None) -> dict:
        """Mirror the logic in sdr_recon._capture() for gps_meta construction."""
        if fix:
            return {
                "lat":   fix.get("latitude_deg"),
                "lon":   fix.get("longitude_deg"),
                "alt_m": fix.get("altitude_m"),
            }
        return {}

    def test_gps_meta_empty_when_no_fix(self):
        meta = self._build_gps_meta(None)
        self.assertEqual(meta, {})

    def test_gps_meta_has_all_keys_when_fix_present(self):
        fix = {"latitude_deg": 45.0, "longitude_deg": -122.0, "altitude_m": 100.0}
        meta = self._build_gps_meta(fix)
        self.assertIn("lat",   meta)
        self.assertIn("lon",   meta)
        self.assertIn("alt_m", meta)

    def test_gps_meta_values_match_fix(self):
        fix = {"latitude_deg": 51.5074, "longitude_deg": -0.1278, "altitude_m": 11.0}
        meta = self._build_gps_meta(fix)
        self.assertAlmostEqual(meta["lat"],   51.5074)
        self.assertAlmostEqual(meta["lon"],   -0.1278)
        self.assertAlmostEqual(meta["alt_m"], 11.0)

    def test_gps_meta_is_json_serialisable(self):
        fix = {"latitude_deg": 45.0, "longitude_deg": -122.0, "altitude_m": 100.0}
        meta = self._build_gps_meta(fix)
        dumped = json.dumps(meta)
        self.assertIn("lat", dumped)


if __name__ == "__main__":
    unittest.main()
