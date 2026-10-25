#!/usr/bin/env python3
"""Prove World NJA2 keeps its native hit-test lens while rendering is widened.

Usage: python ninja_native_ray_check.py STOCK_RAM WIDE_RAM --require-ray

Use captures with the same camera-record angle and stable camera pose. RAM must
stay in private ignored test data. This script only reads it, prints numeric
verification metadata, and writes no captures, game bytes, or disassembly.

NJA2 native evidence: 106900 returns cameraRecord+236; 131120 derives an
independent input scale from its half-angle. 1311A8 builds/rotates the aim ray;
13B0E4 caches a separate collision focal; 13C48E uses it to project hit geometry.
Only the drawing builder's half-angle instruction 08F8C8 is changed for widening.
"""
import argparse
import math
import struct
from pathlib import Path

CAMERA_BASE, CAMERA_STRIDE, CAMERA_COUNT = 0x385818, 252, 9
GUARDS = {
    0x106900: 0xD309, 0x106902: 0x910C, 0x106904: 0x6032,
    0x106908: 0x001E, 0x10691E: 0x00EC,
    0x13112E: 0xD31A, 0x13113E: 0x4021,
    0x1311C4: 0xF312, 0x1311DC: 0xF302,
    0x13B24E: 0x4021, 0x13B264: 0xFC03, 0x13B268: 0xF3CA,
    0x13C4AE: 0xF232, 0x13C4B4: 0xF233,
}

def ensure(condition, message):
    if not condition:
        raise ValueError(message)

class Ram:
    def __init__(self, path, label):
        self.data = path.read_bytes()
        self.label = label
        ensure(len(self.data) == 32 * 1024 * 1024, f"{label}: expected a 32 MiB NAOMI RAM capture")
        for offset, value in GUARDS.items():
            ensure(self.u16(offset) == value, f"{label}: native code guard failed at {offset:06X}")
        pointer = self.u32(0x3860F4) & 0x1FFFFFF
        delta = pointer - CAMERA_BASE
        ensure(delta >= 0 and delta % CAMERA_STRIDE == 0 and delta // CAMERA_STRIDE < CAMERA_COUNT,
               f"{label}: camera-record pointer is outside the measured array")
        self.camera = pointer
        self.angle = self.u32(pointer + 236)
        ensure(1 < self.angle < 16384, f"{label}: angle does not produce valid stock and widened lenses")
        self.k = self.floats(0x3B5880, 1)[0]
        self.hit_focal = self.floats(0x3CEBDC, 1)[0]
        projection = self.floats(0xE0600, 16)
        viewport = self.floats(0xE0728, 16)
        self.draw_focal = abs(projection[0] * viewport[0])
        ensure(abs(self.draw_focal - abs(projection[5] * viewport[5])) < 0.01,
               f"{label}: unequal drawing X/Y pixel focal")

    def u16(self, offset):
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from("<I", self.data, offset)[0]

    def floats(self, offset, count):
        values = struct.unpack_from(f"<{count}f", self.data, offset)
        ensure(all(math.isfinite(value) for value in values), f"{self.label}: nonfinite native float at {offset:06X}")
        return values

    def verify_rays(self, ray_tolerance, origin_tolerance):
        checked = 0
        rotation = self.floats(self.camera + 164, 9)
        camera_origin = self.floats(self.camera + 224, 3)
        for player in range(2):
            state = 0x3869BC + player * 0x30
            gun = 0x3B5808 + player * 0x3C
            active = self.data[state] == 1 and self.data[state + 1] == 1 and self.data[state + 2] == 1
            x, y = struct.unpack_from("<2i", self.data, gun + 24)
            if not active or self.u16(0x3E6978) != 1 or self.u32(gun + 4) != 1 or not (0 <= x <= 640 and 0 <= y <= 480):
                print(f"SKIP {self.label} P{player + 1}: no current active on-screen combat ray")
                continue
            direction = self.floats(gun + 36, 3)
            origin = self.floats(gun + 48, 3)
            ensure(abs(sum(value * value for value in direction) - 1.0) < 0.01,
                   f"{self.label} P{player + 1}: stored world direction is not normalized")
            # Native 12EBB6 treats the record's three successive vec3 values as
            # matrix columns. Its inverse rotation is this row-wise dot product.
            local = [sum(rotation[row * 3 + col] * direction[col] for col in range(3)) for row in range(3)]
            expected = [(x - 320.5) * self.k, (240.5 - y) * self.k, -1000.0]
            length = math.sqrt(sum(value * value for value in expected))
            expected = [value / length for value in expected]
            direction_error = max(abs(a - b) for a, b in zip(local, expected))
            origin_error = max(abs(a - b) for a, b in zip(origin, camera_origin))
            ensure(direction_error <= ray_tolerance,
                   f"{self.label} P{player + 1}: stock ray error {direction_error:.8g} exceeds {ray_tolerance}")
            ensure(origin_error <= origin_tolerance,
                   f"{self.label} P{player + 1}: stale ray origin error {origin_error:.8g} exceeds {origin_tolerance}")
            checked += 1
            print(f"PASS {self.label} P{player + 1}: native ({x},{y}), ray error {direction_error:.8g}, origin error {origin_error:.8g}")
        return checked

def verify(stock, wide, ray_tolerance, origin_tolerance, require_ray):
    ensure(ray_tolerance > 0 and origin_tolerance >= 0, "verification tolerances must be positive/nonnegative")
    stock, wide = Ram(stock, "stock"), Ram(wide, "wide")
    ensure(stock.u16(0x08F8C8) == 0x4D21, "stock: drawing half-angle instruction is not original")
    ensure(wide.u16(0x08F8C8) == 0x0009, "wide: drawing half-angle instruction is not the guarded NOP")
    ensure(stock.angle == wide.angle, "camera angles differ; choose a comparable stock/wide pair")
    angle = stock.angle * 2 * math.pi / 65536
    half = (stock.angle >> 1) * 2 * math.pi / 65536
    expected_k = 1000 * math.tan(half) / 320.5
    expected_stock = 320 / math.tan(half)
    expected_wide = 320 / math.tan(angle)
    for capture in (stock, wide):
        ensure(abs(capture.k - expected_k) < 1e-6, f"{capture.label}: native input scalar does not match stock camera angle")
        ensure(abs(capture.hit_focal - expected_stock) < 0.01, f"{capture.label}: collision focal does not match stock camera angle")
    for offset, name in ((0x3B5880, "native input scalar"), (0x3CEBDC, "collision focal")):
        ensure(stock.data[offset:offset + 4] == wide.data[offset:offset + 4], f"{name} changed when drawing widened")
        print(f"PASS {name} remains bit-identical at {offset:06X}")
    ensure(abs(stock.draw_focal - expected_stock) < 0.01, "stock drawing focal differs from the camera's stock lens")
    ensure(abs(wide.draw_focal - expected_wide) < 0.01 and wide.draw_focal < stock.draw_focal,
           "wide drawing focal does not match the widened full-angle builder")
    print(f"PASS angle {stock.angle}: drawing focal {stock.draw_focal:.6f} -> {wide.draw_focal:.6f}; hit focal {stock.hit_focal:.6f}; input scalar {stock.k:.9f}")
    counts = [capture.verify_rays(ray_tolerance, origin_tolerance) for capture in (stock, wide)]
    ensure(not require_ray or all(counts), "a current native ray is required in each capture")
    print(f"PASS native ray/lens verification ({sum(counts)} player rays checked)")
    return True

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path)
    parser.add_argument("wide", type=Path)
    parser.add_argument("--ray-tolerance", type=float, default=0.001)
    parser.add_argument("--origin-tolerance", type=float, default=2.0)
    parser.add_argument("--require-ray", action="store_true")
    args = parser.parse_args()
    try:
        return 0 if verify(args.stock,args.wide,args.ray_tolerance,args.origin_tolerance,args.require_ray) else 1
    except (OSError, ValueError, struct.error) as error:
        print(f"FAIL {error}")
        return 1

if __name__ == "__main__":
    raise SystemExit(main())
