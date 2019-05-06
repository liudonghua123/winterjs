// |reftest| skip -- regexp-unicode-property-escapes is not supported
// Copyright 2018 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script_Extensions=Mro`
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v11.0.0
esid: sec-static-semantics-unicodematchproperty-p
features: [regexp-unicode-property-escapes]
includes: [regExpUtils.js]
---*/

const matchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x016A40, 0x016A5E],
    [0x016A60, 0x016A69],
    [0x016A6E, 0x016A6F]
  ]
});
testPropertyEscapes(
  /^\p{Script_Extensions=Mro}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Mro}"
);
testPropertyEscapes(
  /^\p{Script_Extensions=Mroo}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Mroo}"
);
testPropertyEscapes(
  /^\p{scx=Mro}+$/u,
  matchSymbols,
  "\\p{scx=Mro}"
);
testPropertyEscapes(
  /^\p{scx=Mroo}+$/u,
  matchSymbols,
  "\\p{scx=Mroo}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [
    0x016A5F
  ],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x00DBFF],
    [0x00E000, 0x016A3F],
    [0x016A6A, 0x016A6D],
    [0x016A70, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script_Extensions=Mro}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Mro}"
);
testPropertyEscapes(
  /^\P{Script_Extensions=Mroo}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Mroo}"
);
testPropertyEscapes(
  /^\P{scx=Mro}+$/u,
  nonMatchSymbols,
  "\\P{scx=Mro}"
);
testPropertyEscapes(
  /^\P{scx=Mroo}+$/u,
  nonMatchSymbols,
  "\\P{scx=Mroo}"
);

reportCompare(0, 0);
