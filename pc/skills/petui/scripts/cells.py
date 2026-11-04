"""p4-cells-v1: pure grid model shared by the validator, the preview runtime and tests.

Mirrors firmware/main/pet_p4_cells.c and the cells parser in pet_p4_miniapp.c. No IO, no device access.
Rows are strings of lowercase hex digits (one per column); out-of-range reads give -1, out-of-range
writes are no-ops and stored values clamp to 0..2^bits-1.
"""
import re

ENGINE = 'p4-cells-v1'
MAX_SIDE = 16
MAX_CELLS = 256
BITS = (2, 4)
MAX_OPS = 4
OUT_OF_RANGE = -1
# Exact key sets of the bounded ops (firmware parse_cell_ops).
OP_KEYS = {'cell': {'op', 'x', 'y', 'set'}, 'cellfill': {'op', 'set'}, 'cellcount': {'op', 'value', 'var'}, 'cellrun': {'op', 'x', 'y', 'value', 'var'}}
WRITE_OPS = ('cell', 'cellfill')
EXCLUSIVE = ('game', 'scene', 'local', 'media', 'instrument', 'data', 'host')
_HEX = re.compile(r'[0-9a-fA-F]+')


def max_value(bits):
    return (1 << bits) - 1


def grid(runtime):
    """(cols, rows, bits) of a structurally valid declaration, else None."""
    c = runtime.get('cells') if isinstance(runtime, dict) else None
    if not isinstance(c, dict):
        return None
    cols, rows, bits = c.get('cols'), c.get('rows'), c.get('bits')
    if any(type(v) is not int for v in (cols, rows, bits)):
        return None
    if not 1 <= cols <= MAX_SIDE or not 1 <= rows <= MAX_SIDE or cols * rows > MAX_CELLS or bits not in BITS:
        return None
    return cols, rows, bits


def valid_row(row, cols, bits):
    return isinstance(row, str) and len(row) == cols and _HEX.fullmatch(row) is not None and all(int(ch, 16) <= max_value(bits) for ch in row)


def schema_errors(runtime):
    """Shape of the root `cells` object and its exclusivity; capability and ops are checked by the caller."""
    errors = []
    c = runtime.get('cells')
    if not isinstance(c, dict) or not set(c) <= {'engine', 'cols', 'rows', 'bits', 'init'} or not {'engine', 'cols', 'rows', 'bits'} <= set(c):
        errors.append(f'runtime/widget.json.cells 只接受 engine/cols/rows/bits 与可选 init')
        return errors
    if c.get('engine') != ENGINE:
        errors.append(f'runtime/widget.json.cells.engine 只接受 {ENGINE}')
    shape = grid(runtime)
    if shape is None:
        errors.append(f'runtime/widget.json.cells 需要 cols/rows 为 1-{MAX_SIDE} 的整数（cols*rows <= {MAX_CELLS}）且 bits 为 2 或 4')
    elif 'init' in c:
        cols, rows, bits = shape
        init = c['init']
        if not isinstance(init, list) or len(init) != rows:
            errors.append('runtime/widget.json.cells.init 必须是每行一个字符串、行数等于 rows 的数组')
        elif not all(valid_row(row, cols, bits) for row in init):
            errors.append(f'runtime/widget.json.cells.init 每行必须恰好 cols 个十六进制数字且每个值 < 2^bits')
    if runtime.get('engine') != 'p4-bounded-runtime-v4':
        errors.append('runtime/widget.json.cells 需要 engine=p4-bounded-runtime-v4')
    if any(k in runtime for k in EXCLUSIVE):
        errors.append('cells 不与 game/scene/local/media/instrument/data/host 混用')
    return errors


def initial_rows(runtime):
    """Row strings from init (lowercased) or an all-zero grid."""
    shape = grid(runtime)
    if shape is None:
        return []
    cols, rows, _ = shape
    init = runtime['cells'].get('init')
    if isinstance(init, list) and len(init) == rows:
        return [str(row).lower() for row in init]
    return ['0' * cols] * rows


def in_range(rows, x, y):
    return isinstance(x, int) and isinstance(y, int) and 0 <= y < len(rows) and 0 <= x < len(rows[y])


def get(rows, x, y):
    return int(rows[y][x], 16) if in_range(rows, x, y) else OUT_OF_RANGE


def clamp(value, bits):
    return max(0, min(max_value(bits), int(value)))


def set_cell(rows, x, y, value, bits):
    """Store a clamped value; out-of-range coordinates are a silent no-op. Returns whether anything changed."""
    if not in_range(rows, x, y):
        return False
    digit = '%x' % clamp(value, bits)
    if rows[y][x] == digit:
        return False
    rows[y] = rows[y][:x] + digit + rows[y][x + 1:]
    return True


def fill(rows, value, bits):
    digit = '%x' % clamp(value, bits)
    changed = any(ch != digit for row in rows for ch in row)
    for y in range(len(rows)):
        rows[y] = digit * len(rows[y])
    return changed


def count(rows, value):
    if not isinstance(value, int) or value < 0:
        return 0
    return sum(1 for row in rows for ch in row if int(ch, 16) == value)


def run(rows, x, y, value):
    """Longest straight run of `value` through (x, y) over the 4 line directions; 0 unless the cell holds value."""
    if not isinstance(value, int) or value < 0 or get(rows, x, y) != value:
        return 0
    best = 0
    for dx, dy in ((1, 0), (0, 1), (1, 1), (1, -1)):
        length = 1
        for sign in (-1, 1):
            cx, cy = x, y
            for _ in range(MAX_SIDE):
                cx += sign * dx
                cy += sign * dy
                if get(rows, cx, cy) != value:
                    break
                length += 1
        best = max(best, length)
    return best


def _arg(value, variables):
    return int(variables.get(value['var'], 0)) if isinstance(value, dict) else int(value)


def apply_ops(ops, rows, variables, limits, bits):
    """Run a branch's cells ops in order against the current variables; count/run outputs clamp to limits."""
    for op in ops or []:
        kind = op.get('op')
        if kind == 'cell':
            set_cell(rows, _arg(op['x'], variables), _arg(op['y'], variables), _arg(op['set'], variables), bits)
        elif kind == 'cellfill':
            fill(rows, _arg(op['set'], variables), bits)
        elif kind in ('cellcount', 'cellrun'):
            value = _arg(op['value'], variables)
            result = count(rows, value) if kind == 'cellcount' else run(rows, _arg(op['x'], variables), _arg(op['y'], variables), value)
            lo, hi = limits.get(op['var'], (-10**9, 10**9))
            variables[op['var']] = max(lo, min(hi, result))


def arg_ok(value, lo, hi, variables):
    """A literal within lo..hi or {"var": "<int var>"} (firmware parse_cell_arg)."""
    if isinstance(value, dict):
        spec = variables.get(value.get('var')) if set(value) == {'var'} and isinstance(value.get('var'), str) else None
        return isinstance(spec, dict) and spec.get('type') == 'int'
    return type(value) is int and lo <= value <= hi


def op_errors(ops, runtime, where, *, program_owned=False):
    """Schema of one `cells` op array (transition, else, tick or tick.then)."""
    shape = grid(runtime)
    variables = runtime.get('vars') if isinstance(runtime.get('vars'), dict) else {}
    if shape is None:
        return [f'{where} 的 cells 操作需要已声明且有效的 cells 网格']
    cols, rows, bits = shape
    if not isinstance(ops, list) or not 1 <= len(ops) <= MAX_OPS:
        return [f'{where}.cells 必须是 1-{MAX_OPS} 个操作']
    errors = []
    for index, op in enumerate(ops):
        at = f'{where}.cells[{index}]'
        kind = op.get('op') if isinstance(op, dict) else None
        if kind not in OP_KEYS or set(op) != OP_KEYS[kind]:
            errors.append(f'{at} 只接受 cell{{x,y,set}} / cellfill{{set}} / cellcount{{value,var}} / cellrun{{x,y,value,var}}')
            continue
        if kind in WRITE_OPS and program_owned:
            errors.append(f'{at} 组件程序拥有网格写入，transition/tick 只能读取（cellcount/cellrun/条件）')
        value_key = 'set' if kind in WRITE_OPS else 'value'
        if not arg_ok(op[value_key], 0, max_value(bits), variables):
            errors.append(f'{at}.{value_key} 必须是 0..{max_value(bits)} 的整数或 {{var}} 整数变量')
        if 'x' in op and not (arg_ok(op['x'], 0, cols - 1, variables) and arg_ok(op['y'], 0, rows - 1, variables)):
            errors.append(f'{at} 的 x/y 必须在网格范围内或为 {{var}} 整数变量')
        if 'var' in op:
            spec = variables.get(op['var']) if isinstance(op['var'], str) else None
            if not isinstance(spec, dict) or spec.get('type') != 'int':
                errors.append(f'{at}.var 必须引用 int 变量')
    return errors
