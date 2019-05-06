// |reftest| skip-if(!this.hasOwnProperty('BigInt')) -- BigInt is not enabled unconditionally
// Copyright (C) 2017 Josh Wolfe. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
description: modulus operator ToNumeric with BigInt operands
esid: sec-multiplicative-operators-runtime-semantics-evaluation
features: [BigInt, Symbol.toPrimitive, computed-property-names]
---*/
assert.sameValue(Object(2n) % 2n, 0n, 'The result of (Object(2n) % 2n) is 0n');
assert.sameValue(2n % Object(2n), 0n, 'The result of (2n % Object(2n)) is 0n');

assert.sameValue({
  [Symbol.toPrimitive]: function() {
    return 2n;
  }
} % 2n, 0n, 'The result of (({[Symbol.toPrimitive]: function() {return 2n;}}) % 2n) is 0n');

assert.sameValue(2n % {
  [Symbol.toPrimitive]: function() {
    return 2n;
  }
}, 0n, 'The result of (2n % {[Symbol.toPrimitive]: function() {return 2n;}}) is 0n');

assert.sameValue({
  valueOf: function() {
    return 2n;
  }
} % 2n, 0n, 'The result of (({valueOf: function() {return 2n;}}) % 2n) is 0n');

assert.sameValue(2n % {
  valueOf: function() {
    return 2n;
  }
}, 0n, 'The result of (2n % {valueOf: function() {return 2n;}}) is 0n');

assert.sameValue({
  toString: function() {
    return 2n;
  }
} % 2n, 0n, 'The result of (({toString: function() {return 2n;}}) % 2n) is 0n');

assert.sameValue(2n % {
  toString: function() {
    return 2n;
  }
}, 0n, 'The result of (2n % {toString: function() {return 2n;}}) is 0n');
reportCompare(0, 0);
