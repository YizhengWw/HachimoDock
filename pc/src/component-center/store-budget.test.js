import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {persistenceFits,persistenceBudget} from './store-budget.js';
import * as packaged from '../../skills/petui/scripts/store-budget.mjs';
const {cases}=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/store-budget-v1.json',import.meta.url)));
for(const c of cases)test(c.name,()=>{assert.equal(persistenceFits(c.runtime),c.ok);assert.equal(packaged.persistenceFits(c.runtime),c.ok);if(c.ok){const q=persistenceBudget(c.runtime);assert.ok(q.bytes<=q.limit);}});
