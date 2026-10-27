// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/formats/heightmap/HeightmapImport.hpp>

#include <algorithm>

namespace Noggit::Formats::Heightmap
{
  bool HeightmapRegion::valid() const noexcept
  {
    return x1 >= x0 && z1 >= z0 && x1 < 64 && z1 < 64;
  }

  std::size_t idealHeightmapDimension(std::uint32_t tiles) noexcept
  {
    return tiles ? std::size_t(tiles) * (heightmap_tile_side - 1) + 1 : 0;
  }

  namespace
  {
    float sample_bilinear(HeightmapBuffer const& image, float u, float v)
    {
      u = std::clamp(u, 0.f, static_cast<float>(image.width - 1));
      v = std::clamp(v, 0.f, static_cast<float>(image.height - 1));

      auto const u0 = static_cast<std::size_t>(u);
      auto const v0 = static_cast<std::size_t>(v);
      std::size_t const u1 = std::min(u0 + 1, image.width - 1);
      std::size_t const v1 = std::min(v0 + 1, image.height - 1);

      float const du = u - static_cast<float>(u0);
      float const dv = v - static_cast<float>(v0);

      float const s00 = image.samples[v0 * image.width + u0];
      float const s10 = image.samples[v0 * image.width + u1];
      float const s01 = image.samples[v1 * image.width + u0];
      float const s11 = image.samples[v1 * image.width + u1];

      return (s00 * (1.f - du) + s10 * du) * (1.f - dv)
           + (s01 * (1.f - du) + s11 * du) * dv;
    }
  }

  bool sliceTileFromHeightmap(HeightmapBuffer const& image,
                              HeightmapRegion const& region,
                              std::uint32_t tile_x,
                              std::uint32_t tile_z,
                              std::vector<float>& out_samples)
  {
    out_samples.clear();

    if (!image.valid() || !region.valid())
      return false;
    if (tile_x < region.x0 || tile_x > region.x1 || tile_z < region.z0 || tile_z > region.z1)
      return false;

    // Vertex steps across the whole region, so neighbouring tiles land on
    // identical source coordinates at their shared border. One multiply per
    // vertex keeps that bit-exact; computing a per-tile base and adding to it
    // would drift by an ULP and leave a hairline seam.
    std::uint64_t const steps_x = std::uint64_t(region.tiles_x()) * (heightmap_tile_side - 1);
    std::uint64_t const steps_z = std::uint64_t(region.tiles_z()) * (heightmap_tile_side - 1);
    float const span_u = static_cast<float>(image.width - 1) / static_cast<float>(steps_x);
    float const span_v = static_cast<float>(image.height - 1) / static_cast<float>(steps_z);

    std::uint64_t const base_col = std::uint64_t(tile_x - region.x0) * (heightmap_tile_side - 1);
    std::uint64_t const base_row = std::uint64_t(tile_z - region.z0) * (heightmap_tile_side - 1);

    out_samples.resize(heightmap_tile_side * heightmap_tile_side);
    for (std::size_t row = 0; row < heightmap_tile_side; ++row)
    {
      float const v = static_cast<float>(base_row + row) * span_v;
      for (std::size_t col = 0; col < heightmap_tile_side; ++col)
      {
        float const u = static_cast<float>(base_col + col) * span_u;
        out_samples[row * heightmap_tile_side + col] = std::clamp(sample_bilinear(image, u, v), 0.f, 1.f);
      }
    }
    return true;
  }
}
