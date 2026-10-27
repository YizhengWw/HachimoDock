// Local QA entry; actual resource packages, no services or personal snapshots.
import React from 'react';
import {createRoot} from 'react-dom/client';
import {BUILTIN_COMPONENT_CENTER} from './fixtures.js';
import {isComponentVisible} from './component-center/library-order.js';
import ComponentCover from './component-center/ComponentCover.jsx';
import './styles.css';
function Preview(){return <main className="cover-qa"><style>{`.cover-qa{max-width:1380px;margin:40px auto;padding:24px}.cover-qa h1{font-size:28px}.cover-qa-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:26px;margin-top:30px}.cover-qa article{min-width:0}.cover-qa h2{font-size:16px;margin:12px 0 6px}.cover-qa p{font-size:13px;color:#777}.cover-qa .component-cover{box-shadow:0 8px 24px #243d2910}@media(max-width:850px){.cover-qa-grid{grid-template-columns:1fr}}`}</style><h1>组件中心 · 最新资源包封面</h1><p>完整 4:3 画布；不裁切、不加载私人数据。打开详情才进行交互。</p><div className="cover-qa-grid">{BUILTIN_COMPONENT_CENTER.components.filter(isComponentVisible).map(c=><article key={c.id} data-id={c.id}><ComponentCover component={c}/><h2>{c.name}</h2><p>资源版本 {c.version||'—'}</p></article>)}</div></main>;}
createRoot(document.getElementById('root')).render(<Preview/>);
