"""Re-encode PNGs in a separate, stored ZIP; never modify source assets or PAK.

No resizing, palette conversion, JPEG/audio transcoding, or alpha quantization.
The optional light-lossy mode rounds atlas RGB to a four-level step (error <= 2/255).
Standalone images (including font/alpha-mask sheets) stay lossless. Reports are separate
from the authoritative resource manifest, whose source-pixel hashes stay intact.
"""
from __future__ import annotations
from game_fixture import game_root

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import zipfile
import zlib

from PIL import Image, ImageChops, ImageStat

SIGNATURE = b"\x89PNG\r\n\x1a\n"


def chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    if not data.startswith(SIGNATURE):
        raise ValueError("Invalid PNG signature")
    result, at = [], 8
    while at < len(data):
        size = struct.unpack_from(">I", data, at)[0]
        kind, payload = data[at + 4:at + 8], data[at + 8:at + 8 + size]
        end = at + 12 + size
        if end > len(data) or zlib.crc32(kind + payload) != struct.unpack_from(">I", data, end - 4)[0]:
            raise ValueError("Invalid PNG chunk length/CRC")
        result.append((kind, payload))
        at = end
    if not result or result[0][0] != b"IHDR" or result[-1][0] != b"IEND":
        raise ValueError("Incomplete PNG")
    return result


def replace_idat(original: list[tuple[bytes, bytes]], stream: bytes) -> bytes:
    result, emitted = bytearray(SIGNATURE), False
    for kind, payload in original:
        if kind == b"IDAT":
            if emitted:
                continue
            emitted, payload = True, stream
        result += struct.pack(">I", len(payload)) + kind + payload
        result += struct.pack(">I", zlib.crc32(kind + payload))
    return bytes(result)


def encode_pixels(image: Image.Image, original: list[tuple[bytes, bytes]]) -> bytes | None:
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True, compress_level=9)
    encoded = chunks(buffer.getvalue())
    # Keep every original metadata chunk. Only replace compatible pixel data.
    if encoded[0] != original[0]:
        return None
    return replace_idat(original, b"".join(v for k, v in encoded if k == b"IDAT"))


def compact_png(name: str, data: bytes, mode: str) -> tuple[bytes, dict]:
    original = chunks(data)
    if any(k == b"acTL" for k, _ in original):
        return data, {"reason": "animated PNG preserved", "max_rgb_error": 0}
    raw = zlib.decompress(b"".join(v for k, v in original if k == b"IDAT"))
    best = data
    for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
        encoder = zlib.compressobj(9, zlib.DEFLATED, 15, 9, strategy)
        candidate = replace_idat(original, encoder.compress(raw) + encoder.flush())
        if len(candidate) < len(best):
            best = candidate
    with Image.open(io.BytesIO(data)) as image:
        image.load()
        # RGB/RGBA re-filtering is safe without changing PLTE/tRNS semantics.
        if image.mode in {"RGB", "RGBA"}:
            candidate = encode_pixels(image, original)
            if candidate is not None and len(candidate) < len(best):
                best = candidate
        # tRNS colors are exact color keys; do not change their RGB samples.
        lossy = (mode == "light-lossy" and name.lower().startswith("compiled/")
                 and name.lower().endswith(".atlas.png")
                 and image.mode in {"RGB", "RGBA"}
                 and not any(k == b"tRNS" for k, _ in original))
        if lossy:
            table = [min(255, ((value + 2) // 4) * 4) for value in range(256)]
            bands = image.split()
            quantized = Image.merge(image.mode, tuple(b.point(table) for b in bands[:3]) + bands[3:])
            candidate = encode_pixels(quantized, original)
            if candidate is not None and len(candidate) < len(best):
                best = candidate
        with Image.open(io.BytesIO(best)) as output:
            before, after = image.convert("RGBA"), output.convert("RGBA")
            if output.size != image.size or before.getchannel("A").tobytes() != after.getchannel("A").tobytes():
                raise ValueError(f"Dimensions/alpha changed: {name}")
            difference = ImageChops.difference(before.convert("RGB"), after.convert("RGB"))
            maximum = max(hi for _, hi in difference.getextrema())
            if maximum > (2 if lossy else 0):
                raise ValueError(f"Pixel error exceeds policy: {name}: {maximum}")
            # Enforce that all non-IDAT chunks, including color metadata, survive.
            if [(k, v) for k, v in chunks(best) if k != b"IDAT"] != [(k, v) for k, v in original if k != b"IDAT"]:
                raise ValueError(f"PNG metadata changed: {name}")
            return best, {"max_rgb_error": maximum,
                          "rgb_rmse": ImageStat.Stat(difference).rms,
                          "width": image.width, "height": image.height,
                          "alpha_unchanged": True}


def compact_package(source: Path, output: Path, mode: str = "lossless", workers: int = 2) -> dict:
    source, output = source.resolve(), output.resolve()
    if source == output:
        raise ValueError("Compact output must differ from the source package")
    if mode not in {"lossless", "light-lossy"} or not 1 <= workers <= 8:
        raise ValueError("Invalid compact mode/worker count")
    output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    # Read each source entry before dispatch; ZipFile is not shared by workers.
    with zipfile.ZipFile(source) as archive:
        names = archive.namelist()
        if len(set(names)) != len(names):
            raise ValueError("Duplicate archive paths")
        def process(item):
            name, data = item
            cooked, details = compact_png(name, data, mode) if name.lower().endswith(".png") else (data, {})
            return name, cooked, {"path": name, "before": len(data), "after": len(cooked),
                                 "source_sha256": hashlib.sha256(data).hexdigest(),
                                 "output_sha256": hashlib.sha256(cooked).hexdigest(), **details}
        with tempfile.TemporaryDirectory(prefix=".compact-", dir=output.parent) as directory:
            temporary = Path(directory) / "package.pak64"
            with ThreadPoolExecutor(max_workers=workers) as pool, zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED) as target:
                # Bound decoded-image memory, including on Python before 3.14.
                for start in range(0, len(names), workers):
                    batch = [(n, archive.read(n)) for n in names[start:start + workers]]
                    for name, data, row in pool.map(process, batch):
                        info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                        info.create_system, info.external_attr = 0, 0x20
                        target.writestr(info, data)
                        rows.append(row)
            with zipfile.ZipFile(temporary) as check:
                if check.namelist() != names or check.testzip() is not None:
                    raise ValueError("Compact archive integrity failed")
                if any(i.compress_type != zipfile.ZIP_STORED for i in check.infolist()):
                    raise ValueError("Runtime supports stored ZIP entries only")
            report = {"format": "pvz-compact-package-v1", "mode": mode,
                      "source": str(source), "output": str(output),
                      "source_bytes": source.stat().st_size, "output_bytes": temporary.stat().st_size,
                      "changed_files": sum(r["source_sha256"] != r["output_sha256"] for r in rows),
                      "lossy_files": sum(r.get("max_rgb_error", 0) > 0 for r in rows), "files": rows}
            temporary.replace(output)
    output.with_suffix(".report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--mode", choices=("lossless", "light-lossy"), default="lossless")
    parser.add_argument("--workers", type=int, default=2)
    args = parser.parse_args()
    report = compact_package(args.source, args.output or args.source.with_name("main-compact.pak64"), args.mode, args.workers)
    print(json.dumps({k: v for k, v in report.items() if k != "files"}, indent=2))


if __name__ == "__main__":
    main()
