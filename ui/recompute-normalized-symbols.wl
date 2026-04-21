(* Batch recompute normalized symbol fields for all library entries *)
(* Outputs results to ui/_norm_fields.json; run merge-normalized-symbols.py after *)
(* Run: wolframscript -file ui/recompute-normalized-symbols.wl *)

Needs["SubTropica`", "SubTropica.wl"];

$libraryDir = FileNameJoin[{Directory[], "library-bundled"}];

processed = 0;
skipped = 0;
allResults = <||>;

Do[
  Module[{entryPath, entry, results, nResults},
    entryPath = FileNameJoin[{dir, "entry.json"}];
    If[!FileExistsQ[entryPath], Continue[]];

    entry = Quiet @ Check[Import[entryPath, "RawJSON"], $Failed];
    If[entry === $Failed, Continue[]];

    results = Lookup[entry, "Results", {}];
    nResults = Length[results];

    Do[
      Module[{r = results[[i]], compressed, expr, normFields},
        compressed = Lookup[r, "resultCompressed", ""];
        If[!StringQ[compressed] || StringLength[compressed] == 0, skipped++; Continue[]];

        expr = Quiet @ Check[Uncompress[compressed], $Failed];
        If[expr === $Failed, skipped++; Continue[]];

        normFields = Quiet @ Check[stComputeNormalizedSymbolFields[expr], <||>];
        If[normFields === <||> || Length[normFields] == 0, skipped++; Continue[]];

        (* Store result keyed by relative path + result index *)
        Module[{relPath = FileNameDrop[entryPath, FileNameDepth[$libraryDir]]},
          allResults[relPath <> ":" <> ToString[i - 1]] = normFields;
        ];
        processed++;

        Print["  ", FileNameTake[dir], ": scale=", normFields["symbolScale"],
          " W's=", Length[normFields["wDefinitions"]],
          " terms=", normFields["normalizedSymbolTerms"]];
      ],
      {i, nResults}
    ];
  ],
  {dir, Sort @ FileNames["*", $libraryDir, {2}]}
];

Print["\nProcessed: ", processed];
Print["Skipped:   ", skipped];

(* Write all results to a single clean JSON file *)
Module[{outPath = FileNameJoin[{Directory[], "ui", "_norm_fields.json"}]},
  Export[outPath, allResults, "RawJSON"];
  Print["Wrote ", outPath];
];
