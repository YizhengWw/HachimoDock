/** Build-time only, pinned MIT calendar library. No executable code enters a package. */
import { Solar } from 'lunar-javascript';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output=process.argv[2];
if(!output)throw Error('Usage: node scripts/build-almanac-data.mjs <empty-component-dir>');
const pack=path.resolve(output);
const content=[];
for(let at=new Date('2020-01-01T00:00:00Z');at<new Date('2036-01-01T00:00:00Z');at.setUTCDate(at.getUTCDate()+1)){
  const solar=Solar.fromYmd(at.getUTCFullYear(),at.getUTCMonth()+1,at.getUTCDate()),lunar=solar.getLunar();
  content.push({date:at.toISOString().slice(0,10),lunar:`${lunar.getMonthInChinese()}月${lunar.getDayInChinese()}`,
    ganzhi:`${lunar.getYearInGanZhi()}年 · ${lunar.getMonthInGanZhi()}月 · ${lunar.getDayInGanZhi()}日`,
    term:lunar.getJieQi(),festivals:[...solar.getFestivals(),...lunar.getFestivals()],yi:lunar.getDayYi(),ji:lunar.getDayJi()});
}
// Independent anchors from HKO annual Gregorian-Lunar concordances. Not the
// upstream library's own output copied into a test as an expected value.
for(const [date,lunar,term] of [
  ['2024-02-10','正月初一',''],['2025-01-29','正月初一',''],
  ['2025-07-25','闰六月初一',''],['2026-02-17','正月初一',''],
  ['2026-06-19','五月初五',''],['2026-09-25','八月十五',''],
  ['2026-04-05','二月十八','清明'],['2026-12-22','冬月十四','冬至'],
]){
  const row=content.find(c=>c.date===date);assert.equal(row.lunar,lunar,date);assert.equal(row.term,term,date);
}
assert.equal(content.length,5844);
for(let i=0;i<content.length;i++){
  const r=content[i];if(i)assert.equal(Date.parse(r.date)-Date.parse(content[i-1].date),86400000);
  for(const k of ['festivals','yi','ji'])assert(r[k].length<=32&&r[k].every(s=>s.length>0&&Buffer.byteLength(s)<=90));
}
const declaration={schemaVersion:1,engine:'pc-app-calendar-v1',service:'calendar',actions:{'host.today':'today','host.details':'details','host.previous':'previous','host.next':'next'},content};
const bytes=JSON.stringify(declaration);assert(Buffer.byteLength(bytes)<=4*1024*1024);
mkdirSync(path.join(pack,'pc'),{recursive:true});mkdirSync(path.join(pack,'licenses'),{recursive:true});
writeFileSync(path.join(pack,'pc/application.json'),bytes+'\n');
writeFileSync(path.join(pack,'licenses/lunar-javascript-MIT.txt'),readFileSync(path.join(root,'node_modules/lunar-javascript/LICENSE')));
writeFileSync(path.join(pack,'licenses/calendar-sources.txt'),[
  'Generated data: lunar-javascript 1.7.7, pinned npm integrity in package-lock.json.',
  'Source: https://www.npmjs.com/package/lunar-javascript/v/1.7.7 (MIT; see bundled license).',
  'Independent civil-calendar anchors: https://www.hko.gov.hk/sc/gts/time/conversion.htm',
  'Annual tables: https://www.hko.gov.hk/tc/gts/time/calendar/pdf/files/2024.pdf',
  'https://www.hko.gov.hk/tc/gts/time/calendar/pdf/files/2025.pdf',
  'https://www.hko.gov.hk/tc/gts/time/calendar/pdf/files/2026.pdf',
  'Timezone: Asia/Shanghai; coverage 2020-01-01 through 2035-12-31 inclusive.',
  '传统宜忌为民俗文化资料，不是科学预测、医疗或投资建议。节日不等于法定休假安排。',
].join('\n')+'\n');
console.log(JSON.stringify({records:content.length,bytes:Buffer.byteLength(bytes),independentAnchors:8,first:content[0].date,last:content.at(-1).date}));
