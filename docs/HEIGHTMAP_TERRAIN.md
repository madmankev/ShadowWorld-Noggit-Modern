# Generating terrain from an image (heightmap import)

One grayscale image → terrain. Three levels exist in this fork, from a single
tile up to a whole map, and they all share the same pixel convention so an
image exported at one level can be imported at another.

## The region importer (one image over many tiles)

**Assist → Import → "Import heightmap (one image over tile range)"**

This is the "generate terrain out of an image" path: a heightmap from a
terrain generator (World Machine, Gaea, Blender...), a paint program, or a
satellite DEM is stretched over an inclusive rectangle of ADT tiles and the
terrain is written from it.

The dialog:

| Field | Meaning |
| --- | --- |
| Whole map / Tile range | Which tiles the image covers. Tile indices are the `X_Y` from the filenames (`azeroth_32_49` is X=32, Z=49), 0..63 each axis. |
| Min / Max Height | World heights the black and white ends of the image map to. Vertex height = `min + (max - min) * pixel`. |
| Mode | **Set** replaces the terrain (generation), **Add**/**Subtract**/**Multiply** blend the image into the existing terrain. |

After Okay you pick the image file (PNG, BMP, JPEG, TGA, TIFF, WebP -
anything Qt reads). Every tile in the range is then loaded, resampled,
applied, **saved immediately** and unloaded again unless it was already open
in the editor. A progress dialog allows canceling; tiles written before the
cancel stay written. When anything was written, the editor offers to
regenerate the horizon data (`.wdl`) right away - answer yes, or do it later
via **Editor → Generate new WDL**, *before* **Export map for client...**:
the export ships the `.wdl` from disk, and a stale one makes distant terrain
in the client disagree with what was imported.

### Image orientation and size

* The image's **top-left corner is the first tile** of the range (x0, z0);
  image **right is +X**, image **down is +Z** - the same orientation as the
  per-tile PNGs from **Assist → Export → Heightmap**.
* A tile's height grid is 257×257 vertices and neighbouring tiles **share
  their border vertices**, so a range of N×M tiles maps 1:1 from an image of
  **(N·256+1) × (M·256+1)** pixels. Whole map: **16385×16385**.
* Any other size works: the image is resampled bilinearly. Sizes above the
  1:1 dimension are downscaled first - extra pixels could never reach a
  vertex.
* Tile seams are continuous at every image size: a border vertex's source
  coordinate depends only on its region-wide position, so both sides of a
  seam sample bit-identical values.
* 16-bit grayscale sources keep their precision (the importer works in
  16-bit and writes through the 16-bit path of the tile height setter);
  8-bit sources quantize to 256 height levels, which shows as terracing on
  gentle slopes. Color images are converted by luminance.
* Pixel values are clamped: anything at or above white becomes Max Height,
  anything at or below black becomes Min Height.

### Suggested workflow for a new landmass

1. **Map Creation Wizard** - create and register the map (WDT, databases,
   WDL prompt).
2. Open the new map, fly to its origin tile.
3. **Assist → Import → Import heightmap (one image over tile range)** with
   Mode = Set, Min/Max = the height span your image encodes.
4. Accept the `.wdl` regeneration prompt.
5. Texturing/doodads as usual, then **Editor → Export map for client...**.

## The per-tile importers (existing)

* **Assist → Import → Import heightmap (file)** - one 257×257 image onto the
  tile under the camera (any size is rescaled to 257×257).
* **Assist → Import → Import heightmaps** - whole map from per-tile PNGs
  named `<map>_<X>_<Z>_height.png` in the project's `world/maps/<map>/`
  folder, exactly what **Assist → Export → Export heightmap** (current tile)
  and **Export → heightmap** (all tiles) write.

## Implementation map

* `src/noggit/formats/heightmap/HeightmapImport.{hpp,cpp}` - Qt-free core:
  `HeightmapBuffer` (decoded 0..1 luminance), `HeightmapRegion` (inclusive
  tile rectangle), `sliceTileFromHeightmap` (bilinear, clamped, seamless),
  `idealHeightmapDimension`. Pinned by the format harness
  (`tools/adt_format_check.cpp`, "heightmap region slicing" section):

      mkdir -p /tmp/synth && g++ -std=c++20 -I src -I src/external \
        -o /tmp/synth/harness tools/adt_format_check.cpp \
        src/noggit/formats/heightmap/HeightmapImport.cpp \
        src/noggit/formats/adt/ModernAdtWriter.cpp \
        src/noggit/formats/adt/ModernAdtReader.cpp \
        src/noggit/formats/wdt/WdtReader.cpp \
        src/noggit/formats/wdl/ModernWdlWriter.cpp \
        src/noggit/formats/export/MapExport.cpp \
        src/noggit/formats/conversion/AdtConversion.cpp \
        src/noggit/formats/conversion/MapConversion.cpp
      /tmp/synth/harness        # 308 checks, 0 failed

* `World::importHeightmapRegion` (`src/noggit/World.cpp`) - load → slice →
  `MapTile::setHeightmapImage` (RGBA64, the precise path) → `saveTile` →
  unload-if-transient, with progress and cancel. Saved tiles go through the
  regular native split-ADT writer, including the LOD/companion handling from
  `MODERN_ADT_SPEC_AUDIT.md`.
* `src/noggit/MapView.cpp` - the dialog, image decoding (16-bit grayscale),
  downscale guard, and the `.wdl` regeneration prompt.
