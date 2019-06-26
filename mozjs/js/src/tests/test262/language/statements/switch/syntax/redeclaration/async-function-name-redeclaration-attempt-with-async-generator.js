// |reftest| error:SyntaxError
// This file was procedurally generated from the following sources:
// - src/declarations/async-generator.case
// - src/declarations/redeclare/switch-attempt-to-redeclare-async-function-declaration.template
/*---
description: redeclaration with AsyncGeneratorDeclaration (AsyncFunctionDeclaration in SwitchStatement)
esid: sec-switch-statement-static-semantics-early-errors
features: [async-iteration, async-functions]
flags: [generated]
negative:
  phase: parse
  type: SyntaxError
info: |
    SwitchStatement : switch ( Expression ) CaseBlock

    It is a Syntax Error if the LexicallyDeclaredNames of CaseBlock contains any
    duplicate entries.

---*/


$DONOTEVALUATE();

switch (0) { case 1: async function f() {} default: async function* f() {} }
