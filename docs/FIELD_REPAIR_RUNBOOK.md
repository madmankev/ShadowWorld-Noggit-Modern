# Field repair runbook - azeroth project after the 2026-10-24 session

Everything code-side that the October field sessions surfaced is fixed and on
the branch (commit stack `d55aa8a` save-path fixes, `f03f917` export audit,
`b89f8c2` repair tool). This runbook is the exact order of operations to turn
the project on disk healthy again. **Work with the game client closed.**

## Step 0 - Rebuild

Rebuild Noggit from this branch (tip `b89f8c2` or newer) and start it on the
`G:\temp2` project, azeroth map.

## Step 1 - Repair every tile file: Assist -> "Re-save every tile (repair project files)"

Why: the 39 tiles that "saved OK" during the October 24 session were written
by a build that rebuilt the OBJ1/OBJ0 placement lists but left the per-entry
companion chunks (MLDB/MLMB/MWDR) stale. Those files are structurally valid
but crash-prone: the client reads the companions in lockstep with the lists
and runs off the end while the tile streams in - the access violation at
azeroth_32_49 (crash dump `2026-10-24 15.03.53`, in-world at 13:44). The
re-save walks every tile through the current writer, which rebuilds the
companions in lockstep (or drops the pair with a logged note when even the
original disagrees with itself).

Expect it to take a while (every existing tile is loaded, saved and unloaded;
cancellable). Watch the log tail: `[Repair] Re-save finished: N tile(s)
written, M failed.` Any failure prints its per-tile diagnostics right above.

The 39 at-risk tiles from the October 24 session (all are repaired by the
re-save; `azeroth_32_49` - the crash tile - is among them):

    azeroth_29_52 azeroth_30_47 azeroth_30_48 azeroth_30_49 azeroth_30_50 azeroth_31_41
    azeroth_31_47 azeroth_31_48 azeroth_31_49 azeroth_31_50 azeroth_31_51 azeroth_31_52
    azeroth_32_47 azeroth_32_48 azeroth_32_49 azeroth_32_53 azeroth_33_48 azeroth_33_49
    azeroth_33_54 azeroth_34_39 azeroth_34_48 azeroth_34_49 azeroth_34_51 azeroth_35_38
    azeroth_35_52 azeroth_36_38 azeroth_36_49 azeroth_36_50 azeroth_36_51 azeroth_36_52
    azeroth_37_39 azeroth_37_40 azeroth_38_42 azeroth_39_38 azeroth_39_39 azeroth_39_40
    azeroth_40_39 azeroth_40_40 azeroth_41_39

## Step 2 - Regenerate horizon data: Editor -> "Generate new WDL"

The re-save rewrote every tile; the `.wdl` (distant-terrain LOD the client
loads before the tiles themselves) must be rebuilt from the new heights or
the horizon will disagree with the terrain.

## Step 3 - Export: Editor -> "Export map for client..."

The exporter now audits every `_obj0`/`_obj1` it packages
(`validateObjectCompanions`): any surviving companion mismatch is named in
the log and the export warnings with the remedy. After Step 1 there should be
**zero** "inconsistent companion chunks" warnings - if any appear, re-save
that tile (its diagnostics say why) and export again.

## Step 4 - Enforce the single-source rule in Patches\

The Epsilon addon serves ANY loose file under `Patches\` by remapped
FileDataID, so a stale duplicate wins as often as the fresh export does. The
October 24 layout held four conflicting sources next to the real one:

| Folder | October 24 content | Verdict |
| --- | --- | --- |
| `Patches\azeroth\` | 3,359 files, 839 tiles + wdt/wdl/lgt.wdt + patch.json | **the install target - keep** |
| `Patches\testnewmap\` | 57 flat partial files, incl. a copy of azeroth_32_49 | quarantine - stale duplicates of saved tiles |
| `Patches\world\` | 64 strays | quarantine |
| `Patches\!Chronicles-Azeroth(3.0.3)\` | 843 files, 210 azeroth tiles | quarantine while editing azeroth - same map id, foreign files |

Run **`scripts\cleanup_epsilon_patches.bat`** (default `G:\epsilon\Patches`,
or pass the path). It *moves* - never deletes - every conflicting folder and
loose `.adt/.wdt/.wdl/.blp` into `Patches\_quarantine\<timestamp>\`, then
lists what remains. `testaz` (the October 23 equivalent of `testnewmap`) is
covered too.

## Step 5 - Deploy and redo the lost edits

Copy the fresh export into `Patches\azeroth\world\maps\azeroth\` as usual,
then verify in-game (fly the Elwynn/32_49 area first).

The 169 tiles below **never persisted any edits** - every save attempt died
on the OBJ1 LOD gate and the MCIN-less fallback ("Split ADT save failed ...
ROOT lacks MHDR/MCIN", 969 events = 169 tiles x ~5.7 rounds). Both gates are
fixed: their retail layouts (one-sided LOD sets, ROOTs without MCIN) now save
natively. Whatever was edited on these tiles during that session has to be
redone - and will now stick:

    azeroth_27_38 azeroth_27_39 azeroth_27_40 azeroth_27_41 azeroth_27_42 azeroth_27_43
    azeroth_27_45 azeroth_27_46 azeroth_27_47 azeroth_27_48 azeroth_27_49 azeroth_27_50
    azeroth_28_38 azeroth_28_39 azeroth_28_40 azeroth_28_41 azeroth_28_42 azeroth_28_43
    azeroth_28_44 azeroth_28_45 azeroth_28_46 azeroth_28_47 azeroth_28_48 azeroth_28_49
    azeroth_28_50 azeroth_28_51 azeroth_28_52 azeroth_28_53 azeroth_29_38 azeroth_29_39
    azeroth_29_40 azeroth_29_41 azeroth_29_42 azeroth_29_43 azeroth_29_44 azeroth_29_45
    azeroth_29_46 azeroth_29_47 azeroth_29_48 azeroth_29_49 azeroth_29_50 azeroth_29_51
    azeroth_29_53 azeroth_29_54 azeroth_30_38 azeroth_30_39 azeroth_30_40 azeroth_30_41
    azeroth_30_42 azeroth_30_43 azeroth_30_44 azeroth_30_45 azeroth_30_46 azeroth_30_51
    azeroth_30_52 azeroth_30_53 azeroth_30_54 azeroth_30_55 azeroth_31_38 azeroth_31_39
    azeroth_31_40 azeroth_31_42 azeroth_31_43 azeroth_31_44 azeroth_31_45 azeroth_31_46
    azeroth_31_53 azeroth_31_54 azeroth_31_55 azeroth_32_39 azeroth_32_40 azeroth_32_41
    azeroth_32_42 azeroth_32_43 azeroth_32_44 azeroth_32_45 azeroth_32_46 azeroth_32_50
    azeroth_32_51 azeroth_32_52 azeroth_32_54 azeroth_33_39 azeroth_33_40 azeroth_33_41
    azeroth_33_42 azeroth_33_43 azeroth_33_44 azeroth_33_45 azeroth_33_46 azeroth_33_47
    azeroth_33_50 azeroth_33_51 azeroth_33_52 azeroth_33_53 azeroth_34_38 azeroth_34_40
    azeroth_34_41 azeroth_34_42 azeroth_34_43 azeroth_34_44 azeroth_34_45 azeroth_34_46
    azeroth_34_47 azeroth_34_50 azeroth_34_52 azeroth_35_39 azeroth_35_40 azeroth_35_41
    azeroth_35_42 azeroth_35_43 azeroth_35_44 azeroth_35_46 azeroth_35_47 azeroth_35_48
    azeroth_35_49 azeroth_35_50 azeroth_35_51 azeroth_36_39 azeroth_36_40 azeroth_36_41
    azeroth_36_42 azeroth_36_43 azeroth_36_44 azeroth_36_47 azeroth_36_48 azeroth_37_38
    azeroth_37_41 azeroth_37_42 azeroth_37_43 azeroth_37_44 azeroth_37_45 azeroth_37_46
    azeroth_38_38 azeroth_38_39 azeroth_38_40 azeroth_38_41 azeroth_38_43 azeroth_38_44
    azeroth_38_45 azeroth_38_46 azeroth_38_47 azeroth_39_41 azeroth_39_42 azeroth_39_43
    azeroth_39_44 azeroth_39_45 azeroth_39_46 azeroth_39_47 azeroth_40_41 azeroth_40_42
    azeroth_40_43 azeroth_40_44 azeroth_40_45 azeroth_40_46 azeroth_40_47 azeroth_41_40
    azeroth_41_41 azeroth_41_42 azeroth_41_43 azeroth_41_44 azeroth_41_45 azeroth_41_46
    azeroth_41_47 azeroth_42_42 azeroth_42_43 azeroth_42_44 azeroth_42_45 azeroth_42_46
    azeroth_42_47

## Problem ledger (everything the field sessions surfaced)

| Problem | Status |
| --- | --- |
| 169 tiles refused to save (all-chunks OBJ1 LOD gate) | **fixed** `d55aa8a` - side-aware LOD rewrite, harness-proven |
| Fallback refused MCIN-less retail ROOTs | **fixed** `d55aa8a` - MCIN synthesized with pre-insertion basis calibration |
| OBJ0 companions stale next to rebuilt MODF (the 32_49 AV) | **fixed** `d55aa8a` - lockstep companion rebuild |
| 39 saved tiles carry corrupted companions on disk | **repair tool** `b89f8c2` - Step 1 above |
| Misleading "Native ROOT writer failed" label | **fixed** `d55aa8a` - names the real step |
| Corrupt files shipped silently by the exporter | **fixed** `f03f917` - export-time companion audit + summary |
| ERROR #134 `UnpackAlphaShadowBits(): Bad genformat` (Oct 23) | **fixed** `f7d9206` - 8-bit alpha + MCNK flag ownership |
| Stale flat/foreign Patches folders serving old files | **tooling** - `scripts\cleanup_epsilon_patches.bat`, Step 4 |
| Export `_lod` failure at 37_23 (fdid 1286788) | environmental - the client CASC does not hold that file; export warns and continues, the client tolerates the absent LOD |
| `searchMaxUID` gaps on 21 tiles | environmental - obj0 content CASC does not serve; scan is MAID-first since `d15864c` |
| Exit-time DeferredDelete exception | benign - normal-exit shutdown order, closed earlier |

## Verification

The Qt-free layers are pinned by the format harness (323 checks, 0 failed at
`b89f8c2`), including regression proofs: reinstating the old all-chunks LOD
gate fails the one-sided-layout tests, and a pre-fix-style corrupt obj0
draws the export warning. Build command: see `MODERN_ADT_SPEC_AUDIT.md`,
"Verifying any of the above".
