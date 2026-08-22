#!/usr/bin/env python3
"""Compare time-aligned legacy and modern renderer frame sequences.

The tool intentionally uses only Python's standard library.  It accepts
non-interlaced 8-bit PNG, PPM/PGM, and uncompressed 24/32-bit BMP frames and
emits a compact JSON report.  Alpha is ignored because the comparison is
against the visible RGB result.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


SUPPORTED_EXTENSIONS = {".bmp", ".pgm", ".png", ".ppm"}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


@dataclass(frozen=True)
class Frame:
    name: str
    width: int
    height: int
    pixels: bytes


@dataclass
class AnalysisOptions:
    low_motion_threshold: float = 0.02
    flicker_threshold: float = 0.02
    pixel_error_threshold: float = 0.02
    cadence_tolerance: float = 0.10
    legacy_timestamps: Optional[Path] = None
    candidate_timestamps: Optional[Path] = None
    diagnostic_dir: Optional[Path] = None
    diagnostic_limit: int = 0


def _natural_sort_key(path: Path) -> Tuple[object, ...]:
    return tuple(
        int(part) if part.isdigit() else part.lower()
        for part in re.split(r"(\d+)", path.name)
    )


def discover_frames(directory: Path) -> List[Path]:
    directory = Path(directory)
    if not directory.is_dir():
        raise ValueError("frame directory does not exist: {}".format(directory))
    paths = sorted(
        (path for path in directory.iterdir()
         if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS),
        key=_natural_sort_key,
    )
    if not paths:
        raise ValueError("no supported frame images found in {}".format(directory))
    return paths


def _read_token(data: bytes, offset: int) -> Tuple[bytes, int]:
    length = len(data)
    while offset < length:
        if data[offset] in b" \t\r\n\v\f":
            offset += 1
            continue
        if data[offset] == ord("#"):
            newline = data.find(b"\n", offset)
            offset = length if newline < 0 else newline + 1
            continue
        break
    start = offset
    while offset < length and data[offset] not in b" \t\r\n\v\f":
        offset += 1
    if start == offset:
        raise ValueError("unexpected end of image header")
    return data[start:offset], offset


def _binary_payload_offset(data: bytes, offset: int) -> int:
    if offset >= len(data) or data[offset] not in b" \t\r\n\v\f":
        raise ValueError("binary image header is missing its separator")
    if data[offset] == ord("\r") and offset + 1 < len(data) and data[offset + 1] == ord("\n"):
        return offset + 2
    return offset + 1


def _load_pnm(path: Path, data: bytes) -> Frame:
    magic, offset = _read_token(data, 0)
    if magic not in (b"P5", b"P6"):
        raise ValueError("unsupported PNM format in {}".format(path))
    width_token, offset = _read_token(data, offset)
    height_token, offset = _read_token(data, offset)
    max_value_token, offset = _read_token(data, offset)
    width = int(width_token)
    height = int(height_token)
    max_value = int(max_value_token)
    if width <= 0 or height <= 0 or max_value != 255:
        raise ValueError("{} requires positive dimensions and max value 255".format(path))
    payload = data[_binary_payload_offset(data, offset):]
    pixel_count = width * height
    if magic == b"P6":
        expected = pixel_count * 3
        if len(payload) < expected:
            raise ValueError("truncated PPM frame: {}".format(path))
        pixels = bytes(payload[:expected])
    else:
        expected = pixel_count
        if len(payload) < expected:
            raise ValueError("truncated PGM frame: {}".format(path))
        pixels = bytes(channel for value in payload[:expected] for channel in (value, value, value))
    return Frame(path.name, width, height, pixels)


def _paeth(a: int, b: int, c: int) -> int:
    estimate = a + b - c
    distance_a = abs(estimate - a)
    distance_b = abs(estimate - b)
    distance_c = abs(estimate - c)
    if distance_a <= distance_b and distance_a <= distance_c:
        return a
    if distance_b <= distance_c:
        return b
    return c


def _unfilter_png_rows(raw: bytes, width: int, height: int, bytes_per_pixel: int) -> List[bytes]:
    row_size = width * bytes_per_pixel
    expected = height * (row_size + 1)
    if len(raw) != expected:
        raise ValueError("PNG scanline payload has an unexpected size")
    rows: List[bytes] = []
    offset = 0
    previous = bytearray(row_size)
    for _ in range(height):
        filter_type = raw[offset]
        offset += 1
        encoded = raw[offset:offset + row_size]
        offset += row_size
        row = bytearray(encoded)
        if filter_type == 1:
            for index in range(row_size):
                left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                row[index] = (row[index] + left) & 0xFF
        elif filter_type == 2:
            for index in range(row_size):
                row[index] = (row[index] + previous[index]) & 0xFF
        elif filter_type == 3:
            for index in range(row_size):
                left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                row[index] = (row[index] + ((left + previous[index]) // 2)) & 0xFF
        elif filter_type == 4:
            for index in range(row_size):
                left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                above = previous[index]
                upper_left = previous[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
                row[index] = (row[index] + _paeth(left, above, upper_left)) & 0xFF
        elif filter_type != 0:
            raise ValueError("unsupported PNG filter type {}".format(filter_type))
        rows.append(bytes(row))
        previous = row
    return rows


def _load_png(path: Path, data: bytes) -> Frame:
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("invalid PNG signature: {}".format(path))
    offset = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = interlace = None
    idat = bytearray()
    palette: Optional[bytes] = None
    while offset + 12 <= len(data):
        chunk_length = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        chunk_type = data[offset:offset + 4]
        offset += 4
        end = offset + chunk_length
        if end + 4 > len(data):
            raise ValueError("truncated PNG chunk in {}".format(path))
        chunk = data[offset:end]
        offset = end + 4
        if chunk_type == b"IHDR":
            if len(chunk) != 13:
                raise ValueError("invalid PNG IHDR in {}".format(path))
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
            if compression != 0 or filtering != 0:
                raise ValueError("unsupported PNG compression/filter method in {}".format(path))
        elif chunk_type == b"PLTE":
            palette = bytes(chunk)
        elif chunk_type == b"IDAT":
            idat.extend(chunk)
        elif chunk_type == b"IEND":
            break
    if width is None or height is None or bit_depth is None or color_type is None:
        raise ValueError("PNG is missing IHDR: {}".format(path))
    if bit_depth != 8 or interlace != 0:
        raise ValueError("{} only supports non-interlaced 8-bit PNG frames".format(path))
    channels_by_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    channels = channels_by_type.get(color_type)
    if channels is None:
        raise ValueError("unsupported PNG color type {} in {}".format(color_type, path))
    if color_type == 3 and (palette is None or len(palette) % 3 != 0):
        raise ValueError("indexed PNG is missing a valid palette: {}".format(path))
    rows = _unfilter_png_rows(zlib.decompress(bytes(idat)), width, height, channels)
    pixels = bytearray()
    for row in rows:
        if color_type == 0:
            pixels.extend(value for value in row for _ in range(3))
        elif color_type == 2:
            pixels.extend(row)
        elif color_type == 3:
            assert palette is not None
            for index in row:
                palette_offset = index * 3
                if palette_offset + 3 > len(palette):
                    raise ValueError("PNG palette index is out of range in {}".format(path))
                pixels.extend(palette[palette_offset:palette_offset + 3])
        elif color_type == 4:
            pixels.extend(value for index in range(0, len(row), 2) for value in (row[index],) * 3)
        else:
            for index in range(0, len(row), 4):
                pixels.extend(row[index:index + 3])
    return Frame(path.name, width, height, bytes(pixels))


def _load_bmp(path: Path, data: bytes) -> Frame:
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("invalid BMP header: {}".format(path))
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40 or len(data) < 14 + dib_size:
        raise ValueError("unsupported BMP DIB header: {}".format(path))
    width, signed_height, planes, bits_per_pixel, compression = struct.unpack_from(
        "<iiHHI", data, 18
    )
    if width <= 0 or signed_height == 0 or planes != 1 or compression != 0:
        raise ValueError("{} requires an uncompressed positive-width BMP".format(path))
    if bits_per_pixel not in (24, 32):
        raise ValueError("{} only supports 24/32-bit BMP frames".format(path))
    height = abs(signed_height)
    row_stride = ((width * bits_per_pixel + 31) // 32) * 4
    expected_end = pixel_offset + row_stride * height
    if pixel_offset > len(data) or expected_end > len(data):
        raise ValueError("truncated BMP frame: {}".format(path))
    pixels = bytearray(width * height * 3)
    for output_y in range(height):
        source_y = output_y if signed_height < 0 else height - 1 - output_y
        row_offset = pixel_offset + source_y * row_stride
        output_offset = output_y * width * 3
        for x in range(width):
            source_offset = row_offset + x * (bits_per_pixel // 8)
            blue, green, red = data[source_offset:source_offset + 3]
            destination = output_offset + x * 3
            pixels[destination:destination + 3] = bytes((red, green, blue))
    return Frame(path.name, width, height, bytes(pixels))


def load_frame(path: Path) -> Frame:
    path = Path(path)
    data = path.read_bytes()
    suffix = path.suffix.lower()
    if suffix in (".ppm", ".pgm"):
        return _load_pnm(path, data)
    if suffix == ".png":
        return _load_png(path, data)
    if suffix == ".bmp":
        return _load_bmp(path, data)
    raise ValueError("unsupported frame extension: {}".format(path.suffix))


def _percentile(values: Sequence[float], quantile: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def _histogram_percentile(histogram: Sequence[int], count: int, quantile: float) -> Optional[float]:
    if count == 0:
        return None
    position = (count - 1) * quantile
    lower_rank = int(math.floor(position))
    upper_rank = int(math.ceil(position))
    lower_value = upper_value = 0
    found_lower = False
    seen = 0
    for value, frequency in enumerate(histogram):
        next_seen = seen + frequency
        if not found_lower and lower_rank < next_seen:
            lower_value = value
            found_lower = True
        if upper_rank < next_seen:
            upper_value = value
            break
        seen = next_seen
    if lower_rank == upper_rank:
        return lower_value / 255.0
    return (lower_value + (upper_value - lower_value) * (position - lower_rank)) / 255.0


def _summarize(values: Sequence[float]) -> dict:
    if not values:
        return {
            "count": 0,
            "mean": None,
            "p50": None,
            "p90": None,
            "p95": None,
            "p99": None,
            "max": None,
        }
    return {
        "count": len(values),
        "mean": sum(values) / len(values),
        "p50": _percentile(values, 0.50),
        "p90": _percentile(values, 0.90),
        "p95": _percentile(values, 0.95),
        "p99": _percentile(values, 0.99),
        "max": max(values),
    }


def _image_metrics(reference: Frame, candidate: Frame, threshold: float) -> dict:
    if (reference.width, reference.height) != (candidate.width, candidate.height):
        raise ValueError("frame dimensions do not match: {} and {}".format(reference.name, candidate.name))
    pixel_count = reference.width * reference.height
    reference_pixels = reference.pixels
    candidate_pixels = candidate.pixels
    channel_sum = 0
    channel_square_sum = 0
    changed_pixels = 0
    maximum = 0
    histogram = [0] * 256
    for offset in range(0, pixel_count * 3, 3):
        red = abs(reference_pixels[offset] - candidate_pixels[offset])
        green = abs(reference_pixels[offset + 1] - candidate_pixels[offset + 1])
        blue = abs(reference_pixels[offset + 2] - candidate_pixels[offset + 2])
        channel_sum += red + green + blue
        channel_square_sum += red * red + green * green + blue * blue
        pixel_error = max(red, green, blue)
        maximum = max(maximum, pixel_error)
        histogram[pixel_error] += 1
        if pixel_error / 255.0 > threshold:
            changed_pixels += 1
    channel_count = pixel_count * 3
    return {
        "mae": channel_sum / (channel_count * 255.0),
        "rmse": math.sqrt(channel_square_sum / (channel_count * 255.0 * 255.0)),
        "max": maximum / 255.0,
        "p95": _histogram_percentile(histogram, pixel_count, 0.95),
        "changed_pixel_ratio": changed_pixels / pixel_count,
    }


def _temporal_metrics(
    previous_reference: Frame,
    current_reference: Frame,
    previous_candidate: Frame,
    current_candidate: Frame,
    low_motion_threshold: float,
    flicker_threshold: float,
) -> dict:
    pixel_count = current_reference.width * current_reference.height
    reference_pixels = current_reference.pixels
    previous_reference_pixels = previous_reference.pixels
    candidate_pixels = current_candidate.pixels
    previous_candidate_pixels = previous_candidate.pixels
    static_count = 0
    flicker_count = 0
    reference_sum = 0
    candidate_sum = 0
    residual_sum = 0
    reference_max = candidate_max = residual_max = 0
    reference_histogram = [0] * 256
    candidate_histogram = [0] * 256
    residual_histogram = [0] * 256
    for offset in range(0, pixel_count * 3, 3):
        reference_delta = max(
            abs(reference_pixels[offset] - previous_reference_pixels[offset]),
            abs(reference_pixels[offset + 1] - previous_reference_pixels[offset + 1]),
            abs(reference_pixels[offset + 2] - previous_reference_pixels[offset + 2]),
        )
        candidate_delta = max(
            abs(candidate_pixels[offset] - previous_candidate_pixels[offset]),
            abs(candidate_pixels[offset + 1] - previous_candidate_pixels[offset + 1]),
            abs(candidate_pixels[offset + 2] - previous_candidate_pixels[offset + 2]),
        )
        if reference_delta / 255.0 <= low_motion_threshold:
            static_count += 1
            residual = max(0, candidate_delta - reference_delta)
            reference_sum += reference_delta
            candidate_sum += candidate_delta
            residual_sum += residual
            reference_max = max(reference_max, reference_delta)
            candidate_max = max(candidate_max, candidate_delta)
            residual_max = max(residual_max, residual)
            reference_histogram[reference_delta] += 1
            candidate_histogram[candidate_delta] += 1
            residual_histogram[residual] += 1
            if residual / 255.0 > flicker_threshold:
                flicker_count += 1
    def distribution(total: int, histogram: Sequence[int], maximum: int) -> dict:
        if static_count == 0:
            return {"mean": None, "p95": None, "max": None}
        return {
            "mean": total / (static_count * 255.0),
            "p95": _histogram_percentile(histogram, static_count, 0.95),
            "max": maximum / 255.0,
        }
    return {
        "low_motion_pixel_ratio": static_count / pixel_count,
        "low_motion_pixel_count": static_count,
        "reference_change": distribution(reference_sum, reference_histogram, reference_max),
        "candidate_change": distribution(candidate_sum, candidate_histogram, candidate_max),
        "flicker_residual": distribution(residual_sum, residual_histogram, residual_max),
        "flicker_pixel_ratio": flicker_count / static_count if static_count else 0.0,
        "flicker_pixel_count": flicker_count,
    }


def _frame_hash(frame: Frame) -> str:
    return hashlib.sha256(frame.pixels).hexdigest()


def _duplicate_report(
    hashes: Sequence[str],
    timestamps: Optional[Sequence[float]] = None,
    nominal_interval: Optional[float] = None,
) -> dict:
    runs = []
    start = 0
    for index in range(1, len(hashes) + 1):
        if index < len(hashes) and hashes[index] == hashes[start]:
            continue
        count = index - start
        if count > 1:
            run = {
                "start_frame": start,
                "end_frame": index - 1,
                "count": count,
                "duplicate_frame_count": count - 1,
                "held_frame_count": count - 1,
                "sha256": hashes[start],
            }
            if timestamps is not None:
                timestamp_span = timestamps[index - 1] - timestamps[start]
                run["timestamp_span_seconds"] = timestamp_span
                if nominal_interval is not None and timestamp_span >= 0.0:
                    run["held_duration_seconds"] = timestamp_span + nominal_interval
                    run["held_duration_is_estimated"] = True
                else:
                    run["held_duration_seconds"] = None
                    run["held_duration_is_estimated"] = False
            else:
                run["timestamp_span_seconds"] = None
                run["held_duration_seconds"] = None
                run["held_duration_is_estimated"] = False
            runs.append(run)
        start = index
    held_frame_count = sum(run["held_frame_count"] for run in runs)
    held_duration_values = [
        run["held_duration_seconds"]
        for run in runs
        if run["held_duration_seconds"] is not None
    ]
    frame_count = len(hashes)
    return {
        "duplicate_frame_count": held_frame_count,
        "duplicate_frame_ratio": held_frame_count / frame_count if frame_count else 0.0,
        "held_frame_count": held_frame_count,
        "held_frame_ratio": held_frame_count / frame_count if frame_count else 0.0,
        "held_run_count": len(runs),
        "held_duration_seconds": sum(held_duration_values) if held_duration_values else None,
        "max_held_duration_seconds": max(held_duration_values) if held_duration_values else None,
        "runs": runs,
    }


def _parse_timestamps(path: Path, expected_count: int) -> List[float]:
    path = Path(path)
    if not path.is_file():
        raise ValueError("timestamp file does not exist: {}".format(path))
    text = path.read_text(encoding="utf-8-sig")
    if path.suffix.lower() == ".json":
        value = json.loads(text)
        if isinstance(value, dict):
            value = value.get("timestamps")
        if not isinstance(value, list):
            raise ValueError("timestamp JSON must contain a list or timestamps field")
        try:
            timestamps = [float(item) for item in value]
        except (TypeError, ValueError):
            raise ValueError("timestamp JSON contains a non-numeric value")
        if any(not math.isfinite(timestamp) for timestamp in timestamps):
            raise ValueError("timestamp JSON contains a non-finite value")
    else:
        timestamps = []
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [field for field in re.split(r"[,;\t ]+", line) if field]
            try:
                timestamp = float(fields[-1])
            except (IndexError, ValueError):
                if not timestamps and fields:
                    continue
                raise ValueError("invalid timestamp line in {}: {}".format(path, line))
            if not math.isfinite(timestamp):
                raise ValueError("timestamp must be finite in {}: {}".format(path, line))
            timestamps.append(timestamp)
    if len(timestamps) != expected_count:
        raise ValueError(
            "timestamp count {} does not match frame count {} for {}".format(
                len(timestamps), expected_count, path
            )
        )
    return timestamps


def _timestamp_report(
    timestamps: Optional[Sequence[float]],
    tolerance: float,
    expected_timestamps: Optional[Sequence[float]] = None,
) -> dict:
    if timestamps is None:
        return {"provided": False}
    intervals = [timestamps[index] - timestamps[index - 1] for index in range(1, len(timestamps))]
    positive_intervals = [interval for interval in intervals if interval > 0.0]
    median_interval = _percentile(positive_intervals, 0.50)
    expected_intervals = []
    if expected_timestamps is not None:
        expected_intervals = [
            expected_timestamps[index] - expected_timestamps[index - 1]
            for index in range(1, len(expected_timestamps))
        ]
    expected_positive = [interval for interval in expected_intervals if interval > 0.0]
    expected_median = _percentile(expected_positive, 0.50)
    nominal_interval = expected_median if expected_median is not None else median_interval
    expected_source = "reference" if expected_median is not None else "self"
    irregular_indices = []
    estimated_dropped_frames = 0
    relative_jitter = []
    for index, interval in enumerate(intervals):
        expected = nominal_interval
        if expected is None or expected <= 0.0:
            if interval <= 0.0:
                irregular_indices.append(index)
            continue
        relative = abs(interval - expected) / expected
        relative_jitter.append(relative)
        if relative > tolerance or interval <= 0.0:
            irregular_indices.append(index)
        if interval > expected * (1.0 + tolerance):
            estimated_dropped_frames += max(0, int(round(interval / expected)) - 1)
    duration_seconds = None
    if timestamps:
        duration_seconds = timestamps[-1] - timestamps[0]
    valid_intervals = [interval for interval in intervals if interval > 0.0]
    fps_values = [1.0 / interval for interval in valid_intervals]
    fps_summary = _summarize(fps_values)
    observed_fps = None
    if len(timestamps) > 1 and duration_seconds is not None and duration_seconds > 0.0 and len(valid_intervals) == len(intervals):
        observed_fps = (len(timestamps) - 1) / duration_seconds
    dropped_frame_denominator = len(intervals) + estimated_dropped_frames
    return {
        "provided": True,
        "count": len(timestamps),
        "first": timestamps[0] if timestamps else None,
        "last": timestamps[-1] if timestamps else None,
        "duration_seconds": duration_seconds,
        "median_interval": median_interval,
        "nominal_interval": nominal_interval,
        "nominal_interval_source": expected_source,
        "interval_count": len(intervals),
        "valid_interval_count": len(valid_intervals),
        "strictly_increasing": all(interval > 0.0 for interval in intervals),
        "irregular_interval_count": len(irregular_indices),
        "irregular_interval_indices": irregular_indices,
        "estimated_dropped_frames": estimated_dropped_frames,
        "estimated_dropped_frame_ratio": (
            estimated_dropped_frames / dropped_frame_denominator
            if dropped_frame_denominator else 0.0
        ),
        "relative_jitter": _summarize(relative_jitter),
        "fps": {
            "observed": observed_fps,
            "p05": _percentile(fps_values, 0.05),
            "mean": fps_summary["mean"],
            "p50": fps_summary["p50"],
            "p90": fps_summary["p90"],
            "p95": fps_summary["p95"],
            "p99": fps_summary["p99"],
            "min": min(fps_values) if fps_values else None,
            "max": fps_summary["max"],
        },
        "cadence_tolerance": tolerance,
    }


def _timestamp_pairs(
    legacy_timestamps: Optional[Sequence[float]],
    candidate_timestamps: Optional[Sequence[float]],
) -> Tuple[List[Tuple[int, int]], dict]:
    """Pair monotonically captured frames by nearest timestamp without reuse."""
    if legacy_timestamps is None or candidate_timestamps is None:
        pair_count = min(
            len(legacy_timestamps) if legacy_timestamps is not None else 0,
            len(candidate_timestamps) if candidate_timestamps is not None else 0,
        )
        return (
            [(index, index) for index in range(pair_count)],
            {
                "provided": False,
                "pairing": "natural filename order by index",
                "unmatched_legacy_frame_count": 0,
                "unmatched_candidate_frame_count": 0,
            },
        )

    pairs: List[Tuple[int, int]] = []
    legacy_index = 0
    candidate_index = 0
    while legacy_index < len(legacy_timestamps) and candidate_index < len(candidate_timestamps):
        current_difference = abs(
            legacy_timestamps[legacy_index] - candidate_timestamps[candidate_index]
        )
        next_legacy_difference = None
        if legacy_index + 1 < len(legacy_timestamps):
            next_legacy_difference = abs(
                legacy_timestamps[legacy_index + 1] - candidate_timestamps[candidate_index]
            )
        next_candidate_difference = None
        if candidate_index + 1 < len(candidate_timestamps):
            next_candidate_difference = abs(
                legacy_timestamps[legacy_index] - candidate_timestamps[candidate_index + 1]
            )
        if (
            next_candidate_difference is not None
            and next_candidate_difference < current_difference
            and (next_legacy_difference is None or next_candidate_difference <= next_legacy_difference)
        ):
            candidate_index += 1
            continue
        if next_legacy_difference is not None and next_legacy_difference < current_difference:
            legacy_index += 1
            continue
        pairs.append((legacy_index, candidate_index))
        legacy_index += 1
        candidate_index += 1

    paired_legacy = {legacy_index for legacy_index, _ in pairs}
    paired_candidate = {candidate_index for _, candidate_index in pairs}
    absolute_differences = [
        abs(legacy_timestamps[legacy_index] - candidate_timestamps[candidate_index])
        for legacy_index, candidate_index in pairs
    ]
    return (
        pairs,
        {
            "provided": True,
            "pairing": "nearest timestamp order without frame reuse",
            "paired_frame_count": len(pairs),
            "unmatched_legacy_frame_count": len(legacy_timestamps) - len(paired_legacy),
            "unmatched_candidate_frame_count": len(candidate_timestamps) - len(paired_candidate),
            "timestamp_delta_seconds": _summarize(absolute_differences),
        },
    )


def _write_ppm(path: Path, width: int, height: int, pixels: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        ("P6\n{} {}\n255\n".format(width, height)).encode("ascii") + pixels
    )


def _error_heatmap(reference: Frame, candidate: Frame) -> bytes:
    pixels = bytearray()
    for offset in range(0, len(reference.pixels), 3):
        error = max(
            abs(reference.pixels[offset] - candidate.pixels[offset]),
            abs(reference.pixels[offset + 1] - candidate.pixels[offset + 1]),
            abs(reference.pixels[offset + 2] - candidate.pixels[offset + 2]),
        )
        pixels.extend((error, min(255, error * 2), 0))
    return bytes(pixels)


def _flicker_heatmap(
    previous_reference: Frame,
    current_reference: Frame,
    previous_candidate: Frame,
    current_candidate: Frame,
    low_motion_threshold: float,
) -> bytes:
    pixels = bytearray()
    for offset in range(0, len(current_reference.pixels), 3):
        reference_delta = max(
            abs(current_reference.pixels[offset] - previous_reference.pixels[offset]),
            abs(current_reference.pixels[offset + 1] - previous_reference.pixels[offset + 1]),
            abs(current_reference.pixels[offset + 2] - previous_reference.pixels[offset + 2]),
        )
        candidate_delta = max(
            abs(current_candidate.pixels[offset] - previous_candidate.pixels[offset]),
            abs(current_candidate.pixels[offset + 1] - previous_candidate.pixels[offset + 1]),
            abs(current_candidate.pixels[offset + 2] - previous_candidate.pixels[offset + 2]),
        )
        residual = max(0, candidate_delta - reference_delta)
        if reference_delta / 255.0 > low_motion_threshold:
            residual = 0
        pixels.extend((residual, 0, 0))
    return bytes(pixels)


def _write_diagnostics(
    legacy_frames: Sequence[Frame],
    candidate_frames: Sequence[Frame],
    pair_indices: Sequence[Tuple[int, int]],
    frame_records: Sequence[dict],
    temporal_records: Sequence[dict],
    options: AnalysisOptions,
) -> List[str]:
    if options.diagnostic_dir is None or options.diagnostic_limit <= 0:
        return []
    diagnostic_dir = Path(options.diagnostic_dir)
    diagnostic_dir.mkdir(parents=True, exist_ok=True)
    limit = min(options.diagnostic_limit, len(frame_records))
    frame_indices = sorted(
        range(len(frame_records)),
        key=lambda index: frame_records[index]["image_error"]["mae"],
        reverse=True,
    )[:limit]
    paths = []
    for index in frame_indices:
        legacy_index, candidate_index = pair_indices[index]
        reference = legacy_frames[legacy_index]
        candidate = candidate_frames[candidate_index]
        output = diagnostic_dir / "frame_{:06d}_error.ppm".format(index)
        _write_ppm(output, reference.width, reference.height, _error_heatmap(reference, candidate))
        paths.append(str(output))
    transition_limit = min(options.diagnostic_limit, len(temporal_records))
    transition_indices = sorted(
        range(len(temporal_records)),
        key=lambda index: (
            temporal_records[index]["flicker_pixel_ratio"],
            temporal_records[index]["flicker_residual"]["p95"] or 0.0,
            temporal_records[index]["flicker_residual"]["mean"] or 0.0,
        ),
        reverse=True,
    )[:transition_limit]
    for transition_index in transition_indices:
        previous_legacy_index, previous_candidate_index = pair_indices[transition_index]
        current_legacy_index, current_candidate_index = pair_indices[transition_index + 1]
        previous_reference = legacy_frames[previous_legacy_index]
        current_reference = legacy_frames[current_legacy_index]
        previous_candidate = candidate_frames[previous_candidate_index]
        current_candidate = candidate_frames[current_candidate_index]
        output = diagnostic_dir / "transition_{:06d}_flicker.ppm".format(transition_index + 1)
        _write_ppm(
            output,
            current_reference.width,
            current_reference.height,
            _flicker_heatmap(
                previous_reference,
                current_reference,
                previous_candidate,
                current_candidate,
                options.low_motion_threshold,
            ),
        )
        paths.append(str(output))
    return paths


def analyze_directories(
    legacy_directory: Path,
    candidate_directory: Path,
    options: Optional[AnalysisOptions] = None,
) -> dict:
    options = options or AnalysisOptions()
    legacy_paths = discover_frames(Path(legacy_directory))
    candidate_paths = discover_frames(Path(candidate_directory))
    legacy_timestamps = (
        _parse_timestamps(Path(options.legacy_timestamps), len(legacy_paths))
        if options.legacy_timestamps is not None
        else None
    )
    candidate_timestamps = (
        _parse_timestamps(Path(options.candidate_timestamps), len(candidate_paths))
        if options.candidate_timestamps is not None
        else None
    )
    if not legacy_paths or not candidate_paths:
        raise ValueError("both frame sequences must contain at least one pair")

    legacy_frames = [load_frame(path) for path in legacy_paths]
    candidate_frames = [load_frame(path) for path in candidate_paths]
    first_dimensions = (legacy_frames[0].width, legacy_frames[0].height)
    for index, frame in enumerate(legacy_frames):
        if (frame.width, frame.height) != first_dimensions:
            raise ValueError("legacy frame dimensions change at frame {}".format(index))
    for index, frame in enumerate(candidate_frames):
        if (frame.width, frame.height) != first_dimensions:
            raise ValueError("candidate frame dimensions do not match at frame {}".format(index))

    legacy_timestamp_report = _timestamp_report(
        legacy_timestamps,
        options.cadence_tolerance,
    )
    candidate_timestamp_report = _timestamp_report(
        candidate_timestamps,
        options.cadence_tolerance,
        legacy_timestamps,
    )
    if legacy_timestamps is not None and candidate_timestamps is not None:
        pair_indices, timestamp_pairing = _timestamp_pairs(
            legacy_timestamps,
            candidate_timestamps,
        )
    else:
        pair_count = min(len(legacy_frames), len(candidate_frames))
        pair_indices = [(index, index) for index in range(pair_count)]
        timestamp_pairing = {
            "provided": False,
            "pairing": "natural filename order by index",
            "paired_frame_count": pair_count,
            "unmatched_legacy_frame_count": len(legacy_frames) - pair_count,
            "unmatched_candidate_frame_count": len(candidate_frames) - pair_count,
        }
    if not pair_indices:
        raise ValueError("timestamps do not contain an alignable frame pair")

    frames = []
    temporal_records = []
    legacy_hashes = [_frame_hash(frame) for frame in legacy_frames]
    candidate_hashes = [_frame_hash(frame) for frame in candidate_frames]
    previous_reference = None
    previous_candidate = None
    for index, (legacy_index, candidate_index) in enumerate(pair_indices):
        reference = legacy_frames[legacy_index]
        candidate = candidate_frames[candidate_index]
        frame_record = {
            "index": index,
            "legacy_index": legacy_index,
            "candidate_index": candidate_index,
            "legacy_file": reference.name,
            "candidate_file": candidate.name,
            "image_error": _image_metrics(reference, candidate, options.pixel_error_threshold),
        }
        if legacy_timestamps is not None:
            frame_record["legacy_timestamp"] = legacy_timestamps[legacy_index]
        if candidate_timestamps is not None:
            frame_record["candidate_timestamp"] = candidate_timestamps[candidate_index]
        frames.append(frame_record)
        if previous_reference is not None and previous_candidate is not None:
            temporal = _temporal_metrics(
                previous_reference,
                reference,
                previous_candidate,
                candidate,
                options.low_motion_threshold,
                options.flicker_threshold,
            )
            temporal["frame_index"] = index
            if legacy_timestamps is not None:
                temporal["legacy_interval_seconds"] = (
                    legacy_timestamps[legacy_index] - legacy_timestamps[pair_indices[index - 1][0]]
                )
            if candidate_timestamps is not None:
                temporal["candidate_interval_seconds"] = (
                    candidate_timestamps[candidate_index] - candidate_timestamps[pair_indices[index - 1][1]]
                )
            temporal_records.append(temporal)
        previous_reference = reference
        previous_candidate = candidate

    legacy_duplicates = _duplicate_report(
        legacy_hashes,
        legacy_timestamps,
        legacy_timestamp_report.get("nominal_interval"),
    )
    candidate_duplicates = _duplicate_report(
        candidate_hashes,
        candidate_timestamps,
        candidate_timestamp_report.get("nominal_interval"),
    )
    frame_count_match = len(legacy_paths) == len(candidate_paths)
    warnings = []
    if not frame_count_match:
        warnings.append("frame sequences have different lengths; only aligned frames were compared")
    if timestamp_pairing["provided"] and (
        timestamp_pairing["unmatched_legacy_frame_count"]
        or timestamp_pairing["unmatched_candidate_frame_count"]
    ):
        warnings.append("timestamp pairing left unmatched frames outside the shared capture timeline")
    report = {
        "format": "generalsgamecode.visual-parity.v1",
        "status": "ok" if not warnings else "warning",
        "options": {
            "low_motion_threshold": options.low_motion_threshold,
            "flicker_threshold": options.flicker_threshold,
            "pixel_error_threshold": options.pixel_error_threshold,
            "cadence_tolerance": options.cadence_tolerance,
        },
        "alignment": {
            "legacy_frame_count": len(legacy_paths),
            "candidate_frame_count": len(candidate_paths),
            "paired_frame_count": len(pair_indices),
            "frame_count_match": frame_count_match,
            "width": first_dimensions[0],
            "height": first_dimensions[1],
            "pairing": timestamp_pairing["pairing"],
            "timestamp_pairing": timestamp_pairing,
            "capture_duration_seconds": {
                "legacy": legacy_timestamp_report.get("duration_seconds"),
                "candidate": candidate_timestamp_report.get("duration_seconds"),
            },
        },
        "frames": frames,
        "summary": {
            "image_error": {
                key: _summarize([frame["image_error"][key] for frame in frames])
                for key in ("mae", "rmse", "max", "p95", "changed_pixel_ratio")
            }
        },
        "temporal": {
            "transitions": temporal_records,
            "summary": {
                key: _summarize([record[key]["mean"] for record in temporal_records if record[key]["mean"] is not None])
                for key in ("reference_change", "candidate_change", "flicker_residual")
            },
            "low_motion_pixel_ratio": _summarize(
                [record["low_motion_pixel_ratio"] for record in temporal_records]
            ),
            "flicker_pixel_ratio": _summarize(
                [record["flicker_pixel_ratio"] for record in temporal_records]
            ),
            "flicker_transition_count": sum(
                1 for record in temporal_records if record["flicker_pixel_count"] > 0
            ),
        },
        "cadence": {
            "legacy": {
                "timestamps": legacy_timestamp_report,
                "duplicates": legacy_duplicates,
            },
            "candidate": {
                "timestamps": candidate_timestamp_report,
                "duplicates": candidate_duplicates,
            },
        },
        "diagnostics": [],
    }
    if warnings:
        report["warnings"] = warnings
    report["diagnostics"] = _write_diagnostics(
        legacy_frames,
        candidate_frames,
        pair_indices,
        frames,
        temporal_records,
        options,
    )
    return report


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Compare time-aligned legacy and D3D11 frame sequences without game changes."
    )
    parser.add_argument("--legacy-dir", "--reference-dir", dest="legacy_dir", required=True)
    parser.add_argument("--d3d11-dir", "--candidate-dir", dest="candidate_dir", required=True)
    parser.add_argument("--legacy-timestamps", type=Path)
    parser.add_argument("--d3d11-timestamps", "--candidate-timestamps", dest="candidate_timestamps", type=Path)
    parser.add_argument("--output", type=Path, help="write JSON here instead of stdout")
    parser.add_argument("--diagnostic-dir", type=Path)
    parser.add_argument("--diagnostic-limit", type=int, default=5)
    parser.add_argument("--low-motion-threshold", type=float, default=0.02)
    parser.add_argument("--flicker-threshold", type=float, default=0.02)
    parser.add_argument("--pixel-error-threshold", type=float, default=0.02)
    parser.add_argument("--cadence-tolerance", type=float, default=0.10)
    parser.add_argument(
        "--fail-on-frame-count-mismatch",
        action="store_true",
        help="return exit code 1 if the two directories contain different frame counts",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    arguments = parser.parse_args(argv)
    if arguments.diagnostic_limit < 0:
        parser.error("--diagnostic-limit cannot be negative")
    options = AnalysisOptions(
        low_motion_threshold=arguments.low_motion_threshold,
        flicker_threshold=arguments.flicker_threshold,
        pixel_error_threshold=arguments.pixel_error_threshold,
        cadence_tolerance=arguments.cadence_tolerance,
        legacy_timestamps=arguments.legacy_timestamps,
        candidate_timestamps=arguments.candidate_timestamps,
        diagnostic_dir=arguments.diagnostic_dir,
        diagnostic_limit=arguments.diagnostic_limit if arguments.diagnostic_dir else 0,
    )
    try:
        report = analyze_directories(arguments.legacy_dir, arguments.candidate_dir, options)
    except (OSError, ValueError, json.JSONDecodeError, zlib.error, struct.error) as error:
        print("visual parity analysis failed: {}".format(error), file=sys.stderr)
        return 2
    serialized = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output is None:
        sys.stdout.write(serialized)
    else:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(serialized, encoding="utf-8")
    if arguments.fail_on_frame_count_mismatch and not report["alignment"]["frame_count_match"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
