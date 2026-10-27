// Build-only editorial assembly. No API, user data, device writes or publication.
import fs from 'node:fs';import path from 'node:path';
const [root,trial,out]=process.argv.slice(2);
const overrides=JSON.parse(fs.readFileSync(new URL('../new-components/source-art/book-editorial-overrides.json',import.meta.url)));
const contaminated=new Set([42,48,51,63,64,71,76,80,83,86,90]);
const replacements={
  16:[['这个假设场景里藏着今天要聊的三个概念：幸存者偏差、社会认同，以及标签如何悄悄替换掉具体证据。它们常常联手，让你在自以为清醒时做出糊涂决定。','这个场景主要体现社会认同。它和幸存者偏差不是同一个概念；后者涉及哪些样本进入视野，需要另外检查。']],
  44:[['你可能会想：机场租金高，所以贵。这个解释没错，但只揭开了一半。假设机场店每天只卖出五十杯，而街角店卖出三百杯，两家的房租分摊到每杯上可能差不多。','你可能会想，都是租金高惹的祸。但稀缺地段能吸引顾客支付更多，也会使商家争租店面，把地段带来的部分收益交给房东。租金可以是这种优势的结果，不能只把它当成高售价的单向原因。'],['价格因此不是客观事实，而是双方在信息不完全下的试探结果。','成交价格是可以观察的事实；信息不完全影响的是它怎样形成，以及双方如何理解它。']],
  61:[['比如客户买电钻，任务可能是墙上挂画，而非拥有电钻。','例如在一个原创假设中，家长选择放学后的活动，想解决的可能是接孩子之前的一段照护空档，而不只是让孩子多学一项技能。']],
  90:[['研究表明，交易越频繁，收益往往越差——部分原因是交易成本累积，另一部分原因是择时错误。','频繁操作会累计成本，也会增加判断时点的次数；最终效果仍取决于具体行为和环境，不能由频率单独决定。'],['比如，持有一个低成本的宽基指数基金，长期来看可能胜过大多数主动管理基金。为什么？因为简单策略避免了频繁决策的陷阱，也降低了成本和犯错概率。','简单和透明的安排可能更容易被理解和持续检查，减少无必要的决策环节。但是否适用仍需看目标与约束，不存在仅凭简单就胜出的保证。'],['低成本意味着更多收益留存，透明意味着你能看懂钱投向了哪里。老张如果选择费率更低的指数基金，并减少交易，结果可能不同。','在其他条件相近时，成本是影响最终结果的一项因素；透明有助于了解钱投向哪里。假设中的老张若先理解费用与风险，至少可以减少一部分认知盲区，但结果仍不确定。']]
};
const append={
  12:'还可以把练习再推进一步：事先约定什么现象出现时需要重新检查判断，而不只是记录一个成功百分比。一次结果无法证明七成这个估计准不准，只有在许多定义清楚、可以比较的判断中，才有机会观察校准程度。若你每次把成功改成不同含义，再多记录也难以对照。评估过程不是替失败找借口，而是把可改进的遗漏和不可控的偶然分开，避免下一轮仍在同一个地方犯错。',
  25:'还需澄清，把脑子推到极限并不是要求耗尽精力。这里强调的是认知投入和有质量的注意力，不是忍到疲惫也不休息。小林若只记录关了多少次通知，却不检查方案是否更清楚，也可能把专注本身当成新的表演指标。可以同时记录一个具体交付和一次必要沟通，观察怎样的时间边界既保护思考，也没有把协作成本全部转给别人。',
  61:'组织匹配还意味着，不只问有没有钱和人，也要问这些资源通常怎样被分配。一个习惯用成熟市场订单规模判断价值的团队，可能很难为尚未清晰的新需求腾出空间。反过来，新团队也不能把探索当作永远不用说明进展的理由。它需要与阶段相称的证据，例如是否找到了反复出现的任务，是否有人愿意改变原来的做法，以及哪些假设已经被否定。把早期学习成果和成熟业务收入混在同一张排名表上，容易奖励看起来漂亮、却没有减少不确定性的活动。这是对组织机制的分析，不是所有新项目都应该独立运营的结论。'
};
let cards=[],bibliography=[],audit=[],usage={generationTokens:0,reviewTokens:0,unknownCalls:0};
const sample=fs.readFileSync(trial,'utf8').trim().split('\n');const sampleText=sample.shift()+' '+sample.join('');
cards.push({id:'thinking-in-systems',title:'系统之美',text:sampleText,source:'Donella Meadows · 原创导读',url:'https://www.penguinrandomhouse.com/books/801035/thinking-in-systems-by-donella-meadows/',category:'组织系统',license:'Original commentary',revision:'editorial-1'});
for(let i=2;i<=100;i++){
 const id=`book-${String(i).padStart(3,'0')}`,dir=path.join(root,id),draft=JSON.parse(fs.readFileSync(path.join(dir,'draft.json')));
 const editedPath=path.join(dir,'edited.json');let chosen=fs.existsSync(editedPath)&&!contaminated.has(i)?JSON.parse(fs.readFileSync(editedPath)):draft;
 let text=chosen.text,question=chosen.question,changes=[];
 if(overrides[id]){({text,question}=overrides[id]);changes.push('manual original replacement');}
 if([12,25,32,35,38,44,61,90].includes(i)&&!overrides[id]){text=draft.text;question=draft.question;}
 for(const [a,b] of replacements[i]||[]){if(!text.includes(a))throw Error(`Missing editorial anchor ${id}`);text=text.replace(a,b);changes.push('targeted concept correction');}
 if(i===35){const paras=text.split(' ');text=`两个人都会做的事，为什么仍可能值得分工？比较优势看的不是谁样样更快，而是谁为了多做一件事，需要少做的另一件事更少。下面用一组明确的假设说明，不是原书案例。假设同样一天里，甲只能在十二个面包和六块点心之间安排时间，乙只能在六个面包和四块点心之间安排时间，而且生产按比例转换。甲做面包和点心都更快，具有两项绝对优势，但不等于两样都该由甲包办。甲多做一个面包，要放弃半块点心；乙多做一个面包，要放弃三分之二块点心。因此甲做面包的机会成本较低，乙做点心的机会成本较低。换一个方向看，甲每多做一块点心要少做两个面包，乙只需少做一个半。\n如果起初两人各分一半时间，他们合计有九个面包和五块点心。若甲专做面包，乙专做点心，总量变成十二个面包和四块点心。不能把两种商品简单相加就宣布总产量提高，因为面包与点心不是同一单位，偏好也未知。更严谨的比较是寻找可同时增加两种消费的可行组合。可以让甲用四分之三天做九个面包，剩余四分之一天做一点五块点心；乙用一天做四块点心。合计仍有九个面包，但点心增加到五点五块。这个有条件的例子说明，调整分工可以扩大可选择的组合，而不是自动规定大家想消费什么。\n交换比例还影响好处如何分配。以一个面包换零点六块点心为例，这个假设比例介于双方做面包所放弃的点心数量之间，因而存在双方通过交换改善选择的空间。但模型中的生产者与现实社会中的劳动者、企业并不相同，总体收益也不意味着每个群体都获益。李嘉图对分工和分配的讨论应结合条件理解，而不是被当成任何贸易安排都必然公平的保证。\n`+paras.slice(2).join(' ');changes.push('recomputed opportunity-cost example');}
 if(i===32){const p=text.split(' ');text=p.filter((_,n)=>n!==1).join(' ');changes.push('remove repetitive paragraph');}
 if(i===38){const p=text.split(' ');p[1]='局部均衡分析先集中研究一个市场，把其他相关市场的变化暂时置于背景，观察供给与需求如何共同作用。它不等于只研究一家店，也不等于现实中的收入、替代品价格和成本真的静止。以假设的奶茶市场为例，需求增加与原料涨价若同时发生，只看一种变化就可能漏掉另一种影响。明确暂时固定了什么，才知道推导的结论需要哪些条件。';text=p.filter((_,n)=>n!==4&&n!==5).join(' ');changes.push('partial equilibrium and brevity correction');}
 if(i===91){let p=text.split(' ');text=p.slice(1,-1).join(' ').replace('买入价格所包含的风险溢价','买入价格所包含的高增长预期').replace('股价跌了三分之一','价格可能承受进一步压力').replace('但历史观察显示，高估值有回归倾向，只是时间不确定。','但不能保证高估值会在某个时点回到过去的水平，市场结构和未来预期也可能变化。');changes.push('remove duplicated introduction and overprecise case');}
 if(append[i]){text+=' '+append[i];changes.push('expanded mechanism and limits');}
 // Reject editorial prompt leakage: return to the original for those manuscripts.
 text=text.replaceAll('左右摇摆的船上','不断变化的目标之间').replaceAll('subgroup','子群体').replaceAll('非医疗的日常','日常').replace(/\s+/g,' ').trim();
 if(!/假设|虚构/.test(text))throw Error(`${id}: unclear example provenance`);
 let title=draft.book.title;
 if(i===57){text=text.replaceAll(title,'创业的神话');title='创业的神话';}
 if(i===87){text=text.replaceAll(title,'你的钱，你的人生');title='你的钱，你的人生';}
 const content=question+' '+text,han=(content.match(/[\u3400-\u9fff]/g)||[]).length;
 if(han<800||han>1500||Buffer.byteLength(content)>6000)throw Error(`${id}: ${han} Han / ${Buffer.byteLength(content)} bytes`);
 cards.push({id,title,text:content,source:`${draft.book.author} · 原创导读`,url:draft.source,category:draft.book.category,license:'Original commentary',revision:'editorial-1'});
 bibliography.push({...draft.book,title,url:draft.source});audit.push({id,han,changes,modelEdited:chosen!==draft&&!overrides[id],originalReviewNotes:draft.reviewNotes});
 for(const [file,field] of [['usage.json','generationTokens'],['review-usage.json','reviewTokens']]){let u=JSON.parse(fs.readFileSync(path.join(dir,file)));usage[field]+=u.usage?.total_tokens||0;if(u.status.includes('unknown'))usage.unknownCalls++;}
}
const cats={};for(const c of cards)cats[c.category]=(cats[c.category]||0)+1;
if(cards.length!==100||Object.keys(cats).length!==10||Object.values(cats).some(n=>n!==10)||new Set(cards.map(c=>c.text)).size!==100)throw Error('Incomplete categories/unique content');
fs.mkdirSync(out,{recursive:true});fs.writeFileSync(path.join(out,'catalog.json'),JSON.stringify(cards,null,2));
fs.writeFileSync(path.join(out,'editorial-audit.json'),JSON.stringify({method:'Bibliographic metadata matching; model-assisted draft review plus targeted manual rewrites. Not expert or full-text edition verification.',cards:100,categories:cats,usage,articles:audit},null,2));
fs.writeFileSync(path.join(out,'bibliography.json'),JSON.stringify(bibliography,null,2));
console.log(JSON.stringify({cards:100,categories:cats,minHan:Math.min(...cards.map(c=>(c.text.match(/[\u3400-\u9fff]/g)||[]).length)),maxHan:Math.max(...cards.map(c=>(c.text.match(/[\u3400-\u9fff]/g)||[]).length)),usage}));
