from pathlib import Path
import importlib.util
import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('budget', ROOT / 'tools/firmware_budget.py')
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


def test_headroom_and_internal_budget():
    sections = {'.iram0.text': 110000, '.dram0.data': 25000, '.dram0.bss': 210000,
                '.ext_ram.bss': 1600000}
    result = budget.evaluate(2350000, 0x280000, sections)
    assert result['otaHeadroomBytes'] == 271440
    assert result['internalStaticBytes'] == 345000
    with pytest.raises(ValueError, match='OTA headroom'):
        budget.evaluate(0x280000 - 100, 0x280000, sections)
    with pytest.raises(ValueError, match='Internal static'):
        budget.evaluate(2350000, 0x280000, {'.dram0.bss': 400000})
