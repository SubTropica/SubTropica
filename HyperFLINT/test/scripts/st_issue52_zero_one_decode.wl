(* Issue #52 round 5 regression (Mathematica side): decode a hyperflint
   response carrying zero_one_periods atoms and multiple-zeta tokens through
   SubTropica's translator; values must match ginsh; a missing atom must fail
   closed through the ::zeroone message (never through ::unknowntoken). *)
$HistoryLength = 0;
Get[FileNameJoin[{Environment["ST_ROOT"], "SubTropica.wl"}]];
resp = ImportString["{\"op\":\"hyperflint\",\"result\":[{\"coef\":\"(5/3)*zop_2*mzv_2 + (2)*mzv_1_m3 + (7)*mzv_3_5\",\"key\":[]}],\"vars\":[\"x\"],\"zero_one_periods\":{\"zop_1\":[0,1,-1,-2,-1],\"zop_2\":[-2,-1]}}", "RawJSON"];
SubTropica`Private`$stHFZeroOnePeriodMap = resp["zero_one_periods"];
str = SubTropica`Private`stHyperFlintCoefStringToMma[resp["result"][[1]]["coef"]];
expr = ToExpression[str];
val = STToGinsh[expr];
(* G(-2,-1;1), G(0,0,-1,-1;1) = zeta(3bar,1), and zeta(5,3) (mzv_3_5 is the
   MZV, NOT Mathematica's Hurwitz Zeta[3,5] = 0.0244). *)
target = 5/3*0.147220676959241258302428276265933126*Zeta[2] + 2*0.0877856715686553020365932949977619342 + 7*0.0377076729848475440500280466022544;
ok = NumericQ[val] && Abs[val - target] < 10^-12 && FreeQ[expr, SubTropica`Private`TryReduceZeroOnePeriod] && FreeQ[expr, Zeta[3, 5]];
Print["[st-issue52] decoded: ", InputForm[expr]];
Print["[st-issue52] value ", N[val, 16], " target ", N[target, 16], " -> ", If[ok, "PASS", "FAIL"]];
(* Missing atom: must fail closed via ::zeroone, and the unknown-token scan
   must NOT be the thing that caught it (Check sees only messages that are
   actually generated, so the calls are not Quiet-ed). *)
call := SubTropica`Private`stHyperFlintCoefStringToMma["3*zop_9"];
guard = Quiet[call];
zeroFired = Check[call, "fired", STHyperFlint::zeroone] === "fired";
unknownFired = Check[call, "fired", STHyperFlint::unknowntoken] === "fired";
guardOK = guard === $Failed && zeroFired && !unknownFired;
Print["[st-issue52] unknown-atom guard -> ", If[guardOK, "PASS", "FAIL"], "  {failed, zeroone, unknowntoken} = ", {guard === $Failed, zeroFired, unknownFired}];
If[ok && guardOK, Exit[0], Exit[1]];
