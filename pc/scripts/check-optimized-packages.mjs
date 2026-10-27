// Disposable QA library only. Never publishes to the user's library or community.
import fs from "node:fs";
import path from "node:path";
import {spawnSync,execFileSync} from "node:child_process";
import {fileURLToPath} from "node:url";
const pc=path.resolve(path.dirname(fileURLToPath(import.meta.url)),"..");
const out=path.resolve(process.argv[2]||"");
const python=process.env.PET_TEST_PYTHON||"python3";
if(!process.argv[2]||!fs.existsSync(path.join(out,"offline-capabilities.json")))throw Error("Provide the isolated QA output directory and explicit offline capability fixture");
const report=[],published=[];
fs.mkdirSync(path.join(out,"archives"),{recursive:true});
for(const id of fs.readdirSync(path.join(out,"packages"))){
  if(!/^[a-z][a-z0-9_-]{0,43}$/.test(id))throw Error("Invalid component ID");
  const source=path.join(out,"packages",id),cap=path.join(out,"offline-capabilities.json");
  for(const test of ["validate_generated_widget.py","smoke_test_widget_game.py"]){
    const r=spawnSync(python,[path.join(pc,"skills/petui/scripts",test),source,"--capabilities",cap],{encoding:"utf8"});
    report.push({id,test,code:r.status,log:r.stdout+r.stderr});
    if(r.status!==0)throw Error(r.stdout+r.stderr);
  }
  const r=spawnSync(python,[path.join(pc,"skills/petui/scripts/publish_generated_widget.py"),source,
    "--pet-home",path.join(out,"isolated-publication-final"),"--source-agent","codex-offline-regression","--capabilities",cap],{encoding:"utf8"});
  if(r.status!==0)throw Error(r.stderr);
  const result=JSON.parse(r.stdout);published.push({id,code:r.status,result});
  const file=path.join(out,"archives",`${id}-${result.componentVersion}-${result.versionHash}.clawpkg`);
  if(fs.existsSync(file))execFileSync("unzip",["-tq",file]);
  else execFileSync("zip",["-qr",file,"."],{cwd:source});
}
fs.writeFileSync(path.join(out,"package-regression.json"),JSON.stringify(report,null,2));
fs.writeFileSync(path.join(out,"publication-regression.json"),JSON.stringify(published,null,2));
console.log(`${report.length} checks passed; ${published.length} isolated atomic publications and versioned archives. No installed library or cloud writes.`);
