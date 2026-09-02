# content/sandbox/res -- fixtures for sb_res

Content the `sb_res` sandbox names through `RID()` / `RES_TREE()`, so the build can prove
every one of its names stands for exactly one file and list them in the sandbox's resource
manifest (`build/obj/sb_res/sb_res_res_manifest.txt`).  Nothing else references this tree,
so no package ever carries it.

| File                   | Proves                                                              |
|------------------------|---------------------------------------------------------------------|
| `icon/save.png`        | a leaf resolves to one file, whatever its extension                 |
| `icon/load.png`        | a second leaf named explicitly inside a subtree is listed once      |
| `icon/never.png`       | a name only ever queried at runtime still needs a file              |
| `icon/tree_only.png`   | a file no source names is listed by expanding the subtree           |
| `icon/sub/deep.txt`    | subtree expansion recurses                                          |
| `font/mono/16.recipe`  | a name may be backed by a recipe (baked to a font at cook time)     |

File and directory names here are lowercase, as all content must be: the name a `RID()`
spells is the path the runtime opens, and res_tool fails the build with file:line on a file
that is spelled otherwise.

Adding a file under `icon/` changes `sb_res`'s manifest (and its harvest test's expected
count) without touching a source file; the build notices through the deps file res_tool
writes beside the manifest.
