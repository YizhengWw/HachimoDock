import test from 'node:test';
import assert from 'node:assert/strict';
import {bridgeEventArguments} from './service-bridge-controls-model.js';
const seek={arguments:{percent:{type:'int',value:{event:'position'},min:0,max:100}}};
test('bridge PC controls preserve integer and boolean typing without adding paths or constant arguments',()=>{
 assert.deepEqual(bridgeEventArguments(seek,{position:'42',paths:'/private/not-an-argument'}),{position:42});
 assert.deepEqual(bridgeEventArguments({arguments:{mode:{type:'string',value:'shuffle'},key:{type:'string',value:{selection:'queue'}}}}),{});
 assert.deepEqual(bridgeEventArguments({arguments:{flag:{type:'bool',value:{event:'flag'}}}},{flag:false}),{flag:false});
});
test('bridge PC controls reject blank, fractional, unsafe and out-of-range mutation values',()=>{
 for(const value of ['',undefined,'1.5','-1','101','Infinity','NaN','9007199254740992'])assert.throws(()=>bridgeEventArguments(seek,{position:value}));
});
test('bridge PC text parameters use byte limits and reject control characters',()=>{
 const command={arguments:{label:{type:'string',value:{event:'text'}}}};
 assert.deepEqual(bridgeEventArguments(command,{text:'汉'.repeat(21)}),{text:'汉'.repeat(21)});
 for(const text of ['汉'.repeat(22),'a\u0000b','line\nfeed'])assert.throws(()=>bridgeEventArguments(command,{text}));
});
