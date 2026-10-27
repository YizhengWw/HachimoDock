import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import {chromium} from "playwright";
const output=process.env.PET_UI_REPORT_DIR;
assert(output,"PET_UI_REPORT_DIR required");fs.mkdirSync(output,{recursive:true});
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER||"/Applications/360Chrome.app/Contents/MacOS/360Chrome",headless:true});
try{
  const page=await browser.newPage({viewport:{width:1400,height:1100}}),errors=[],rows=[];
  page.on("pageerror",e=>errors.push(e.message));
  await page.goto(process.env.PET_APP_PREVIEW_URL||"http://127.0.0.1:4199/app-surface-preview.html");
  await page.locator("article").last().waitFor();assert.equal(await page.locator("article").count(),22);
  await page.waitForTimeout(400);
  for(const card of await page.locator("article").all()){
    const id=await card.getAttribute("data-id"),screen=card.locator(".screen");
    if(!await screen.count()){rows.push({id,status:"native-pending-hardware"});continue;}
    assert.equal(await screen.locator('[role="alert"]').count(),0,id);
    for(const button of await screen.locator('g[role="button"]').all()){
      const label=await button.getAttribute("aria-label");
      const size=await button.evaluate(el=>{
        const rect=[...el.children].filter(e=>e.tagName.toLowerCase()==="rect").at(-1)||el.querySelector("rect");
        const b=rect?.getBoundingClientRect();return b?{width:b.width,height:b.height}:null;
      });
      if(size)assert(size.width>=44&&size.height>=44,`${id}/${label}: target ${JSON.stringify(size)}`);
    }
    await screen.screenshot({path:path.join(output,`${id}.png`)});rows.push({id,status:"rendered"});
  }
  const home=page.locator('[data-id="hachimi-home"] .screen');
  await home.getByRole("button",{name:"select.plant",exact:true}).locator(":scope > rect").last().click();
  const plant=home.getByRole("button",{name:"select.plant",exact:true}).locator("image");
  const before=await plant.getAttribute("x");
  await home.getByRole("button",{name:"下一个",exact:true}).click();
  assert.notEqual(await plant.getAttribute("x"),before);
  await home.getByRole("button",{name:"上一个",exact:true}).click();
  assert.equal(await plant.getAttribute("x"),before);
  await home.getByRole("button",{name:"完成 · 自动保存",exact:true}).click();
  const garden=page.locator('[data-id="hachimi-garden"] .screen');
  await garden.getByRole("button",{name:"开始专注",exact:true}).click();
  assert.equal(await garden.getByRole("button",{name:"+5 分钟",exact:true}).count(),0);
  const tomato=page.locator('[data-id="tomato-clock"] .screen');
  const start=tomato.getByRole("button",{name:/开始专注|继续专注/,exact:true});
  await start.click();
  await tomato.getByRole("button",{name:"重计时",exact:true}).click();
  assert.equal(await tomato.getByRole("dialog").count(),1);
  await tomato.screenshot({path:path.join(output,"tomato-confirm.png")});
  await tomato.getByRole("button",{name:"取消",exact:true}).click();
  assert.equal(await tomato.getByRole("dialog").count(),0);
  await tomato.getByRole("button",{name:"重计时",exact:true}).click();
  await tomato.getByRole("button",{name:"确认",exact:true}).click();
  assert.equal(await tomato.getByRole("dialog").count(),0);
  for(const id of ["block-combo","snake-turn","two-key-pong"]){
    const screen=page.locator(`[data-id="${id}"] .screen`);
    await screen.getByRole("button",{name:/开始游戏|开始接球/,exact:true}).click();
    await screen.getByRole("button",{name:"暂停/继续",exact:true}).click();
    const scene=screen.locator('[data-content-view]').first();
    const snapshot=await scene.innerHTML();
    await page.waitForTimeout(250);
    assert.equal(await scene.innerHTML(),snapshot,`${id}: paused scene must stay still`);
    await screen.getByRole("button",{name:"继续游戏",exact:true}).click();
    assert.equal(await scene.innerHTML(),snapshot,`${id}: primary resume must not reset game`);
    if(id==="two-key-pong"){
      await screen.scrollIntoViewIfNeeded();const box=await screen.boundingBox();
      await page.mouse.move(box.x+100,box.y+300);await page.mouse.down();
      await page.mouse.move(box.x+340,box.y+300,{steps:8});await page.mouse.up();
      assert.equal(await screen.locator('[data-entity="bat"] rect').getAttribute("x"),"272");
      await screen.getByRole("button",{name:"暂停/继续",exact:true}).click();
      const stopped=await screen.locator('[data-entity="bat"] rect').getAttribute("x");
      await page.mouse.move(box.x+340,box.y+300);await page.mouse.down();await page.mouse.move(box.x+100,box.y+300);await page.mouse.up();
      assert.equal(await screen.locator('[data-entity="bat"] rect').getAttribute("x"),stopped);
    }
  }
  assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(output,"report.json"),JSON.stringify({rows,errors,homeSymmetric:true,disabledControls:true,confirmation:true,primaryResume:true},null,2));
  console.log("PASS 22 component inventory, rendered screens, symmetric decoration, disabled controls, confirm/cancel, resume continuity.");
}finally{await browser.close();}
