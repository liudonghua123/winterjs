// |reftest| error:SyntaxError
// This file was procedurally generated from the following sources:
// - src/declarations/let.case
// - src/declarations/redeclare/block-attempt-to-redeclare-generator-declaration.template
/*---
description: redeclaration with let-LexicalDeclaration (GeneratorDeclaration in BlockStatement)
esid: sec-block-static-semantics-early-errors
features: [generators]
flags: [generated]
negative:
  phase: parse
  type: SyntaxError
info: |
    Block : { StatementList }

    It is a Syntax Error if the LexicallyDeclaredNames of StatementList contains
    any duplicate entries.

---*/


$DONOTEVALUATE();

{ function* f() {} let f }
