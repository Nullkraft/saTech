"""Rigol DS1102E adapter for the saTech full-test runner."""

from __future__ import annotations

import os
from pathlib import Path
import sys
import time


RIGOL_PROJECT = Path("~/projects/Rigol-DS1102E-Oscilloscope").expanduser()
if str(RIGOL_PROJECT) not in sys.path:
    sys.path.insert(0, str(RIGOL_PROJECT))

from rigol_ds1102e_spi_analysis import (  # noqa: E402
    decode_spi_data_words,
    decode_spi_data_words_windowed,
    normalize_waveform_samples,
    detect_rising_edge_sample_indexes,
)


class RigolFullTestAdapter:
    def __init__(
        self,
        device="/dev/usbtmc0",
        clock_channel=1,
        data_channel=2,
        delay=0.2,
        read_size=1200000,
        threshold=5,
        slope_threshold=5,
        low_ratio=0.2,
        high_ratio=0.8,
        max_extra_edges=16,
        clock_vertical_scale=2.0,
        trigger_level=1.28,
        timebase_scale="5.0us",
    ):
        self.device = device
        self.clock_channel = clock_channel
        self.data_channel = data_channel
        self.delay = delay
        self.read_size = read_size
        self.threshold = threshold
        self.slope_threshold = slope_threshold
        self.low_ratio = low_ratio
        self.high_ratio = high_ratio
        self.max_extra_edges = max_extra_edges
        self.clock_vertical_scale = clock_vertical_scale
        self.trigger_level = trigger_level
        self.timebase_scale = timebase_scale

    def scope_setup(self):
        self._write(":STOP")
        self._write(f":CHAN{self.clock_channel}:DISP ON")
        self._write(f":CHAN{self.data_channel}:DISP ON")
        self._write(f":CHAN{self.clock_channel}:SCALe {self.clock_vertical_scale:.1f}")
        self._write(":TRIGger:MODE EDGE")
        self._write(f":TRIGger:EDGE:SOURce CHAN{self.clock_channel}")
        self._write(f":TRIGger:EDGE:LEVel {self.trigger_level:.2f}")
        self._write(":TRIGger:EDGE:SWEep SING")
        self._write(":WAVeform:POINts:MODE RAW")
        self._write(f":TIMebase:SCALe {self.timebase_scale}")

    def start_new_waveform(self):
        self._write(":RUN")

    def capture_waveform(self):
        self._require_stopped_scope()
        return {
            "channels": {
                str(self.clock_channel): self._query_waveform_payload(self.clock_channel),
                str(self.data_channel): self._query_waveform_payload(self.data_channel),
            }
        }

    def spi_decode(self, capture, expected_writes=None, expected_addresses=None):
        clock_samples = normalize_waveform_samples(capture["channels"][str(self.clock_channel)])
        data_samples = normalize_waveform_samples(capture["channels"][str(self.data_channel)])
        sample_indexes = detect_rising_edge_sample_indexes(
            clock_samples,
            threshold=self.threshold,
            slope_threshold=self.slope_threshold,
        )
        if expected_addresses is not None and expected_writes is None:
            expected_writes = len(expected_addresses)
        if expected_writes is not None:
            decoded = decode_spi_data_words_windowed(
                data_samples,
                sample_indexes,
                expected_writes=expected_writes,
                max_extra_edges=self.max_extra_edges,
                low_ratio=self.low_ratio,
                high_ratio=self.high_ratio,
                expected_addresses=expected_addresses,
            )
        else:
            decoded = decode_spi_data_words(
                data_samples,
                sample_indexes,
                low_ratio=self.low_ratio,
                high_ratio=self.high_ratio,
            )
        return {
            "decoded_words": decoded["decoded_words"],
            "address_map": decoded["address_map"],
            "sample_indexes": sample_indexes,
            "selected_sample_indexes": decoded.get("selected_sample_indexes"),
            "window": decoded.get("window"),
        }

    def _require_stopped_scope(self, attempts=20):
        status = ""
        for _ in range(attempts):
            status = self._query_text(":TRIGger:STATus?").upper()
            if status == "STOP":
                return
        raise RuntimeError(f"scope did not trigger and stop; last status was {status!r}")

    def _write(self, scpi):
        fd = os.open(self.device, os.O_RDWR)
        try:
            os.write(fd, self._payload(scpi))
            # The scope can ignore back-to-back setup writes without pacing.
            time.sleep(self.delay)
        finally:
            os.close(fd)

    def _query_text(self, scpi, read_size=4096):
        return self._query_bytes(scpi, read_size).decode("ascii", "replace").replace("\x00", "").strip()

    def _query_waveform_payload(self, channel):
        response = self._query_waveform_bytes(f":WAVeform:DATA? CHAN{channel}")
        for _ in range(4):
            if len(response) != 600 or self.read_size <= 600:
                break
            reread = self._query_waveform_bytes(f":WAVeform:DATA? CHAN{channel}")
            if len(reread) > len(response):
                response = reread
            elif len(reread) != 600:
                response = reread
                break
        return self._waveform_payload(response)

    def _query_waveform_bytes(self, scpi):
        return self._query_bytes(scpi, self.read_size).rstrip(b"\n")

    def _query_bytes(self, scpi, read_size):
        fd = os.open(self.device, os.O_RDWR)
        try:
            os.write(fd, self._payload(scpi))
            time.sleep(self.delay)
            return os.read(fd, read_size)
        finally:
            os.close(fd)

    @staticmethod
    def _payload(scpi):
        return f"{scpi.rstrip()}\n".encode("ascii")

    @staticmethod
    def _waveform_payload(response):
        if len(response) < 2 or response[0:1] != b"#":
            return response
        digit_count = response[1] - ord("0")
        if not 0 <= digit_count <= 9:
            return response
        if digit_count == 0:
            return response[2:].rstrip(b"\r\n")
        header_end = 2 + digit_count
        if len(response) < header_end:
            return response
        payload_len = int(response[2:header_end].decode("ascii"))
        payload_end = header_end + payload_len
        if len(response) < payload_end:
            return response
        return response[header_end:payload_end]
