#!/usr/bin/env python3
import subprocess
import tempfile
from pathlib import Path


def test_stats_renderer_writes_rgb565_frame() -> None:
    root = Path(__file__).resolve().parents[1]
    renderer = root / "fb-stats-renderer.py"
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        payload = tmp_path / ".stats-display"
        frame = tmp_path / "stats.rgb565"
        payload.write_text(
            "\n".join(
                [
                    "STATS_DASHBOARD_V1",
                    "agent=Codex",
                    "lunch=3.7",
                    "metricValue=1.30M",
                    "metricUnit=TOKEN",
                    "alerts=1",
                    "completed=4",
                    "breakdown=input 900.0K output 400.0K cache 0",
                    "sources=codex 1.30M",
                ]
            )
        )

        subprocess.run(
            [
                "python3",
                str(renderer),
                "--input",
                str(payload),
                "--output-frame",
                str(frame),
                "--width",
                "96",
                "--height",
                "64",
            ],
            check=True,
        )

        data = frame.read_bytes()
        assert len(data) == 96 * 64 * 2
        assert len(set(data)) > 8


def test_stats_renderer_omits_top_right_status_boxes() -> None:
    root = Path(__file__).resolve().parents[1]
    renderer = root / "fb-stats-renderer.py"
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        payload = tmp_path / ".stats-display"
        frame = tmp_path / "stats.rgb565"
        payload.write_text(
            "\n".join(
                [
                    "STATS_DASHBOARD_V1",
                    "agent=Codex",
                    "lunch=3.7",
                    "metricValue=1.30M",
                    "metricUnit=TOKEN",
                    "alerts=1",
                    "completed=4",
                ]
            )
        )

        subprocess.run(
            [
                "python3",
                str(renderer),
                "--input",
                str(payload),
                "--output-frame",
                str(frame),
                "--width",
                "480",
                "--height",
                "640",
            ],
            check=True,
        )

        data = frame.read_bytes()
        upper_right = bytearray()
        for y in range(0, 120):
            start = (y * 480 + 320) * 2
            upper_right.extend(data[start:(y * 480 + 480) * 2])
        yellow = bytes([0x46, 0xF5])
        green = bytes([0x10, 0x56])
        assert yellow not in upper_right
        assert green not in upper_right


def test_stats_renderer_320x240_keeps_dashboard_inside_safe_area() -> None:
    root = Path(__file__).resolve().parents[1]
    renderer = root / "fb-stats-renderer.py"
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        payload = tmp_path / ".stats-display"
        frame = tmp_path / "stats.rgb565"
        payload.write_text(
            "\n".join(
                [
                    "STATS_DASHBOARD_V1",
                    "agent=Codex",
                    "lunch=177.0",
                    "metricValue=61.94M",
                    "metricUnit=TOKEN",
                    "breakdown=input 61.80M output 136.4K cache 59.59M",
                ]
            )
        )

        subprocess.run(
            [
                "python3",
                str(renderer),
                "--input",
                str(payload),
                "--output-frame",
                str(frame),
                "--width",
                "320",
                "--height",
                "240",
            ],
            check=True,
        )

        data = frame.read_bytes()
        panel_border = bytes([0x06, 0x32])
        bottom_guard = bytearray()
        for y in range(232, 240):
            bottom_guard.extend(data[(y * 320) * 2:((y + 1) * 320) * 2])
        assert panel_border not in bottom_guard


def test_stats_renderer_rotates_output_180() -> None:
    root = Path(__file__).resolve().parents[1]
    renderer = root / "fb-stats-renderer.py"
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        payload = tmp_path / ".stats-display"
        normal = tmp_path / "normal.rgb565"
        rotated = tmp_path / "rotated.rgb565"
        payload.write_text("STATS_DASHBOARD_V1\nagent=Codex\nmetricValue=123\n")

        base_cmd = [
            "python3",
            str(renderer),
            "--input",
            str(payload),
            "--width",
            "32",
            "--height",
            "24",
        ]
        subprocess.run(base_cmd + ["--output-frame", str(normal)], check=True)
        subprocess.run(base_cmd + ["--output-frame", str(rotated), "--rotate", "180"], check=True)

        normal_data = normal.read_bytes()
        rotated_data = rotated.read_bytes()
        assert len(rotated_data) == len(normal_data)
        assert rotated_data[:2] == normal_data[-2:]
        assert rotated_data[-2:] == normal_data[:2]


def test_rawvideo_blit_rotates_output_180() -> None:
    root = Path(__file__).resolve().parents[1]
    blit = root / "fb-rawvideo-blit.py"
    with tempfile.TemporaryDirectory() as tmp:
        fb = Path(tmp) / "fb"
        fb.write_bytes(b"\x00" * 16)
        # 4x2 RGB565 pixels: 1..8. 180-degree rotation should become 8..1.
        frame = b"".join(int(pixel).to_bytes(2, "little") for pixel in range(1, 9))
        expected = b"".join(int(pixel).to_bytes(2, "little") for pixel in range(8, 0, -1))
        subprocess.run(
            ["python3", str(blit), str(fb), "4", "2", "0", "180"],
            input=frame,
            check=True,
        )
        assert fb.read_bytes() == expected


if __name__ == "__main__":
    test_stats_renderer_writes_rgb565_frame()
    test_stats_renderer_omits_top_right_status_boxes()
    test_stats_renderer_320x240_keeps_dashboard_inside_safe_area()
    test_stats_renderer_rotates_output_180()
    test_rawvideo_blit_rotates_output_180()
