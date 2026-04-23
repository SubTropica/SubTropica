(* SubTropica paclet loader.
   Prepend the paclet root to $Path so sibling packages loaded by
   BeginPackage dependencies (HyperIntica`, mzv`, PolynomialQuotientFF`)
   resolve. Some Wolfram kernels don't auto-add the paclet root to
   $Path when init.m runs, which would cause Needs["HyperIntica`"] from
   inside SubTropica.wl to fail with "Cannot open HyperIntica`". *)
Module[{root = DirectoryName[$InputFileName, 2]},
    If[!MemberQ[$Path, root], PrependTo[$Path, root]];
    Get[FileNameJoin[{root, "SubTropica.wl"}]]
]
