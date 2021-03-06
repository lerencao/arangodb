// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

Protocol.Debugger.enable();

Protocol.Debugger.onceScriptParsed().then(message => message.params.scriptId)
  .then((scriptId) => Protocol.Debugger.getPossibleBreakpoints({ start: { lineNumber: 0, columnNumber: 0, scriptId: scriptId }}))
  .then(InspectorTest.logMessage)
  .then(InspectorTest.completeTest);

compileAndRunWithOrigin("() => []", "", 0, 0);
