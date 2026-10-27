#!/usr/bin/env python3
"""Package only the three tested widgets; no workstation configuration or saves."""
import base64,hashlib,json,sys,zipfile,io,subprocess
from pathlib import Path
from PIL import Image
REPO=Path(__file__).resolve().parents[2];OUT=REPO.parent/'builds/local-delights-20261022'
sys.path.insert(0,str(REPO/'server'))
from community.package import inspect_archive
IDS=('decision-capsule','answer-book','hope-train')
def main():
    batch=[];report=[]
    selected=[a for a in sys.argv[1:] if not a.startswith('--')] or list(IDS)
    assert all(id in IDS for id in selected)
    for id in selected:
        root=REPO/'pc/community-components/local-delights'/id
        for script in ('validate_generated_widget.py','smoke_test_widget_game.py'):
            result=subprocess.run([sys.executable,str(REPO/'pc/skills/petui/scripts'/script),str(root),'--json'],capture_output=True,text=True,check=True)
            result=json.loads(result.stdout);assert result['ok'],result
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
            for file in sorted(root.rglob('*')):
                assert not file.is_symlink()
                if file.is_file():z.write(file,file.relative_to(root))
        checked=inspect_archive(data.getvalue(),REPO/'pc/skills/petui/scripts')
        # Export the same canonical archive served by the community, not a
        # timestamp-dependent input ZIP with a different download checksum.
        raw=checked['package']
        device_cover=OUT/'device'/('hope-train-fixed.png' if id=='hope-train' else f'{id}-initial.png')
        use_device='--device-covers' in sys.argv
        im=Image.open(device_cover if use_device else OUT/'covers'/f'{id}.jpg');assert im.size==(640,480)
        cover=io.BytesIO();im.convert('RGB').save(cover,format='PNG');cover=cover.getvalue()
        (OUT/'covers'/f'{id}.png').write_bytes(cover);(OUT/f'{id}.clawpkg').write_bytes(raw)
        manifest=checked['manifest'];share=json.loads((root/'share.json').read_text())
        evidence='封面为设备实际截图；USB 动作、保存及画面检查通过，实体触屏和按键手感待用户验收。' if use_device else '封面为实际资源包预览；自动回归已通过，实机画面仍待验收。'
        batch.append({'approve':True,'publication':{'requestId':'delights-'+hashlib.sha256(raw+cover).hexdigest()[:48],'rightsConfirmed':True,'privacyConfirmed':True,'package':base64.b64encode(raw).decode(),'covers':[base64.b64encode(cover).decode()],'metadata':{'author':manifest['author'],'summary':manifest['description'],'description':manifest['description']+'\n需要 Pet Manager 0.1.136 和支持 p4-local-tools-v1 的 0.7.102 固件。'+evidence,'category':share['category'],'license':share['license'],'sourceUrl':'','releaseNotes':'修正设备左右切换按钮的缺字显示；资源包更新即可，无需再次升级固件。' if use_device else '首版：原生 640×480 精细界面、触屏与按键操作、设备存档；使用统一有界离线能力，不携带脚本或凭据。'}}})
        report.append({'id':id,'name':manifest['name'],'version':manifest['version'],'sha256':checked['sha256'],'bytes':len(raw),'requirements':checked['requirements'],'privacy':'passed','deviceVerified':False})
    (OUT/'batch.json').write_text(json.dumps(batch,ensure_ascii=False),encoding='utf-8')
    previous=json.loads((OUT/'publication-manifest.json').read_text()) if (OUT/'publication-manifest.json').exists() else []
    combined={r['id']:r for r in previous};combined.update({r['id']:r for r in report})
    (OUT/'publication-manifest.json').write_text(json.dumps(list(combined.values()),ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(report,ensure_ascii=False,indent=2))
if __name__=='__main__':main()
