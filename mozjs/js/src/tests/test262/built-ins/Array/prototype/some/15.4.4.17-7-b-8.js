// Copyright (c) 2012 Ecma International.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-array.prototype.some
es5id: 15.4.4.17-7-b-8
description: >
    Array.prototype.some - deleting own property causes index property
    not to be visited on an Array-like object
---*/

var accessed = false;

function callbackfn(val, idx, obj) {
  accessed = true;
  return idx === 1;
}
var arr = {
  length: 2
};

Object.defineProperty(arr, "1", {
  get: function() {
    return 6.99;
  },
  configurable: true
});

Object.defineProperty(arr, "0", {
  get: function() {
    delete arr[1];
    return 0;
  },
  configurable: true
});

assert.sameValue(Array.prototype.some.call(arr, callbackfn), false, 'Array.prototype.some.call(arr, callbackfn)');
assert(accessed, 'accessed !== true');

reportCompare(0, 0);
