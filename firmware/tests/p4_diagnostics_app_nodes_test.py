"""Real diagnostics appNodes block and cJSON wire roundtrip, quota v2 and v3."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MASKS = (0, (1 << 0) | (1 << 63), (1 << 64) - 1, 1 << 32, (1 << 53) | 1)


def check_diagnostics_all_nodes_and_exact_mask(tmp_path, v3):
    source = (ROOT / "main/pet_p4_diagnostics.c").read_text()
    # Execute the production block verbatim, including its guards and field names.
    start = source.index('      cJSON *nodes=cJSON_AddObjectToObject(p,"appNodes");')
    end = source.index("\n#endif", start)
    block = source[start:end]
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "cJSON.h"
#include "pet_p4_app_draw_trace.h"

typedef struct { pet_app_draw_trace_t app_nodes; } metrics_t;
static void append_nodes(cJSON *p, const metrics_t *m) {
''' + block + r'''
}
static const cJSON *number(const cJSON *object, const char *key) {
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(object,key);
    assert(cJSON_IsNumber(value));
    return value;
}
int main(void) {
    const uint64_t masks[]={0,UINT64_C(1)|(UINT64_C(1)<<63),UINT64_MAX,
        UINT64_C(1)<<32,(UINT64_C(1)<<53)|UINT64_C(1)};
    for(unsigned test=0;test<sizeof(masks)/sizeof(masks[0]);test++) {
        metrics_t metrics={0};pet_app_draw_trace_t *trace=&metrics.app_nodes;
        trace->nodes_mask=masks[test];
        for(unsigned i=0;i<PET_APP_TRACE_NODES;i++)trace->nodes_us[i]=1000U+i;
        trace->nodes_us[PET_APP_TRACE_NODES-1]=UINT32_MAX;
        trace->images_us=11;trace->key_us=12;trace->cache_copy_us=13;trace->prefix_us=14;
        trace->clear_us=15;trace->remember_us=16;
        trace->cache_hit=1;trace->cache_deferred=1;trace->invalid_clock=17;
        cJSON *root=cJSON_CreateObject();assert(root);
        append_nodes(root,&metrics);
        /* The legacy number follows the library's existing print precision. */
        cJSON *legacy=cJSON_CreateNumber(trace->nodes_mask);assert(legacy);
        char *legacy_wire=cJSON_PrintUnformatted(legacy);assert(legacy_wire);
        cJSON *legacy_decoded=cJSON_Parse(legacy_wire);assert(legacy_decoded);
        char *wire=cJSON_PrintUnformatted(root);assert(wire);
        /* Reparse serialized bytes with the real library, not a cJSON double. */
        cJSON *decoded=cJSON_Parse(wire);assert(decoded);
        const cJSON *nodes=cJSON_GetObjectItemCaseSensitive(decoded,"appNodes");
        assert(cJSON_IsObject(nodes));
        const cJSON *costs=cJSON_GetObjectItemCaseSensitive(nodes,"nodeUs");
        assert(cJSON_IsArray(costs)&&cJSON_GetArraySize(costs)==PET_APP_TRACE_NODES);
        for(unsigned i=0;i<PET_APP_TRACE_NODES;i++) {
            const cJSON *value=cJSON_GetArrayItem(costs,(int)i);
            assert(cJSON_IsNumber(value)&&value->valuedouble==(double)trace->nodes_us[i]);
        }
        double low=number(nodes,"maskLow")->valuedouble;
        double high=number(nodes,"maskHigh")->valuedouble;
        assert(low>=0&&low<=UINT32_MAX&&high>=0&&high<=UINT32_MAX);
        assert(low==(double)(uint32_t)trace->nodes_mask);
        assert(high==(double)(uint32_t)(trace->nodes_mask>>32));
        uint64_t restored=(uint64_t)(uint32_t)low|((uint64_t)(uint32_t)high<<32);
        assert(restored==trace->nodes_mask);
        assert(number(nodes,"mask")->valuedouble==legacy_decoded->valuedouble);
        puts(wire);
        cJSON_Delete(legacy_decoded);cJSON_free(legacy_wire);cJSON_Delete(legacy);
        cJSON_Delete(decoded);cJSON_free(wire);cJSON_Delete(root);
    }
    return 0;
}
'''
    src = tmp_path / "diagnostics-nodes.c"
    binary = tmp_path / "diagnostics-nodes"
    src.write_text(code)
    subprocess.run([
        # Vendored cJSON uses sprintf, deprecated by recent macOS SDK headers.
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
        "-fsanitize=address,undefined", f"-DPET_P4_QUOTA_V3={v3}",
        "-DPET_P4_APP_NODE_TRACE=1", "-DPET_P4_APP_PHASE_TRACE=1",
        "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/vendor/cjson"),
        str(src), str(ROOT / "tests/vendor/cjson/cJSON.c"), "-lm", "-o", str(binary),
    ], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    records = [json.loads(line)["appNodes"] for line in result.stdout.splitlines()]
    assert len(records) == len(MASKS)
    count = 64 if v3 else 48
    for mask, nodes in zip(MASKS, records):
        expected = list(range(1000, 1000 + count))
        expected[-1] = (1 << 32) - 1
        assert nodes["nodeUs"] == expected
        assert nodes["nodeUs"][47] == expected[47]
        if v3:
            assert nodes["nodeUs"][63] == (1 << 32) - 1
        assert 0 <= nodes["maskLow"] <= (1 << 32) - 1
        assert 0 <= nodes["maskHigh"] <= (1 << 32) - 1
        assert nodes["maskLow"] | (nodes["maskHigh"] << 32) == mask
        assert isinstance(nodes["mask"], (int, float))  # retain legacy number type
        if mask <= 1 << 32:
            assert nodes["mask"] == mask
        assert {key: nodes[key] for key in (
            "imagesUs", "keyUs", "cacheCopyUs", "prefixUs", "clearUs", "rememberUs",
            "cacheHit", "cacheDeferred", "invalidClock",
        )} == dict(imagesUs=11, keyUs=12, cacheCopyUs=13, prefixUs=14, clearUs=15,
                  rememberUs=16, cacheHit=1, cacheDeferred=1, invalidClock=17)
    assert records[1]["maskLow"] == 1 and records[1]["maskHigh"] == 0x80000000
    assert int(records[1]["mask"]) != MASKS[1]  # legacy number really loses bit 0


class DiagnosticsAppNodesTest(unittest.TestCase):
    def test_quota_v2_48_nodes_and_exact_mask(self):
        with tempfile.TemporaryDirectory() as tmp:
            check_diagnostics_all_nodes_and_exact_mask(Path(tmp), 0)

    def test_quota_v3_64_nodes_and_exact_mask(self):
        with tempfile.TemporaryDirectory() as tmp:
            check_diagnostics_all_nodes_and_exact_mask(Path(tmp), 1)


if __name__ == "__main__":
    unittest.main()
