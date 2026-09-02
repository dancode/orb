# content/sandbox/res -- fixtures for sb_res

Content the `sb_res` sandbox names through `RID()` / `RES_TREE()`, so the build can resolve
every one of its names to a file.  Nothing else references this tree, so no package ever
carries it.

| File                   | Proves                                                              |
|------------------------|---------------------------------------------------------------------|
| `icon/save.png`        | a leaf resolves to its cooked path (`.png` -> `.tex`)               |
| `icon/Load.png`        | the recorded path keeps the on-disk spelling (`Load.tex`)           |
| `icon/never.png`       | a name only ever queried at runtime still needs a file              |
| `icon/tree_only.png`   | a file no source names is listed by expanding the subtree           |
| `icon/sub/deep.txt`    | subtree expansion recurses; a copy-kind file keeps its extension    |
| `font/mono/16.recipe`  | a recipe resolves to the cooked file its `kind` line produces       |

Adding a file under `icon/` changes `sb_res`'s generated table (and its harvest test's
expected count) without touching a source file; the build notices through the deps file
res_tool writes beside the table.
