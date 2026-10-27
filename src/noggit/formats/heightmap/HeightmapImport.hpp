// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

//! Turning one grayscale image into terrain heights over a rectangle of ADT
//! tiles.
//!
//! The editor already imports one 257x257 image per tile (World::
//! importADTHeightmap) and a whole map from pre-sliced per-tile PNGs
//! (World::importAllADTsHeightmaps). What was missing is the natural authoring
//! workflow: a single heightmap - from a terrain generator, a paint program or
//! a satellite DEM - stretched over an arbitrary tile range, generating the
//! terrain in one pass. This header is the Qt-free half of that: image
//! decoding stays with the caller (QImage), the slicing/resampling math lives
//! here so the standalone format harness can pin it down.
//!
//! Orientation is the one the existing per-tile "Export heightmap" PNGs use:
//! the image's top-left corner maps to the region's first tile (x0, z0),
//! image right is +tile X, image down is +tile Z, and inside a tile the
//! 257x257 vertex grid is addressed exactly like MapTile::setHeightmapImage
//! addresses its input image (chunk (k, l) owns pixels (k*16 .. k*16+16,
//! l*16 .. l*16+16)).
namespace Noggit::Formats::Heightmap
{
  //! A decoded grayscale image: row-major luminance samples in 0..1, row 0 is
  //! the top of the image. Samples outside 0..1 are clamped during slicing,
  //! so HDR sources only need a normalization pass by the caller.
  struct HeightmapBuffer
  {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<float> samples; //! width * height entries

    [[nodiscard]] bool valid() const noexcept
    {
      return width != 0 && height != 0 && samples.size() == width * height;
    }
  };

  //! An inclusive rectangle of ADT tile indices - the same X_Y numbers the
  //! tile filenames carry (azeroth_32_49 is tile x=32, z=49).
  struct HeightmapRegion
  {
    std::uint32_t x0 = 0;
    std::uint32_t z0 = 0;
    std::uint32_t x1 = 0;
    std::uint32_t z1 = 0;

    //! Only meaningful when valid() - the difference underflows otherwise.
    [[nodiscard]] std::uint32_t tiles_x() const noexcept { return x1 - x0 + 1; }
    [[nodiscard]] std::uint32_t tiles_z() const noexcept { return z1 - z0 + 1; }
    [[nodiscard]] std::uint64_t tile_count() const noexcept
    {
      return valid() ? std::uint64_t(tiles_x()) * tiles_z() : 0;
    }
    [[nodiscard]] bool valid() const noexcept;
  };

  //! Height vertices along one tile edge: 16 chunks of 16 units plus the
  //! shared border vertex. A tile's heightmap image is 257x257 for the same
  //! reason (MapTile::getHeightmapImage).
  inline constexpr std::size_t heightmap_tile_side = 257;

  //! The image dimension that maps 1:1 onto `tiles` tiles per row/column:
  //! tiles * 256 + 1, because neighbouring tiles share the border vertex.
  //! A whole-map image is therefore 16385x16385; anything else is resampled.
  [[nodiscard]] std::size_t idealHeightmapDimension(std::uint32_t tiles) noexcept;

  //! Resample the part of `image` that belongs to tile (tile_x, tile_z) into
  //! a row-major 257x257 grid of 0..1 samples (out_samples is cleared first).
  //!
  //! Bilinear with clamped edges. The source coordinate of a vertex is a
  //! pure function of its region-wide vertex index, so the right column of
  //! one tile and the left column of its neighbour sample the exact same
  //! coordinates: seams are bit-identical at any image size, and terrain
  //! generated across tiles is continuous.
  //!
  //! Returns false (leaving out_samples empty) when the buffer or region is
  //! invalid or the tile is outside the region.
  [[nodiscard]] bool sliceTileFromHeightmap(HeightmapBuffer const& image,
                                            HeightmapRegion const& region,
                                            std::uint32_t tile_x,
                                            std::uint32_t tile_z,
                                            std::vector<float>& out_samples);
}
