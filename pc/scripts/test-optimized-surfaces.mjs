// Local fixtures only: no hardware, media, network services, or real user saves.
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import {chromium} from "playwright";
const output=process.env.PET_UI_REPORT_DIR;
assert(output,"PET_UI_REPORT_DIR required");
fs.mkdirSync(output,{recursive:true});
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER||"/Applications/360Chrome.app/Contents/MacOS/360Chrome",headless:true});
try{
  const page=await browser.newPage({viewport:{width:1400,height:1100}}),errors=[],results=[];
  page.on("pageerror",e=>errors.push(e.message));
  await page.goto(process.env.PET_APP_PREVIEW_URL||"http://127.0.0.1:4198/app-surface-preview.html");
  await page.locator("article").last().waitFor();
  assert.equal(await page.locator("article").count(),22);
  await page.waitForTimeout(700); // WASM initialization, not device I/O.
  for(const card of await page.locator("article").all()){
    const id=await card.getAttribute("data-id"),screen=card.locator(".screen");
    if(!await screen.count()){results.push({id,status:"native-renderer-tested-separately"});continue;}
    assert.equal(await screen.locator('[role="alert"]').count(),0);
    let buttons=0;
    for(const button of await screen.locator('g[role="button"]').all()){
      const label=await button.getAttribute("aria-label");
      const m=await button.evaluate(el=>{
        const hit=el.lastElementChild.getBoundingClientRect(),text=el.querySelector("[data-text-box]");
        if(!text)return {w:hit.width,h:hit.height};
        const b=text.getBoundingClientRect();
        return {w:hit.width,h:hit.height,left:b.left-hit.left,right:hit.right-b.right,top:b.top-hit.top,bottom:hit.bottom-b.bottom};
      });
      assert(m.w>=44&&m.h>=44,`${id}/${label}: small target ${JSON.stringify(m)}`);
      if(m.left!=null)assert(m.left>=4&&m.right>=4&&m.top>=4&&m.bottom>=4,`${id}/${label}: text padding ${JSON.stringify(m)}`);
      buttons++;
    }
    await screen.screenshot({path:path.join(output,`${id}.png`)});
    results.push({id,status:"rendered",buttons});
  }
  const home=page.locator('[data-id="hachimi-home"] .screen');
  assert.equal(await home.getByRole("button",{name:"完成布置",exact:true}).count(),0);
  await home.getByRole("button",{name:"布置小屋",exact:true}).click();
  const plant=home.getByRole("button",{name:"previous",exact:true});
  const before=await plant.locator("image").getAttribute("x");
  // The SVG group's bounds include the off-canvas atlas; click its real hit box.
  await plant.locator(":scope > rect").last().click();
  assert.notEqual(await plant.locator("image").getAttribute("x"),before);
  await home.getByRole("button",{name:"完成布置",exact:true}).click();
  assert.equal(await home.getByRole("button",{name:"完成布置",exact:true}).count(),0);
  const garden=page.locator('[data-id="hachimi-garden"] .screen');
  const time=await garden.textContent();
  await garden.getByRole("button",{name:"+5 分钟",exact:true}).click();
  assert.notEqual(await garden.textContent(),time);
  await garden.getByRole("button",{name:"开始专注",exact:true}).click();
  await garden.getByRole("button",{name:"暂停",exact:true}).click();
  assert.match(await garden.textContent(),/休息一下/);
  const pong=page.locator('[data-id="two-key-pong"] .screen');
  await pong.getByRole("button",{name:"开始接球",exact:true}).click();
  await pong.getByRole("button",{name:"暂停/继续",exact:true}).click();
  assert.match(await pong.textContent(),/已暂停/);
  const ball=pong.locator('[data-content-view="scene"] ellipse');
  const position=await ball.evaluate(e=>[e.getAttribute("cx"),e.getAttribute("cy")]);
  await page.waitForTimeout(650);
  assert.deepEqual(await ball.evaluate(e=>[e.getAttribute("cx"),e.getAttribute("cy")]),position);
  await pong.getByRole("button",{name:"暂停/继续",exact:true}).click();
  assert.doesNotMatch(await pong.textContent(),/已暂停/);
  const radio=page.locator('[data-id="city-radio"] .screen');
  assert.equal(await radio.locator("circle").count(),1,"real media renderer must render transport");
  assert.match(await radio.textContent(),/等待选台|等待收听/);
  assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(output,"report.json"),JSON.stringify({results,errors,homeDecoration:true,gardenControls:true,pongPause:true,mediaRenderer:true},null,2));
  console.log(`PASS: ${results.length} components; ${results.filter(r=>r.status==="rendered").length} rendered, home/decor and garden touch, radio transport, no JS errors.`);
}finally{await browser.close();}
