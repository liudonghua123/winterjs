// |reftest| skip -- regexp-unicode-property-escapes is not supported
// Copyright 2018 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script_Extensions=Brahmi`
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v11.0.0
esid: sec-static-semantics-unicodematchproperty-p
features: [regexp-unicode-property-escapes]
includes: [regExpUtils.js]
---*/

const matchSymbols = buildString({
  loneCodePoints: [
    0x01107F
  ],
  ranges: [
    [0x011000, 0x01104D],
    [0x011052, 0x01106F]
  ]
});
testPropertyEscapes(
  /^\p{Script_Extensions=Brahmi}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Brahmi}"
);
testPropertyEscapes(
  /^\p{Script_Extensions=Brah}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Brah}"
);
testPropertyEscapes(
  /^\p{scx=Brahmi}+$/u,
  matchSymbols,
  "\\p{scx=Brahmi}"
);
testPropertyEscapes(
  /^\p{scx=Brah}+$/u,
  matchSymbols,
  "\\p{scx=Brah}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x00DBFF],
    [0x00E000, 0x010FFF],
    [0x01104E, 0x011051],
    [0x011070, 0x01107E],
    [0x011080, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script_Extensions=Brahmi}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Brahmi}"
);
testPropertyEscapes(
  /^\P{Script_Extensions=Brah}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Brah}"
);
testPropertyEscapes(
  /^\P{scx=Brahmi}+$/u,
  nonMatchSymbols,
  "\\P{scx=Brahmi}"
);
testPropertyEscapes(
  /^\P{scx=Brah}+$/u,
  nonMatchSymbols,
  "\\P{scx=Brah}"
);

reportCompare(0, 0);
