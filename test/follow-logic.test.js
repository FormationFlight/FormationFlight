// Node test for html/follow-logic.js's pure functions. No build step, no
// new npm dependency: node:test + node:assert, run with `node --test`.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { slotFromOffset, offsetFromSlot, validateConfig } from '../html/follow-logic.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const fixturePath = path.join(__dirname, '..', 'docs', 'spec', 'fixtures', 'follow-config-cases.json');
const fixture = JSON.parse(readFileSync(fixturePath, 'utf8'));

// ---- Cross-mirror equivalence (JS side): every fixture case's
// expectValid expectation, shared across all three validators. ----

test('validateConfig() matches every fixture case', () => {
  for (const tc of fixture.cases) {
    const cfg = { ...fixture.baseline, ...tc.overrides };
    const err = validateConfig(cfg);
    const isValid = err === null;
    assert.equal(isValid, tc.expectValid, `case "${tc.name}": expected expectValid=${tc.expectValid}, got ${isValid} (err=${JSON.stringify(err)})`);
  }
});

// ---- slotFromOffset()/offsetFromSlot() round-trip, all three axes'
// label pairs including the 0 -> CENTER/LEVEL case. ----

const axisLabelPairs = [
  ['AHEAD', 'BEHIND', 'CENTER'],
  ['RIGHT', 'LEFT', 'CENTER'],
  ['ABOVE', 'BELOW', 'LEVEL'],
];

test('slotFromOffset()/offsetFromSlot() round-trip for all three axes', () => {
  for (const [pos, neg, zero] of axisLabelPairs) {
    for (const v of [15, -15, 0, 0.5, -0.5]) {
      const slot = slotFromOffset(v, pos, neg, zero);
      const back = offsetFromSlot(slot, Math.abs(v), pos, neg);
      assert.equal(back, v, `${pos}/${neg}/${zero} round-trip failed for v=${v} (slot=${slot})`);
    }
  }
});

test('slotFromOffset() maps exactly 0 to the zero label', () => {
  assert.equal(slotFromOffset(0, 'AHEAD', 'BEHIND', 'CENTER'), 'CENTER');
  assert.equal(slotFromOffset(0, 'ABOVE', 'BELOW', 'LEVEL'), 'LEVEL');
});

test('offsetFromSlot() maps the zero label back to exactly 0', () => {
  assert.equal(offsetFromSlot('CENTER', 15, 'AHEAD', 'BEHIND'), 0);
  assert.equal(offsetFromSlot('LEVEL', 10, 'ABOVE', 'BELOW'), 0);
});

// ---- validateConfig()'s own rule set: the uniqueness/ordering rules
// also covered by the fixture loop above, asserted explicitly by name here
// so a failure names the exact rule, not just "some fixture case". ----

test('validateConfig() uniqueness/ordering rules', () => {
  const base = fixture.baseline;

  const gvarClash = validateConfig({ ...base, statusGvarIndex: 1, conditionFlagsGvarIndex: 1 });
  assert.ok(gvarClash, 'GVAR index uniqueness should be enforced');
  assert.equal(gvarClash.section, 'gvar');

  const rcClash = validateConfig({ ...base, rcLongChannel: 5, rcLatChannel: 5 });
  assert.ok(rcClash, 'RC channel uniqueness should be enforced');
  assert.equal(rcClash.section, 'rc');

  const autothrottleOverlap = validateConfig({ ...base, rcLongChannel: 5, autothrottleEnableRcChannel: 5 });
  assert.ok(autothrottleOverlap, 'autothrottle arm channel must differ from RC axis channels');
  assert.equal(autothrottleOverlap.section, 'autothrottle');

  const thresholdOrder = validateConfig({ ...base, autothrottleEnableMinThresholdUs: 2100, autothrottleEnableMaxThresholdUs: 1700 });
  assert.ok(thresholdOrder, 'autothrottle threshold max must be > min');
  assert.equal(thresholdOrder.section, 'autothrottle');
});
