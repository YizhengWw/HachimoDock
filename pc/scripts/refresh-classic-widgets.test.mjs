import {test} from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import {initialApp,appAction,appAdvance,appDashboard,appHit,appPage} from "../src/component-center/app-runtime.js";
const root=process.env.PET_APP_PACKAGE_DIR;
const load=id=>JSON.parse(fs.readFileSync(path.join(root,id,"runtime/widget.json"),"utf8"));
test("hydration records only acknowledgements; page cycling and pause remain real",{skip:!root},()=>{
  const r=load("drink-reminder");
  let s=initialApp(r);
  s=appAction(r,s,"reminder.pause_resume");s=appAdvance(r,s,60000);
  assert.equal(s.vars.remaining_min,60);assert.equal(s.vars.drink_count_today,0);
  assert.equal(appDashboard(r,s).metricUnit,"继续提醒");
  s=appAction(r,s,"reminder.pause_resume");
  for(let n=0;n<60;n++)s=appAdvance(r,s,60000);
  assert.equal(s.state,"due");assert.equal(s.vars.drink_count_today,0);
  assert.equal(appHit(r.surface,appPage(r,s),90,400,s.vars),"reminder.acknowledge");
  s=appAction(r,s,"reminder.acknowledge");
  assert.equal(s.vars.drink_count_today,1);assert.equal(s.vars.remaining_min,60);
  s=appAction(r,s,"reminder.switch_view");assert.equal(s.page,"records");
  assert.equal(appDashboard(r,s).metricValue,"1");
  s=appAction(r,s,"reminder.switch_view");assert.equal(s.page,"countdown");
});
test("pomodoro skip isn't completion; pause and real completion behave correctly",{skip:!root},()=>{
  const r=load("tomato-clock");let s=initialApp(r);
  s=appAction(r,s,"tomato.skip_phase");assert.equal(s.vars.completed_sessions,0);
  s=appAction(r,s,"tomato.skip_phase");assert.equal(s.state,"focus");
  s=appAction(r,s,"tomato.start_pause");s=appAdvance(r,s,1000);assert.equal(s.vars.remaining_s,1500);
  s=appAction(r,s,"tomato.start_pause");
  for(let n=0;n<1500;n++)s=appAdvance(r,s,1000);
  assert.equal(s.state,"rest");assert.equal(s.vars.completed_sessions,1);
  assert.ok(s.vars.remaining_s>=299&&s.vars.remaining_s<=300);
});
test("token pages preserve bridge variables and honest empty state",{skip:!root},()=>{
  const r=load("token-usage");let s=initialApp(r);
  assert.equal(appDashboard(r,s).metricValue,"—");
  Object.assign(s.vars,{total_display:"120",input_display:"80",output_display:"40"});
  for(const [action,expected] of [["stats.show_total","120"],["stats.show_input","80"],["stats.show_output","40"]]){
    s=appAction(r,s,action);assert.equal(appDashboard(r,s).metricValue,expected);
  }
});
