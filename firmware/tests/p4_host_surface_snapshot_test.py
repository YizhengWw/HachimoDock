"""Execute real miniapp surface resolution and immutable host getters on a host.

Only the runtime aggregate/scheduler/form boundaries are adapted. Production
find_var/text_at/render_integer/render_scroll/app_surface_locked, host ownership
predicates and the actual surface/host headers are compiled unchanged.
No parser, transport, device, live publication or hardware timing claim.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(os.environ.get('P4_HOST_BINDING_FIRMWARE', Path(__file__).resolve().parents[1]))
MINIAPP = Path(os.environ.get('P4_HOST_BINDING_MINIAPP', ROOT / 'main/pet_p4_miniapp.c'))


def function(source, signature):
    """Extract a definition, ignoring braces in C comments/string literals."""
    start = source.index(signature)
    opening = source.index('{', start)
    depth, state, i = 0, 'code', opening
    while i < len(source):
        c, pair = source[i], source[i:i + 2]
        if state == 'line':
            if c == '\n':
                state = 'code'
        elif state == 'comment':
            if pair == '*/':
                state, i = 'code', i + 1
        elif state in ('"', "'"):
            if c == '\\':
                i += 1
            elif c == state:
                state = 'code'
        elif pair == '//':
            state, i = 'line', i + 1
        elif pair == '/*':
            state, i = 'comment', i + 1
        elif c in ('"', "'"):
            state = c
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
        i += 1
    raise ValueError('Unclosed production function: ' + signature)


class HostSurfaceSnapshotTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='host-surface-', dir=os.environ.get('P4_HOST_BINDING_TMP'))
        tmp = Path(cls.temp.name)
        source = MINIAPP.read_text()
        # The actual small variable declaration; the large runtime aggregate is
        # represented only by fields the selected production resolver accesses.
        begin = source.index('typedef enum {\n  MINIAPP_VAR_INT,')
        end = source.index('} miniapp_var_t;', begin) + len('} miniapp_var_t;')
        declarations = source[begin:end]
        fixture = (Path(__file__).parent / 'fixtures/p4_host_surface_snapshot.c').read_text()
        helpers = '\n'.join(function(source, x) for x in (
            'static bool runtime_host_variable(',
            'static bool runtime_host_action(',
            'static void copy_utf8(',
            'static int find_var(',
            'static const char *text_at(',
            'static int32_t render_integer(',
            'static int32_t render_scroll(',
            'static bool app_surface_locked(',
        ))
        clamp = function((ROOT / 'main/pet_p4_surface.c').read_text(), 'int32_t pet_surface_clamp_offset(')
        code = fixture.replace('/* ACTUAL_VARIABLE_DECLARATION */', declarations)
        code = code.replace('/* ACTUAL_RESOLVERS */', clamp + '\n' + helpers)
        path = tmp / 'surface.c'
        path.write_text(code)
        cls.binary = tmp / 'surface'
        subprocess.run([
            'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-g',
            '-I', str(ROOT / 'main'), '-I', str(ROOT / 'tests/vendor/cjson'),
            str(path), '-o', str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check_case(self, case):
        subprocess.run([str(self.binary), str(case)], check=True)

    def test_host_text_and_both_pan_axes(self): self.check_case(0)
    def test_captured_snapshots_are_independent_and_repeatable(self): self.check_case(1)
    def test_host_values_use_existing_declared_limits(self): self.check_case(2)
    def test_text_table_negative_and_past_end_remain_empty(self): self.check_case(3)
    def test_stale_missing_and_wrong_type_use_existing_zero_fallback(self): self.check_case(4)
    def test_native_runtime_keeps_its_values_without_host(self): self.check_case(5)
    def test_missing_optional_offsets_keep_zero_fallback(self): self.check_case(6)
    def test_inactive_and_null_output_keep_existing_contract(self): self.check_case(7)
    def test_native_cells_cursor_clamp_is_unchanged(self): self.check_case(8)
    def test_utf8_text_uses_same_bounded_copy(self): self.check_case(9)
    def test_host_getter_keeps_eight_variable_bound(self): self.check_case(10)


if __name__ == '__main__':
    unittest.main()
