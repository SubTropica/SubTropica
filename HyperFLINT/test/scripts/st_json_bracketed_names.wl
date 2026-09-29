(* INV-JSON-STRING-ARRAYS regression, Mathematica side (2026-09-22).

   The ST -> HF bridge invariants are exercised through the CLI transport
   against CANNED engine responses (test/fixtures/st_fake_hf/, recorded from
   the real engine on 2026-09-22 and doctored here by string replacement), so
   the gates need no engine computation and do not depend on the state of the
   LibraryLink dylib.  A stand-in executable prints the response of the case
   under test; the lazy loader is disarmed so that the CLI path is taken.

   Gates:
     (a) STFindLROrdersHF: nPolys equal to the sent counts -> the verdict is
         returned; nPolys [1] for 3 sent, or nPolys absent -> ::polycount and
         $Failed (unconditional: find_lr_orders has emitted nPolys since the
         LibraryLink MVP).
     (b) STFindLROrdersScanHF and STBuildFactorTable: schema 3 with a wrong
         count -> ::polycount and $Failed; schema 2 without nPolys -> accepted
         (the stale-binary warning is the only signal); STBuildFactorTable with
         an indexed coefficient name returns a table (its clean-scope Block
         shadows the head symbol mm; before: Block::lvsym, table unevaluated).
     (c) STHyperFlint: the canned result 1 decodes to 1; a canned result that
         still depends on the integration variable -> ::varsurvived and $Failed;
         the issue #52 round-5 face response (22 terms, zero-one periods, an
         I Pi delta[x6] term with x6 integrated) decodes without ::varsurvived,
         x6 surviving only inside HyperIntica`delta.

   Needs wolframscript; ST_ROOT = the SubTropica tree.  Exit 0 iff all pass. *)
$HistoryLength = 0;
$STSuppressStaleWarn = True;
(* All gates run in one evaluation, so General::stop would mute a message after
   its third print and a later !fired[] gate would pass vacuously. *)
Off[General::stop];
root = Environment["ST_ROOT"];
Get[FileNameJoin[{root, "SubTropica.wl"}]];

fixDir = FileNameJoin[{root, "HyperFLINT", "test", "fixtures", "st_fake_hf"}];
work = CreateDirectory[];
respFile = FileNameJoin[{work, "response.json"}];
reqFile = FileNameJoin[{work, "request.json"}];
stub = FileNameJoin[{work, "hyperflint_stub.sh"}];
(* the stand-in records the request it receives and prints the canned response *)
Export[stub, "#!/bin/sh\ncat > \"" <> reqFile <> "\"\ncat \"" <> respFile <> "\"\n", "Text"];
Run["chmod +x " <> stub];
canned[name_String] := Import[FileNameJoin[{fixDir, name}], "Text"];
setResponse[s_String] := Export[respFile, s, "Text"];
(* A doctoring that does not match its fixture is a test defect, never a silent
   no-op: the response then becomes invalid JSON and the gate fails. *)
doctor[s_String, rules_] := Module[{d = StringReplace[s, rules]},
    If[d === s, Print["[st-json-bracketed] doctoring did not match: ", rules]; "DOCTORING FAILED", d]];
(* the request SubTropica produced, and the recorded one, as sorted rule lists
   (mzv_data_path is an absolute path of the recording machine) *)
producedReq[] := Sort @ Normal @ KeyDrop[Import[reqFile, "RawJSON"], {"mzv_data_path"}];
recordedReq[name_String] := Sort @ Normal @ KeyDrop[Import[FileNameJoin[{fixDir, name}], "RawJSON"], {"mzv_data_path"}];

(* CLI transport with the stand-in; the loader is disarmed inside the Block below. *)
$STHyperFlintUseLibraryLink = False;
$STHyperFlintAllowCLI = True;
$STHyperFlintPath = stub;
$STHyperFlintDataPath = FileNameJoin[{root, "HyperFLINT", "data", "mzv_reductions.json"}];

nPass = 0; nFail = 0;
gate[label_String, ok_] := (If[TrueQ[ok], nPass++, nFail++];
    Print["[st-json-bracketed] ", If[TrueQ[ok], "PASS ", "FAIL "], label]);
(* fired[msg, expr] is True iff evaluating expr generated msg.  Check sees only
   messages that are actually generated, so these calls are not Quiet-ed; both
   arguments stay held (an evaluated message name is its text, Check::mgre). *)
SetAttributes[fired, HoldAll];
fired[msg_, expr_] := Check[expr; False, True, msg];

polys = {x + mm[1], x + y, y + 1}; xvars = {x, y};
exps = {{{1, 0}, {0, 1}, {1, 1}}};
lr = canned["find_lr_orders.resp.json"];
sc = canned["find_lr_orders_scan.resp.json"];
ft = canned["factor_table.resp.json"];
hf = canned["hyperflint.resp.json"];

Block[{SubTropica`stHyperFlintTryLoadLibrary},
  SubTropica`stHyperFlintTryLoadLibrary[] := False;

  (* ---- (a) STFindLROrdersHF ---- *)
  setResponse[lr];
  r = Quiet[STFindLROrdersHF[polys, xvars],
      {STFindLROrdersHF::versionmismatch, STFindLROrdersHF::schemamismatch}];
  gate["a1 find_lr_orders: matching nPolys returns the verdict {{y, x}, score}",
      MatchQ[r, {{y, x}, _?NumericQ}]];
  gate["a1' the produced find_lr_orders request equals the recorded one",
      producedReq[] === recordedReq["find_lr_orders.req.json"]];
  setResponse[doctor[lr, "\"nPolys\":[3]" -> "\"nPolys\":[1]"]];
  gate["a2 find_lr_orders: nPolys [1] for 3 sent -> ::polycount and $Failed",
      fired[STFindLROrdersHF::polycount, STFindLROrdersHF[polys, xvars]] &&
      Quiet[STFindLROrdersHF[polys, xvars]] === $Failed];
  setResponse[doctor[lr, ",\"nPolys\":[3]" -> ""]];
  gate["a3 find_lr_orders: nPolys absent -> ::polycount and $Failed",
      fired[STFindLROrdersHF::polycount, STFindLROrdersHF[polys, xvars]] &&
      Quiet[STFindLROrdersHF[polys, xvars]] === $Failed];

  (* ---- (b) scan wrapper and factor table ---- *)
  setResponse[sc];
  r = Quiet[STFindLROrdersScanHF[{polys}, xvars, exps]];
  gate["b1 scan: matching nPolys returns the association",
      AssociationQ[r] && KeyExistsQ[r, "Orders"]];
  gate["b1' the produced find_lr_orders_scan request equals the recorded one",
      producedReq[] === recordedReq["find_lr_orders_scan.req.json"]];
  setResponse[doctor[sc, "\"nPolys\":[3]" -> "\"nPolys\":[2]"]];
  gate["b2 scan: schema 3 with a wrong count -> STFindLROrdersScanHF::polycount and $Failed",
      fired[STFindLROrdersScanHF::polycount, STFindLROrdersScanHF[{polys}, xvars, exps]] &&
      Quiet[STFindLROrdersScanHF[{polys}, xvars, exps]] === $Failed];
  setResponse[doctor[sc, {"\"schema_version\":3" -> "\"schema_version\":2",
      ",\"nPolys\":[3]" -> ""}]];
  r = Quiet[STFindLROrdersScanHF[{polys}, xvars, exps]];
  gate["b3 scan: schema 2 without nPolys is accepted", AssociationQ[r]];

  setResponse[ft];
  r = Quiet[STBuildFactorTable[polys, "Order" -> {y, x}]];
  gate["b4 factor_table: indexed coefficient name mm[1] returns a table (head symbol shadowed)",
      AssociationQ[r]];
  gate["b4' the produced factor_table request equals the recorded one",
      producedReq[] === recordedReq["factor_table.req.json"]];
  setResponse[doctor[ft, "\"nPolys\":[3]" -> "\"nPolys\":[2]"]];
  gate["b5 factor_table: schema 3 with a wrong count -> STBuildFactorTable::polycount and $Failed",
      fired[STBuildFactorTable::polycount, STBuildFactorTable[polys, "Order" -> {y, x}]] &&
      Quiet[STBuildFactorTable[polys, "Order" -> {y, x}]] === $Failed];
  setResponse[doctor[ft, {"\"schema_version\":3" -> "\"schema_version\":2",
      "\"nPolys\":[3]," -> ""}]];
  r = Quiet[STBuildFactorTable[polys, "Order" -> {y, x}]];
  gate["b6 factor_table: schema 2 without nPolys is accepted", AssociationQ[r]];

  (* ---- (c) STHyperFlint ---- *)
  setResponse[hf];
  gate["c1 hyperflint: the canned result 1 decodes to 1",
      Quiet[STHyperFlint[1/(1 + x)^2, {x}]] === 1];
  gate["c1' the produced hyperflint request equals the recorded one (up to mzv_data_path)",
      producedReq[] === recordedReq["hyperflint.req.json"]];
  setResponse[doctor[hf, "\"coef\":\"1\"" -> "\"coef\":\"1/(1+x)^2\""]];
  gate["c2 hyperflint: a result still depending on x -> ::varsurvived and $Failed",
      fired[STHyperFlint::varsurvived, STHyperFlint[1/(1 + x)^2, {x}]] &&
      Quiet[STHyperFlint[1/(1 + x)^2, {x}]] === $Failed];
  (* codex referee: substituting 1 for delta[x] would cancel x*(1 - delta[x]) to 0 *)
  setResponse[doctor[hf, "\"coef\":\"1\"" -> "\"coef\":\"x*(1-delta[x])\""]];
  gate["c2' hyperflint: x*(1 - delta[x]) is a survivor (the blank is an inert atom, not 1)",
      fired[STHyperFlint::varsurvived, STHyperFlint[1/(1 + x)^2, {x}]] &&
      Quiet[STHyperFlint[1/(1 + x)^2, {x}]] === $Failed];
  setResponse[canned["hyperflint_issue52_face1.resp.json"]];
  req52 = Import[FileNameJoin[{root, "HyperFLINT", "test", "fixtures",
      "issue52_round5_face1.json"}], "RawJSON"];
  integrand52 = ToExpression[req52["expr"]];
  vars52 = ToExpression /@ req52["vars_int"];
  r = STHyperFlint[integrand52, vars52];
  gate["c3 issue #52 face: decodes (not $Failed) and carries HyperIntica`delta[x6]",
      r =!= $Failed && !FreeQ[r, HyperIntica`delta[x6]]];
  gate["c4 issue #52 face: x6 survives nowhere outside delta",
      r =!= $Failed && FreeQ[r /. HyperIntica`delta[_] -> 1, x6]];
  gate["c5 issue #52 face: no ::varsurvived",
      !fired[STHyperFlint::varsurvived, STHyperFlint[integrand52, vars52]]];
];

DeleteDirectory[work, DeleteContents -> True];
Print["[st-json-bracketed] ", nPass, " PASS / ", nFail, " FAIL"];
If[nFail === 0, Exit[0], Exit[1]];
