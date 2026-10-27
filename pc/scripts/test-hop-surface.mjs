import assert from 'node:assert/strict';import {chromium} from 'playwright';
const b=await chromium.launch({executablePath:'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
 const p=await b.newPage({viewport:{width:960,height:900}}),errors=[];p.on('pageerror',e=>errors.push(e.message));
 await p.goto('http://127.0.0.1:4203/new-components-preview.html?component=hop');
 const screen=p.locator('.screen');await screen.getByRole('button',{name:'按住开始',exact:true}).waitFor();
 const hit=await screen.getByRole('button',{name:'按住开始',exact:true}).boundingBox();
 const clickAt=async()=>{await p.mouse.move(hit.x+hit.width/2,hit.y+hit.height/2);await p.mouse.down();};
 await clickAt();await p.waitForTimeout(700);await p.mouse.up();await p.waitForTimeout(1000);
 const labels=()=>screen.locator('[data-label]').evaluateAll(a=>a.map(e=>e.getAttribute('data-label')));
 assert((await labels()).includes('1'),'pointer down/up must launch and score');
 await screen.screenshot({path:'new-components/qa/hop-playing.png'});
 await clickAt();await p.waitForTimeout(400);await p.mouse.move(hit.x-20,hit.y);await p.mouse.up();
 const position=await screen.locator('[data-content-view] image').getAttribute('x');await p.waitForTimeout(900);assert.equal(await screen.locator('[data-content-view] image').getAttribute('x'),position,'drag out cancels charge');
 await screen.getByRole('button',{name:'暂停',exact:true}).click();assert((await labels()).includes('歇一下，云朵会等你'));
 await screen.getByRole('button',{name:'继续',exact:true}).click();assert((await labels()).includes('松开，出发！'));
 const key=screen.getByRole('button',{name:'按住蓄力',exact:true});await key.focus();await p.keyboard.down('Space');await p.waitForTimeout(700);await p.keyboard.up('Space');await p.waitForTimeout(1000);
 assert((await labels()).includes('2'),'keyboard hold should work');
 for(const el of await screen.getByRole('button').all()){const r=await el.boundingBox();assert(r.width>=44&&r.height>=44);}
 assert.deepEqual(errors,[]);console.log('PASS actual UI: pointer hold/release, score, cancel outside, pause/resume, keyboard hold, touch targets, no JS exceptions.');
}finally{await b.close();}
