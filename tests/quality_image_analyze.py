"""Analyze every native RGB pixel from QualityImageCompare's completed output.

Uses numpy/Pillow already used by tests/analyze_document_quality.py. No resizing,
registration, smoothing, cropping, tone adjustment, or masked overall score.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics

import numpy as np
from PIL import Image


WIDTH, HEIGHT = 2560, 1440
TIMINGS = (
    "encode_api_submit_ms", "encode_wait_readback_ms", "encode_complete_ms",
    "packetize_ms", "decoder_push_ms", "cpu_buffer_decode_ms", "yuv_to_rgb_ms",
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_rgb(path):
    with Image.open(path) as image:
        if image.format != "BMP" or image.size != (WIDTH, HEIGHT):
            raise ValueError(f"Expected native {WIDTH}x{HEIGHT} BMP: {path}")
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def safe_child(folder, name):
    if not isinstance(name, str) or Path(name).name != name or name in ("", ".", ".."):
        raise ValueError("Output log must reference a plain filename")
    return folder / name


def edge_mask(source, threshold):
    """Reference-only mask: mark BOTH endpoints of horizontal/vertical RGB edges."""
    mask = np.zeros(source.shape[:2], dtype=bool)
    horizontal = np.max(np.abs(np.diff(source, axis=1)), axis=2) > threshold
    vertical = np.max(np.abs(np.diff(source, axis=0)), axis=2) > threshold
    mask[:, :-1] |= horizontal
    mask[:, 1:] |= horizontal
    mask[:-1, :] |= vertical
    mask[1:, :] |= vertical
    return mask


def conditional_mae(absolute, mask):
    return float(np.mean(absolute[mask], dtype=np.float64)) if mask.any() else None


def metric(source, received, edges):
    error = received - source
    absolute = np.abs(error)
    mse = float(np.mean(error * error, dtype=np.float64))
    dark = np.all(source <= 30, axis=2)
    white = np.all(source >= 240, axis=2)
    # Gradient MAE covers every horizontal/vertical neighbor and every channel.
    # It supplements the complete image score; it does not discard difficult pixels.
    horizontal = np.abs(np.diff(received, axis=1) - np.diff(source, axis=1))
    vertical = np.abs(np.diff(received, axis=0) - np.diff(source, axis=0))
    gradient_mae = float((np.sum(horizontal, dtype=np.float64) + np.sum(vertical, dtype=np.float64))
                         / (horizontal.size + vertical.size))
    return {
        "compared_pixels": WIDTH * HEIGHT,
        "excluded_pixels": 0,
        "rgb_mae": float(np.mean(absolute, dtype=np.float64)),
        "rgb_mse": mse,
        "rgb_rmse": math.sqrt(mse),
        "rgb_psnr_db": 10 * math.log10(255**2 / mse) if mse else None,
        "rgb_psnr_is_infinite": mse == 0,
        "absolute_channel_error_p95": float(np.percentile(absolute, 95)),
        "absolute_channel_error_p99": float(np.percentile(absolute, 99)),
        "absolute_channel_error_max": int(np.max(absolute)),
        "pixel_max_channel_error_gt_12_fraction": float(np.mean(np.max(absolute, axis=2) > 12)),
        "exact_rgb_pixel_fraction": float(np.mean(np.all(error == 0, axis=2))),
        "signed_bias_rgb": np.mean(error, axis=(0, 1), dtype=np.float64).tolist(),
        "mae_rgb": np.mean(absolute, axis=(0, 1), dtype=np.float64).tolist(),
        "edge_pixels": int(np.sum(edges)),
        "edge_mae": conditional_mae(absolute, edges),
        "nonedge_pixels": int(np.sum(~edges)),
        "nonedge_mae": conditional_mae(absolute, ~edges),
        "gradient_mae": gradient_mae,
        "dark_pixels": int(np.sum(dark)),
        "dark_mae": conditional_mae(absolute, dark),
        "white_pixels": int(np.sum(white)),
        "white_mae": conditional_mae(absolute, white),
    }


def describe(values):
    return {"mean": statistics.fmean(values), "median": statistics.median(values),
            "min": min(values), "max": max(values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Completed QualityImageCompare output directory")
    parser.add_argument("--edge-threshold", type=float, default=12,
                        help="Reference RGB neighbor threshold on 0..255 samples (default: 12)")
    args = parser.parse_args()
    if not math.isfinite(args.edge_threshold) or not 0 <= args.edge_threshold <= 255:
        raise ValueError("Edge threshold must be finite and in 0..255")
    folder = args.directory.resolve()
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    completion = json.loads((folder / "complete.json").read_text(encoding="utf-8"))
    repetitions = manifest["repetitions_per_condition"]
    if type(repetitions) is not int or repetitions < 1:
        raise ValueError("Invalid repetitions_per_condition")
    budgets = manifest.get("budgets")
    dithers = manifest.get("skip_dither_values")
    if (not isinstance(budgets, list) or not budgets or any(type(b) is not int or b <= 0 for b in budgets)
            or len(set(budgets)) != len(budgets)):
        raise ValueError("Unexpected experiment setting: budgets")
    if (not isinstance(dithers, list) or not dithers or any(type(d) is not bool for d in dithers)
            or len(set(dithers)) != len(dithers)):
        raise ValueError("Unexpected experiment setting: skip_dither_values")
    if completion != {"complete": True, "measured_frames": len(budgets) * len(dithers) * repetitions}:
        raise ValueError("Run is incomplete or completion marker does not match the manifest")
    expected_manifest = {
        "width": WIDTH, "height": HEIGHT, "chroma": "444", "precision": 1,
        "input_color_space": "sRGB_full", "output_color_space": "sRGB_full",
        "ycbcr_matrix": "BT709", "ycbcr_range": "full", "chroma_midpoint": "128/255",
        "intermediate_plane_format": "R8_UNORM",
        "packet_boundary_bytes": 32768,
    }
    for key, value in expected_manifest.items():
        if manifest.get(key) != value:
            raise ValueError(f"Unexpected experiment setting: {key}")
    reference_path = safe_child(folder, manifest["reference"])
    reference_u8 = load_rgb(reference_path)
    source_audit = {"reference_sha256": sha256(reference_path),
                    "reference_rgb_sha256": hashlib.sha256(reference_u8.tobytes()).hexdigest()}
    original_path = Path(manifest["source_path"])
    source_audit["original_source_available"] = original_path.is_file()
    if original_path.is_file():
        original = load_rgb(original_path)
        if not np.array_equal(reference_u8, original):
            raise ValueError("Original source RGB pixels differ from saved reference; source may have changed")
        source_audit["original_source_sha256"] = sha256(original_path)
        source_audit["original_rgb_equals_reference"] = True
    source = reference_u8.astype(np.float32)
    edges = edge_mask(source, args.edge_threshold)
    records = [json.loads(line) for line in (folder / "measurements.jsonl").read_text(encoding="utf-8").splitlines() if line.strip()]
    expected = {(budget, dither, rep) for budget in budgets
                for dither in dithers for rep in range(repetitions)}
    seen = set()
    analyzed = []
    for record in records:
        if type(record["skip_dither"]) is not bool or type(record["repetition"]) is not int:
            raise ValueError("Invalid condition or repetition type")
        key = (record["budget_bytes"], record["skip_dither"], record["repetition"])
        if key not in expected or key in seen:
            raise ValueError(f"Unexpected or repeated condition: {key}")
        seen.add(key)
        if type(record["actual_bitstream_bytes"]) is not int or record["actual_bitstream_bytes"] <= 0:
            raise ValueError(f"Invalid actual packetized bytes: {key}")
        for timing in TIMINGS:
            if not math.isfinite(record[timing]) or record[timing] < 0:
                raise ValueError(f"Invalid {timing}: {key}")
        bitmap = safe_child(folder, record["bitmap"])
        received = load_rgb(bitmap).astype(np.float32)
        analyzed.append({**record, "bitmap_sha256": sha256(bitmap), **metric(source, received, edges)})
    if seen != expected:
        raise ValueError(f"Missing conditions: {sorted(expected - seen)}")
    summaries = []
    summary_metrics = ("actual_bitstream_bytes", *TIMINGS, "rgb_mae", "rgb_mse", "rgb_rmse",
                       "edge_mae", "nonedge_mae", "gradient_mae", "absolute_channel_error_p95")
    for budget in budgets:
        for skip in dithers:
            group = [row for row in analyzed if row["budget_bytes"] == budget and row["skip_dither"] == skip]
            summary = {"budget_bytes": budget, "skip_dither": skip, "samples": len(group)}
            for name in summary_metrics:
                values = [row[name] for row in group if row[name] is not None]
                summary[name] = describe(values) if values else None
            # Pool MSE over all same-size frames, then derive PSNR. Do not average dB scores.
            pooled_mse = summary["rgb_mse"]["mean"]
            summary["pooled_rgb_psnr_db"] = 10 * math.log10(255**2 / pooled_mse) if pooled_mse else None
            summary["pooled_rgb_psnr_is_infinite"] = pooled_mse == 0
            summaries.append(summary)
    report = {
        "scope": "Same static real document and fixed codec budget; no capture/network/display or 165 Hz conclusion",
        "pixel_policy": "All 2560x1440 RGB pixels; excluded_pixels=0; native rows; no alignment, smoothing, crop, or correction",
        "edge_policy": f"Source-only 4-neighbor RGB difference > {args.edge_threshold}; both endpoints included; supplemental metric",
        "sample_unit": "8-bit gamma-coded RGB channel values on 0..255 scale",
        "infinite_psnr_policy": "Null psnr plus explicit psnr_is_infinite=true when MSE equals zero",
        "manifest": manifest, "source_audit": source_audit, "samples": analyzed, "conditions": summaries,
    }
    (folder / "analysis.json").write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    csv_fields = ("budget_bytes", "skip_dither", "repetition", "bitmap", "actual_bitstream_bytes", "packet_count",
                  "maximum_packet_bytes", "packets_over_boundary",
                  *TIMINGS, "rgb_mae", "rgb_psnr_db", "rgb_psnr_is_infinite", "edge_mae", "nonedge_mae",
                  "gradient_mae", "absolute_channel_error_p95", "excluded_pixels")
    with (folder / "analysis_samples.csv").open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=csv_fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(analyzed)
    print("budget skip_dither mean_bytes pooled_RGB_PSNR edge_MAE median_encode_ms median_decode_ms")
    for item in summaries:
        psnr = item["pooled_rgb_psnr_db"]
        edge = item["edge_mae"]
        psnr_text = f"{psnr:.3f}" if psnr is not None else "inf"
        print(f'{item["budget_bytes"]} {str(item["skip_dither"]).lower()} '
              f'{item["actual_bitstream_bytes"]["mean"]:.1f} {psnr_text}',
              f'{edge["mean"]:.4f}' if edge else "n/a",
              f'{item["encode_complete_ms"]["median"]:.3f} {item["cpu_buffer_decode_ms"]["median"]:.3f}')


if __name__ == "__main__":
    main()
