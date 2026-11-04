"""Synthetic same-source device/PC navigation; no USB, providers or user data."""
import os
import subprocess
from p4_program_test import ROOT,WARNINGS,run

REPO=ROOT.parent

def test_navigation_product_artifacts_reproducible():
    run([os.environ.get('PET_FRAMEWORK_NODE','node'),REPO/'pc/scripts/build-navigation-runtime.mjs','--check'])
    source=(REPO/'pc/src/component-center/app-navigation.js').read_bytes()
    assert source==(REPO/'pc/skills/petui/scripts/app-navigation.mjs').read_bytes()

def test_navigation_native_bridge_and_product_wasm_40000_operations(tmp_path):
    source=tmp_path/'navigation.c';binary=tmp_path/'navigation'
    source.write_text(r'''
#include "pet_p4_navigation_core.h"
#include <assert.h>
#include <stdio.h>
void nv_begin(unsigned,unsigned,int);int nv_history(unsigned,int),nv_rule(int,int,int),nv_allowed(void),nv_apply(void),nv_page(void),nv_at(unsigned);unsigned nv_depth(void);
int main(void){
 pet_nav_history_t h={0};int8_t page=0;uint32_t seed=19,hash=2166136261U;
 for(unsigned step=0;step<40000;step++){
  seed=seed*1664525U+1013904223U;unsigned op=(seed>>16)%4+1;
  seed=seed*1664525U+1013904223U;int target=op==PET_NAV_BACK?-1:(int)((seed>>16)%12);
  seed=seed*1664525U+1013904223U;int from=(int)((seed>>16)%13)-1;
  pet_nav_rule_t r={.op=(uint8_t)op,.target=(int8_t)target,.from_page=(int8_t)from};
  nv_begin(12,h.depth,page);for(unsigned i=0;i<h.depth;i++)assert(nv_history(i,h.pages[i]));assert(nv_rule(op,target,from));
  bool enabled=pet_nav_allowed(&h,page,&r);assert((bool)nv_allowed()==enabled);
  assert((bool)nv_apply()==pet_nav_apply(&h,&page,&r));assert(nv_page()==page&&nv_depth()==h.depth);
  hash=(hash^(unsigned)enabled)*16777619U;hash=(hash^(uint8_t)page)*16777619U;hash=(hash^h.depth)*16777619U;
  for(unsigned i=0;i<h.depth;i++){assert(nv_at(i)==h.pages[i]);hash=(hash^h.pages[i])*16777619U;}
 }
 /* Raw ABI validates before narrowing, requires all history entries, and stays
  * failed after bad input until an explicit fresh full snapshot is supplied. */
 nv_begin(4,1,0);assert(nv_rule(PET_NAV_BACK,-1,-1)&&!nv_allowed());
 assert(!nv_history(0,256)&&!nv_allowed());assert(!nv_history(0,0));
 nv_begin(4,0,0);assert(!nv_rule(257,0,-1)&&!nv_allowed());
 nv_begin(4,0,0);assert(!nv_rule(PET_NAV_PUSH,256,-1)&&!nv_allowed());
 nv_begin(4,0,0);assert(!nv_rule(PET_NAV_BACK,0,-1)&&!nv_allowed());
 nv_begin(4,0,0);assert(!nv_rule(PET_NAV_PUSH,0,-257)&&!nv_allowed());
 nv_begin(4,PET_NAV_MAX_DEPTH+1,0);assert(!nv_rule(PET_NAV_RESET,0,-1)&&!nv_allowed());
 pet_nav_history_t invalid={.depth=255};pet_nav_rule_t reset={.op=PET_NAV_RESET,.target=0,.from_page=-1};
 int8_t p=0;assert(!pet_nav_apply(&invalid,&p,&reset)&&invalid.depth==255&&p==0);
 invalid.depth=1;invalid.pages[0]=PET_NAV_MAX_PAGES;assert(!pet_nav_apply(&invalid,&p,&reset)&&invalid.depth==1);
 assert(!pet_nav_apply(NULL,&p,&reset)&&!pet_nav_apply(&h,NULL,&reset));
 printf("%u\n",hash);return 0;
}
''')
    run(['cc',*WARNINGS,'-fsanitize=address,undefined','-I',ROOT/'main',source,
         REPO/'pc/scripts/game-wasm/navigation_bridge.c','-o',binary])
    expected=run([binary])
    script=r'''
import {navigate,navigationAllowed,NAVIGATION_ENGINE} from './pc/src/component-center/app-navigation.js';
const names=Array.from({length:12},(_,i)=>'page'+i),runtime={navigation:NAVIGATION_ENGINE,quota:'p4-quota-v3',pages:names.map(id=>({id}))},state={page:'page0',history:[]};
let seed=19,hash=2166136261;
const mix=n=>{hash=Math.imul(hash^n,16777619)>>>0;};
for(let step=0;step<40000;step++){
 seed=(Math.imul(seed,1664525)+1013904223)>>>0;const op=(seed>>>16)%4+1;
 seed=(Math.imul(seed,1664525)+1013904223)>>>0;const target=op===4?-1:(seed>>>16)%12;
 seed=(Math.imul(seed,1664525)+1013904223)>>>0;const from=(seed>>>16)%13-1;
 const rule={page:{[['','push','replace','reset','back'][op]]:op===4?true:names[target]},...(from>=0?{from_page:names[from]}:{})};
 const allowed=navigationAllowed(runtime,state,rule);if(navigate(runtime,state,rule)!==allowed)throw Error('availability');
 mix(Number(allowed));mix(names.indexOf(state.page));mix(state.history.length);for(const page of state.history)mix(names.indexOf(page));
}
console.log(hash);
'''
    result=subprocess.run([os.environ.get('PET_FRAMEWORK_NODE','node'),'--input-type=module','-e',script],
                          cwd=REPO,check=True,capture_output=True,text=True,timeout=30)
    assert not result.stderr and result.stdout.strip()==expected
