"""Reviewed static UI paths, not whole-program worst-case stack analysis.

SU frames include inlined locals. Missing optional helpers can be inlined, but
external entry points/reports are required. Vendor/libc/ISR stacks and unlisted
call edges still need physical high-water and contention acceptance. No change
to task sizes, scheduling, device data or any audio path.
"""
from pathlib import Path
import hashlib

BUDGET = 4096
MODULES = ('pet_p4_renderer', 'pet_p4_miniapp', 'pet_p4_app_host', 'pet_p4_form_v2', 'pet_p4_surface', 'pet_p4_host_collection')
R, M, H, F, S, C = MODULES
ROOT = ((R, 'pet_p4_renderer_render'), (R, 'render_miniapp_page'))
APP = ROOT + ((R, 'render_package_app'),)
SNAPSHOT = ROOT + ((M, 'pet_p4_miniapp_render_snapshot'),)
FORM = SNAPSHOT + ((M, 'app_surface_locked'),)
LABEL = APP + ((R, 'draw_app_label'),)
DATA_LABEL = APP + ((R, 'draw_app_data'), (R, 'app_data_label'), (R, 'draw_app_label'))
PATHS = {
    # New review paths: per-node raster scratch and visibility projection must
    # remain inside the SAME 4096-byte reviewed path budget.
    'app_panel_gradient': APP + ((R, 'draw_app_panel'), (R, 'cold_fast_panel_interior'), (R, 'blend_rgb565')),
    'app_cache_visibility': APP + ((R, 'pet_app_frame_matches'), (R, 'pet_app_frame_content_matches'),
        (R, 'pet_app_frame_eligible_at'), (S, 'pet_surface_node_viewport'), (S, 'pet_surface_project'), (S, 'pet_surface_clamp_offset')),
    'clip_frame_visibility': ROOT + ((R, 'renderer_component_media_frame'),
        (S, 'pet_surface_node_viewport'), (S, 'pet_surface_project'), (S, 'pet_surface_clamp_offset')),
    'media_text': ROOT + ((R, 'render_media_page'), (R, 'render_package_media'), (R, 'draw_text_line')),
    'app_label_width': LABEL + ((R, 'app_text_width'), (R, 'app_glyph_width')),
    'app_label_icon': LABEL + ((R, 'draw_app_icon'),),
    'data_label_width': DATA_LABEL + ((R, 'app_text_width'), (R, 'app_glyph_width')),
    'data_label_icon': DATA_LABEL + ((R, 'draw_app_icon'),),
    'snapshot_host': SNAPSHOT + ((H, 'pet_app_host_render_capture'), (H, 'render_view_locked')),
    'snapshot_form_text': FORM + ((F, 'pet_form2_text'), (F, 'state_valid'), (F, 'shape')),
    'snapshot_form_values': FORM + ((M, 'form2_values'),),
    'snapshot_form_availability': FORM + ((M, 'runtime_action_enabled'), (F, 'pet_form2_allowed'), (F, 'state_valid'), (F, 'shape')),
    'snapshot_host_collection': FORM + ((S, 'pet_surface_resolve_collection'), (C, 'pet_host_collection_matches')),
    'render_metrics': ((R, 'pet_p4_renderer_render'), (R, 'record_render_performance')),
    'async_asset_pack': ((R,'pet_p4_renderer_render'),(R,'render_asset_pet_frame'),(R,'render_h264_asset_frame_async'),(R,'pack_h264_i420_for_ppa')),
    'async_asset_packed_fallback': ((R,'pet_p4_renderer_render'),(R,'render_asset_pet_frame'),(R,'render_h264_asset_frame_async'),(R,'scale_h264_packed_cpu')),
    'async_asset_i420_fallback': ((R,'pet_p4_renderer_render'),(R,'render_asset_pet_frame'),(R,'render_h264_asset_frame_async'),(R,'scale_h264_frame_cpu')),
}
REQUIRED = ((R, 'pet_p4_renderer_render'), (M, 'pet_p4_miniapp_render_snapshot'),
            (H, 'pet_app_host_render_capture'), (F, 'pet_form2_text'))


def parse_frames(text):
    frames = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        parts = line.split('\t')
        if len(parts) != 3:
            raise ValueError('stack_report_shape')
        name = parts[0].rsplit(':', 1)[-1].split('.', 1)[0]
        size = int(parts[1])
        if not name or not name.replace('_', '').isalnum() or size < 0 or size > 1048576:
            raise ValueError('stack_report_frame')
        old = frames.get(name)
        # A dynamic clone cannot be hidden by a smaller static clone.
        frames[name] = (max(size, old[0] if old else 0),
                        parts[2] == 'static' and (old[1] if old else True))
    return frames


def check_frames(reports, elf_sha256):
    if set(reports) != set(MODULES) or len(elf_sha256) != 64 or any(c not in '0123456789abcdef' for c in elf_sha256):
        raise ValueError('stack_report_inputs')
    frames = {module: parse_frames(text) for module, text in reports.items()}
    if any(name not in frames[module] for module, name in REQUIRED):
        raise ValueError('stack_report_entry_missing')
    paths = []
    for identifier, chain in PATHS.items():
        entries = []
        for module, name in chain:
            frame = frames[module].get(name)
            if frame is not None and not frame[1]:
                raise ValueError('stack_path_dynamic')
            entries.append({'module': module, 'function': name, 'bytes': frame[0] if frame else 0,
                            'emitted': frame is not None})
        size = sum(row['bytes'] for row in entries)
        if size > BUDGET:
            raise ValueError('stack_path_budget')
        paths.append({'id': identifier, 'bytes': size, 'frames': entries})
    return {'schema': 4, 'engine': 'p4-render-stack-v4', 'elfSha256': elf_sha256,
            'budgetBytes': BUDGET, 'allCallEdges': False, 'paths': paths,
            'maxReviewedBytes': max(row['bytes'] for row in paths)}


def check_build(directory, elf):
    directory, elf = Path(directory), Path(elf)
    reports = {module: (directory / 'main' / (module + '.c.su')).read_text(encoding='utf-8')
               for module in MODULES}
    return check_frames(reports, hashlib.sha256(elf.read_bytes()).hexdigest())
