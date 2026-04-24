(* SubTropica paclet loader.
   Prepend the paclet root to $Path so sibling packages loaded by
   BeginPackage dependencies (HyperIntica`, mzv`, PolynomialQuotientFF`)
   resolve. Some Wolfram kernels don't auto-add the paclet root to
   $Path when init.m runs, which would cause Needs["HyperIntica`"] from
   inside SubTropica.wl to fail with "Cannot open HyperIntica`".

   The Get is wrapped in CheckAbort so a failed load-time probe (e.g.,
   a RunProcess on a missing external tool that bubbles an Abort past
   the per-probe guards) degrades into an actionable error instead of
   a silent $Aborted return from Needs["SubTropica`"]. *)
Module[{root = DirectoryName[$InputFileName, 2], result},
    If[!MemberQ[$Path, root], PrependTo[$Path, root]];
    result = CheckAbort[
        Get[FileNameJoin[{root, "SubTropica.wl"}]],
        $Aborted];
    If[result === $Aborted,
        Print["[SubTropica] Package load was aborted before it finished."];
        Print["[SubTropica] This almost always means a required external tool"];
        Print["             is missing or mis-configured.  Common culprits:"];
        Print["               * polymake  (REQUIRED): brew install polymake  |  apt install polymake"];
        Print["               * python3   (needed for pySecDec): brew install python  |  apt install python3"];
        Print["               * ginac/ginsh, maple, HyperInt (optional)"];
        Print["[SubTropica] For a full report, retry with:"];
        Print["               $Echo = {\"stdout\"}; Needs[\"SubTropica`\"];"];
        Print["             or after a successful load, call STCheckDependencies[]."];
        $Failed,
        result]
]
