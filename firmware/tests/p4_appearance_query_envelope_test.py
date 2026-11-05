"""Actual production query/JSON closure; only OS/storage/broker boundaries adapted.
No hardware, network, Cargo or PIO. Shared parser vectors are emitted by this C.
"""
from pathlib import Path
import hashlib,json,os,re,subprocess
import pytest
ROOT=Path(os.environ.get("PET_APPEARANCE_TEST_ROOT",str(Path(__file__).resolve().parents[1])))
MAIN=Path(os.environ.get("PET_APPEARANCE_TEST_MAIN",str(ROOT/"main")))
FIX=Path(__file__).resolve().parent/"fixtures"
def function(s,name):
 m=re.search(r"(?m)^[A-Za-z_][\w \t*]*\b"+re.escape(name)+r"\s*\(",s);assert m,name
 start=s.index("{",m.end());mask=re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',lambda m:" "*len(m[0]),s,flags=re.S)
 depth=1;i=start+1
 while depth:
  depth+=(mask[i]=="{")-(mask[i]=="}");i+=1
 return s[m.start():i]
@pytest.fixture(scope="module")
def query_binary(tmp_path_factory):
 b=tmp_path_factory.mktemp("appearance-query-c");s=(MAIN/"pet_p4_protocol.c").read_text()
 code=(FIX/"appearance_query_prefix.inc").read_text()
 a=s.index("typedef struct {\n  bool asset_transfer,raw_receiving");z=s.index("static appearance_io_observation_t appearance_io_observe",a)
 code+=s[a:z]
 names=["copy_text","asset_query_snapshot_videos","asset_query_video_proof","json_string","slot_is_valid","pet_p4_appearance_active","pet_p4_appearance_mutation_blocked","send_topic_payload","send_asset_ack_indexed","send_asset_ack","appearance_io_observe","appearance_io_busy","asset_query_refuse","asset_query_modern","asset_query_bound","asset_query_not_proof","send_asset_slot_query_ack"]
 # Negative-control source lacks only the new helpers; keep its real old query.
 for name in names:
  if name.startswith("asset_query_") and name not in ["asset_query_refuse"] and name+"(" not in s:continue
  code+=function(s,name)+"\n"
 code+=(FIX/"appearance_query_suffix.inc").read_text();(b/"query.c").write_text(code)
 argv=["/usr/bin/cc","-std=gnu11","-O1","-g","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-Wno-deprecated-declarations","-fsanitize=address,undefined","-fno-omit-frame-pointer","-I",str(ROOT/"main"),"-I",str(ROOT/"tests/vendor/cjson"),str(b/"query.c"),str(ROOT/"tests/vendor/cjson/cJSON.c"),"-o",str(b/"query")]
 (b/"invocation.json").write_text(json.dumps({"argv":argv,"cwd":str(b),"sourceSHA256":hashlib.sha256((MAIN/"pet_p4_protocol.c").read_bytes()).hexdigest()},indent=2))
 r=subprocess.run(argv,cwd=b,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60);(b/"compile.log").write_text(r.stdout);assert r.returncode==0,r.stdout
 return b/"query"
CASES=["operation","owner","transfer","storage","unknown","unknown-operation","distinct-tokens","board","boot","nonce","id","duplicate","partial","schema","legacy","identity-snapshot","idle","callback-refusal","storage-unavailable","bad-transfer","missing-transfer","duplicate-board","duplicate-boot","duplicate-schema","duplicate-transfer"]+[f"oom-{n}" for n in range(30)]
@pytest.mark.parametrize("case",CASES)
def test_real_query_binding_classification_and_no_busy_storage(query_binary,tmp_path,case):
 argv=[str(query_binary),case];env={"HOME":str(tmp_path),"TMPDIR":str(tmp_path),"PATH":"/usr/bin:/bin","ASAN_OPTIONS":"detect_leaks=0"}
 (tmp_path/"invocation.json").write_text(json.dumps({"argv":argv,"cwd":str(tmp_path),"env":env},indent=2))
 r=subprocess.run(argv,cwd=tmp_path,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=15);(tmp_path/"output.log").write_text(r.stdout);assert r.returncode==0,r.stdout
 if r.stdout.startswith("{"):
  got=json.loads(r.stdout);assert got["phase"]=="slot-query"
  fixture=Path(os.environ.get("PET_APPEARANCE_QUERY_VECTORS",str(ROOT.parent/"pc/src-tauri/test-fixtures/appearance-query-envelope-v1.json")))
  if fixture.exists():
   rows=json.loads(fixture.read_text())["vectors"]
   row=next(v for v in rows if v["name"]==case);assert got==row["ack"]
