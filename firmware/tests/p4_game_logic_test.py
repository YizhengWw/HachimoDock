"""Compile and run the heap-free P4 mini-game engine on the host."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def test_p4_game_engine_moves_blocks_snake_and_flappy():
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "p4-game-test"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(ROOT / "main" / "pet_p4_game.c"),
                str(ROOT / "tests" / "p4_game_logic_test.c"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
