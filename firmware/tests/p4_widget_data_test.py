"""Execute the actual bounded-data C implementation with host lock stubs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def test_list_header_date_and_price_only_tone():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    renderer = source.split("static void render_data_miniapp_page", 1)[1].split("static void render_miniapp_page", 1)[0]
    assert "draw_text_line(data->date," in renderer
    assert "draw_text_line(r->label, 24, y, 235, ink," in renderer
    assert "draw_text_line(r->value, 270, y, 176, tone," in renderer
    assert "draw_text_line(r->detail, 465, y + 2, 153, tone," in renderer
    assert "draw_text_line(r->meta, 24, y + 29, 592, muted," in renderer

def test_widget_data_runtime():
    cjson = ROOT / "tests/vendor/cjson"
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "widget-data-test"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/stubs/widget_data"), "-I", str(cjson),
                        str(ROOT / "main/pet_p4_widget_data.c"), str(cjson / "cJSON.c"),
                        str(ROOT / "tests/p4_widget_data_test.c"), "-lm", "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
