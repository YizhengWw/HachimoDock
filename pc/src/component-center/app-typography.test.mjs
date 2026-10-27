import {test} from "node:test";
import assert from "node:assert/strict";
import {glyphs} from "./app-typography.js";
test("48px numeric cells preserve normalized metrics and do not sample adjacent glyphs",()=>{
  const surface={images:[{id:"font",frame_width:64,frame_height:96}],fonts:[{image:"font",cell:[32,48],chars:"012",metrics:[[18,4,28],[18,4,28],[18,4,28]]}]};
  const map=glyphs(surface,[{id:"font",dataUrl:"fixture"}]);
  assert.deepEqual([...map.values()].map(g=>[g.x,g.y,g.cellWidth,g.cellHeight]),[[0,0,32,48],[32,0,32,48],[0,48,32,48]]);
  assert.equal(map.get("0").metrics[0],18);
  delete surface.fonts[0].cell;
  assert.equal(glyphs(surface,[{id:"font"}]).get("2").y,32);
});
