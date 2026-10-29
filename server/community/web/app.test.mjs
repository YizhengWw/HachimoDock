// Read-only, isolated DOM regression. No production requests, browser profile or device access.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source=readFileSync(new URL('./app.js',import.meta.url),'utf8');
class Element {
  constructor(tag){this.tag=tag;this.children=[];this.attributes={};this.events={};}
  append(...children){this.children.push(...children);}
  replaceChildren(...children){this.children=[...children];}
  setAttribute(key,value){this.attributes[key]=value;}
  addEventListener(key,handler){this.events[key]=handler;}
}
function harness(detail){
  const root=new Element('main'),writes=[],parent={postMessage(){}};
  const sandbox=vm.createContext({Node:Element,URLSearchParams,AbortSignal,crypto:globalThis.crypto,
    document:{querySelector:()=>root,createElement:tag=>new Element(tag),createTextNode:text=>text},
    window:{parent,addEventListener(){}},parent,location:{hash:'',origin:'https://community.invalid'},
    localStorage:{getItem:key=>key==='community-browser-token'?JSON.stringify('a'.repeat(64)):null,setItem:(...args)=>writes.push(args)},
    setTimeout:()=>0,clearTimeout(){},__detail:detail});
  vm.runInContext(source,sandbox);
  vm.runInContext("api=async(action,fields)=>action==='rankings'?{items:[]}:__detail(fields.id)",sandbox);
  return {root,writes,run:code=>vm.runInContext(code,sandbox)};
}
const plain=value=>JSON.parse(JSON.stringify(value));
function text(node){return typeof node==='string'?node:[node.textContent||'',...node.children.map(text)].join(' ');}
function all(node,predicate){return typeof node==='string'?[]:[...(predicate(node)?[node]:[]),...node.children.flatMap(child=>all(child,predicate))];}

test('404 and legacy native unlist errors hide entries without changing favorites',async()=>{
  const h=harness(id=>{if(id==='withdrawn-http')throw Object.assign(Error('gone'),{status:404});if(id==='withdrawn-native')throw Error('组件不存在或已下架');return {id,name:'可用组件',available:true};});
  const saved=[{id:'withdrawn-http',name:'旧收藏 A'},{id:'withdrawn-native',name:'旧收藏 B'},{id:'active',name:'可用组件'}];
  h.run(`context={favorites:${JSON.stringify(saved)}}`);
  const result=plain(await h.run('loadFavoritePage(context.favorites,0)'));
  assert.deepEqual(result,{items:[{id:'active',name:'可用组件',available:true}],failedCount:0,nextOffset:null});
  assert.deepEqual(plain(h.run('context.favorites')),saved);assert.deepEqual(h.writes,[]);
});

test('favorites UI has no withdrawn card, link or unavailable placeholder',async()=>{
  const h=harness(id=>{if(id==='withdrawn')throw Error('组件不存在或已下架');return {id,name:'可用组件',available:true};});
  h.run("state.view='favorites';context={favorites:[{id:'withdrawn',name:'已撤回作品'},{id:'active',name:'可用组件'}]}");
  await h.run('renderList()');
  assert.equal(all(h.root,node=>node.className==='card').length,1);
  assert.doesNotMatch(text(h.root),/已撤回作品|暂不可用或已下架/);
  assert.match(text(h.root),/可用组件/);assert.deepEqual(h.writes,[]);
});

test('offline favorites show a retry notice, not stale actionable cards',async()=>{
  const h=harness(()=>{throw TypeError('network unavailable');});
  h.run("state.view='favorites';context={favorites:[{id:'withdrawn',name:'已撤回作品'}]}");
  await h.run('renderList()');
  assert.equal(all(h.root,node=>node.className==='card').length,0);
  assert.doesNotMatch(text(h.root),/已撤回作品/);assert.match(text(h.root),/部分收藏暂时无法加载/);
  assert.match(text(h.root),/重试/);assert.deepEqual(h.writes,[]);
});

test('hidden first page keeps next page reachable and filters explicit unavailable records',async()=>{
  const h=harness(id=>({id,name:'收藏',available:id==='saved-18'}));
  h.run('context={favorites:Array.from({length:19},(_,i)=>({id:`saved-${i}`}))}');
  assert.deepEqual(plain(await h.run('loadFavoritePage(context.favorites,0)')),{items:[],failedCount:0,nextOffset:18});
  assert.equal((await h.run('loadFavoritePage(context.favorites,18)')).items.length,1);
  assert.equal(h.run("card({id:'withdrawn',available:false})"),null);
  assert.deepEqual(h.writes,[]);
});
