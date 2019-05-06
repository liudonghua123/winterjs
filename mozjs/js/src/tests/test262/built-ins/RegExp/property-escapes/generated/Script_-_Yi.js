// |reftest| skip -- regexp-unicode-property-escapes is not supported
// Copyright 2018 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script=Yi`
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
    [0x00A000, 0x00A48C],
    [0x00A490, 0x00A4C6]
  ]
});
testPropertyEscapes(
  /^\p{Script=Yi}+$/u,
  matchSymbols,
  "\\p{Script=Yi}"
);
testPropertyEscapes(
  /^\p{Script=Yiii}+$/u,
  matchSymbols,
  "\\p{Script=Yiii}"
);
testPropertyEscapes(
  /^\p{sc=Yi}+$/u,
  matchSymbols,
  "\\p{sc=Yi}"
);
testPropertyEscapes(
  /^\p{sc=Yiii}+$/u,
  matchSymbols,
  "\\p{sc=Yiii}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x009FFF],
    [0x00A48D, 0x00A48F],
    [0x00A4C7, 0x00DBFF],
    [0x00E000, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script=Yi}+$/u,
  nonMatchSymbols,
  "\\P{Script=Yi}"
);
testPropertyEscapes(
  /^\P{Script=Yiii}+$/u,
  nonMatchSymbols,
  "\\P{Script=Yiii}"
);
testPropertyEscapes(
  /^\P{sc=Yi}+$/u,
  nonMatchSymbols,
  "\\P{sc=Yi}"
);
testPropertyEscapes(
  /^\P{sc=Yiii}+$/u,
  nonMatchSymbols,
  "\\P{sc=Yiii}"
);

reportCompare(0, 0);
