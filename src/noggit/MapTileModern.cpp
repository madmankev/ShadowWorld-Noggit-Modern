// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapHeaders.h>
#include <noggit/formats/adt/ModernAdtReader.hpp>
#include <noggit/TextureManager.h>
#include <noggit/texture_set.hpp>

#include <ClientFile.hpp>
#include <Listfile.hpp>

#include <QtCore/QSettings>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <stdexcept>
#include <vector>

namespace
{
  constexpr std::uint32_t on_disk_fourcc(char a, char b, char c, char d)
  {
    return (static_cast<std::uint32_t>(a) << 24)
         | (static_cast<std::uint32_t>(b) << 16)
         | (static_cast<std::uint32_t>(c) << 8)
         | static_cast<std::uint32_t>(d);
  }

  std::string split_adt_path(std::string const& root_path, std::string const& suffix)
  {
    auto const extension = root_path.rfind(".adt");
    if (extension == std::string::npos)
      return root_path + suffix;
    return root_path.substr(0, extension) + suffix + ".adt";
  }

  struct ModernHeightDiagnostic
  {
    bool found_mcvt = false;
    float raw_header_zpos = 0.0f;
    float raw_header_xpos = 0.0f;
    float raw_header_ypos = 0.0f;
    float mcvt_min = 0.0f;
    float mcvt_max = 0.0f;
    std::vector<float> heights;
    std::vector<float> first_heights;
  };

  ModernHeightDiagnostic inspect_modern_height_data(
    char const* file_data,
    std::size_t file_size,
    std::size_t mcnk_offset)
  {
    ModernHeightDiagnostic result;

    if (mcnk_offset + 8 + sizeof(MapChunkHeader) > file_size)
      return result;

    MapChunkHeader raw_header{};
    std::memcpy(&raw_header, file_data + mcnk_offset + 8, sizeof(raw_header));
    result.raw_header_zpos = raw_header.zpos;
    result.raw_header_xpos = raw_header.xpos;
    result.raw_header_ypos = raw_header.ypos;

    std::uint32_t declared_size = 0;
    std::memcpy(&declared_size, file_data + mcnk_offset + 4, sizeof(declared_size));

    auto const payload_begin = mcnk_offset + 8;
    auto const payload_end = payload_begin + declared_size;
    if (payload_end > file_size || declared_size < 128)
      return result;

    std::size_t sub = payload_begin + 128;
    while (sub + 8 <= payload_end)
    {
      std::uint32_t magic = 0;
      std::uint32_t size = 0;
      std::memcpy(&magic, file_data + sub, sizeof(magic));
      std::memcpy(&size, file_data + sub + 4, sizeof(size));

      auto const data_begin = sub + 8;
      if (size > payload_end - data_begin)
        break;

      if (magic == on_disk_fourcc('M', 'C', 'V', 'T') && size >= 145 * sizeof(float))
      {
        result.found_mcvt = true;
        result.mcvt_min = std::numeric_limits<float>::max();
        result.mcvt_max = std::numeric_limits<float>::lowest();
        result.heights.reserve(145);
        result.first_heights.reserve(10);

        for (std::size_t i = 0; i < 145; ++i)
        {
          float height = 0.0f;
          std::memcpy(&height, file_data + data_begin + i * sizeof(float), sizeof(height));
          result.heights.push_back(height);
          result.mcvt_min = std::min(result.mcvt_min, height);
          result.mcvt_max = std::max(result.mcvt_max, height);
          if (i < 10)
            result.first_heights.push_back(height);
        }
        break;
      }

      sub = data_begin + size;
    }

    return result;
  }

  void log_modern_height_diagnostic(
    MapTile const* tile,
    MapChunk const* chunk,
    std::size_t chunk_index,
    ModernHeightDiagnostic const& raw)
  {
    float final_min_y = std::numeric_limits<float>::max();
    float final_max_y = std::numeric_limits<float>::lowest();
    for (auto const& vertex : chunk->mVertices)
    {
      final_min_y = std::min(final_min_y, vertex.y);
      final_max_y = std::max(final_max_y, vertex.y);
    }

    std::ostringstream heights;
    heights << std::fixed << std::setprecision(3);
    for (std::size_t i = 0; i < raw.first_heights.size(); ++i)
    {
      if (i)
        heights << ',';
      heights << raw.first_heights[i];
    }

    LogDebug << "[ModernADT][HeightDiag] tile=" << tile->index.x << ',' << tile->index.z
             << " chunk=" << chunk_index
             << " grid(parsed)=" << chunk->px << ',' << chunk->py
             << " rawHeaderPos(z,x,y)=" << raw.raw_header_zpos << ','
             << raw.raw_header_xpos << ',' << raw.raw_header_ypos
             << " MapChunkBase(x,y,z)=" << chunk->xbase << ',' << chunk->ybase << ',' << chunk->zbase
             << " directMCVT=" << (raw.found_mcvt ? "yes" : "no")
             << " MCVT[min,max]=" << raw.mcvt_min << ',' << raw.mcvt_max
             << " finalY[min,max]=" << final_min_y << ',' << final_max_y
             << " first10={" << heights.str() << "}"
             << std::endl;
  }

  void rebuild_modern_chunk_geometry(
    MapChunk* chunk,
    MapTile const* tile,
    ModernHeightDiagnostic const& raw)
  {
    if (chunk->px < 0 || chunk->px >= 16 || chunk->py < 0 || chunk->py >= 16)
      throw std::runtime_error("Shadowlands ROOT MCNK contains an invalid chunk grid coordinate.");

    if (!raw.found_mcvt || raw.heights.size() != 145)
      throw std::runtime_error("Shadowlands ROOT MCNK is missing a valid 145-float MCVT subchunk.");

    chunk->xbase = tile->xbase + static_cast<float>(chunk->px) * CHUNKSIZE;
    chunk->zbase = tile->zbase + static_cast<float>(chunk->py) * CHUNKSIZE;
    chunk->ybase = 0.0f;

    auto* vertex = chunk->mVertices;
    std::size_t height_index = 0;
    float min_y = std::numeric_limits<float>::max();
    float max_y = std::numeric_limits<float>::lowest();

    for (int row = 0; row < 17; ++row)
    {
      for (int column = 0; column < ((row % 2) ? 8 : 9); ++column)
      {
        float local_x = static_cast<float>(column) * UNITSIZE;
        float const local_z = static_cast<float>(row) * 0.5f * UNITSIZE;
        if (row % 2)
          local_x += UNITSIZE * 0.5f;

        float const y = raw.raw_header_ypos + raw.heights[height_index++];
        vertex->x = chunk->xbase + local_x;
        vertex->y = y;
        vertex->z = chunk->zbase + local_z;
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
        ++vertex;
      }
    }

    chunk->vmin = glm::vec3(chunk->xbase, min_y, chunk->zbase);
    chunk->vmax = glm::vec3(chunk->xbase + 8.0f * UNITSIZE,
                            max_y,
                            chunk->zbase + 8.0f * UNITSIZE);
    chunk->vcenter = (chunk->vmin + chunk->vmax) * 0.5f;
  }

  glm::vec3 get_modern_tile_local_neighbor(MapTile* tile, MapChunk* chunk, int vertex_index, unsigned dir)
  {
    constexpr float half_unit = UNITSIZE / 2.f;
    static constexpr std::array<float, 4> xdiff{-half_unit, half_unit, half_unit, -half_unit};
    static constexpr std::array<float, 4> zdiff{-half_unit, -half_unit, half_unit, half_unit};

    float const vertex_x = chunk->mVertices[vertex_index].x + xdiff[dir];
    float const vertex_z = chunk->mVertices[vertex_index].z + zdiff[dir];
    TileIndex const neighbor_tile({vertex_x, 0.0f, vertex_z});

    if (neighbor_tile.x == tile->index.x && neighbor_tile.z == tile->index.z)
    {
      glm::vec3 result{};
      tile->getVertexInternal(vertex_x, vertex_z, &result);
      return result;
    }

    // Loading modern tiles happens in parallel (normally a ~5x5 neighborhood).
    // Never query another tile from an AsyncLoader worker here: another tile may
    // still be waiting on this one. Mirror Noggit's unloaded-neighbor fallback
    // instead and keep the edge height flat until a later seam refresh exists.
    return {vertex_x, chunk->mVertices[vertex_index].y, vertex_z};
  }

  void recalc_modern_tile_local_normals(MapTile* tile, MapChunk* chunk)
  {
    auto& tile_buffer = tile->getChunkHeightmapBuffer();
    int const chunk_start = (chunk->px * 16 + chunk->py) * mapbufsize * 4;

    for (int i = 0; i < mapbufsize; ++i)
    {
      glm::vec3 const P1 = get_modern_tile_local_neighbor(tile, chunk, i, 0);
      glm::vec3 const P2 = get_modern_tile_local_neighbor(tile, chunk, i, 1);
      glm::vec3 const P3 = get_modern_tile_local_neighbor(tile, chunk, i, 2);
      glm::vec3 const P4 = get_modern_tile_local_neighbor(tile, chunk, i, 3);

      glm::vec3 const N1 = glm::cross(P2 - chunk->mVertices[i], P1 - chunk->mVertices[i]);
      glm::vec3 const N2 = glm::cross(P3 - chunk->mVertices[i], P2 - chunk->mVertices[i]);
      glm::vec3 const N3 = glm::cross(P4 - chunk->mVertices[i], P3 - chunk->mVertices[i]);
      glm::vec3 const N4 = glm::cross(P1 - chunk->mVertices[i], P4 - chunk->mVertices[i]);

      glm::vec3 norm = glm::normalize(N1 + N2 + N3 + N4);
      norm.x = std::floor(norm.x * 127.0f) / 127.0f;
      norm.y = std::floor(norm.y * 127.0f) / 127.0f;
      norm.z = std::floor(norm.z * 127.0f) / 127.0f;

      int const pixel_start = chunk_start + i * 4;
      tile_buffer[pixel_start] = -norm.z;
      tile_buffer[pixel_start + 1] = norm.y;
      tile_buffer[pixel_start + 2] = -norm.x;
    }

    chunk->requeueChunkUpdate(ChunkUpdateFlags::NORMALS);
  }

  //! Opens the TEX0 part of a split ADT and decodes it through the shared
  //! Formats::ADT reader. Any failure degrades to "no texture data" (an
  //! empty ModernTex0Data): the tile's terrain still loads, and every
  //! problem is reported with the [ModernADT][TexDiag] tag.
  Noggit::Formats::ADT::ModernTex0Data load_modern_tex0(std::string const& root_path,
                                                        int tile_x,
                                                        int tile_z,
                                                        bool verbose)
  {
    Noggit::Formats::ADT::ModernTex0Data no_textures;
    auto const tex_path = split_adt_path(root_path, "_tex0");

    try
    {
      BlizzardArchive::Listfile::FileKey key(tex_path);
      BlizzardArchive::ClientFile tex_file(
        key, Noggit::Application::NoggitApplication::instance()->clientData());

      auto result = Noggit::Formats::ADT::parseTex0(tex_file.getBuffer(), tex_file.getSize());

      for (auto const& issue : result.issues)
      {
        switch (issue.severity)
        {
          case Noggit::Formats::ADT::AdtIssueSeverity::error:
            LogError << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
                     << ": " << issue.message << std::endl;
            break;
          case Noggit::Formats::ADT::AdtIssueSeverity::warning:
            Log << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
                << ": " << issue.message << std::endl;
            break;
          case Noggit::Formats::ADT::AdtIssueSeverity::info:
            LogDebug << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
                     << ": " << issue.message << std::endl;
            break;
        }
      }

      if (!result.ok)
      {
        LogError << "[ModernADT] TEX0 '" << tex_path << "' could not be decoded for tile "
                 << tile_x << ',' << tile_z << "; the tile loads without terrain textures."
                 << std::endl;
        return no_textures;
      }

      LogDebug << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
               << " TEX0 diffuseIDs=" << result.data.diffuse_ids.size()
               << " heightIDs=" << result.data.height_ids.size()
               << " MCNK=" << result.data.chunks.size() << std::endl;

      if (verbose)
      {
        for (std::size_t chunk_index = 0;
             chunk_index < std::min<std::size_t>(4, result.data.chunks.size());
             ++chunk_index)
        {
          auto const& tex_chunk = result.data.chunks[chunk_index];
          std::ostringstream layers;
          for (std::size_t i = 0; i < tex_chunk.layers.size(); ++i)
          {
            auto const& layer = tex_chunk.layers[i];
            if (i) layers << ' ';
            layers << '[' << i << ":tex=" << layer.texture_id
                   << ",fdid=" << layer.file_data_id
                   << ",flags=0x" << std::hex << layer.flags << std::dec
                   << ",alpha=" << layer.alpha_offset
                   << ",effect=" << layer.effect_id << ']';
          }
          LogDebug << "[ModernADT][TexDiag] tile=" << tile_x << ',' << tile_z
                   << " chunk=" << chunk_index
                   << " MCLY entries=" << tex_chunk.layers.size()
                   << " MCAL bytes=" << tex_chunk.mcal.size()
                   << " :: " << layers.str() << std::endl;
        }
      }

      return std::move(result.data);
    }
    catch (std::exception const& e)
    {
      LogError << "[ModernADT] Unable to open/read TEX0 '" << tex_path << "' for tile "
               << tile_x << ',' << tile_z << ": " << e.what()
               << "; the tile loads without terrain textures." << std::endl;
      return no_textures;
    }
    catch (...)
    {
      LogError << "[ModernADT] Unable to open/read TEX0 '" << tex_path << "' for tile "
               << tile_x << ',' << tile_z << " (unknown error)"
               << "; the tile loads without terrain textures." << std::endl;
      return no_textures;
    }
  }


  //! What binding a chunk's MCLY rows to textures produced.
  //!
  //! Returning a bare count hid the difference between "this chunk has no
  //! texture layers" and "this chunk's layers could not be resolved", so the
  //! per-tile summary reported `0 failed chunk(s)` for tiles that came up
  //! completely untextured.
  struct modern_texture_bind_result
  {
    std::size_t layers_bound = 0;
    //! Layers whose FileDataID the project listfile could not resolve.
    std::size_t unresolved_layers = 0;
    //! Layers abandoned because an earlier one in the same chunk failed. They
    //! cannot be kept: MCLY indices must stay aligned with the texture slots.
    std::size_t dropped_layers = 0;
    //! First FileDataID that could not be resolved, for the aggregated report.
    std::uint32_t first_unresolved_fdid = 0;
  };

  modern_texture_bind_result bind_modern_texture_layers(MapChunk* chunk,
                                                        Noggit::Formats::ADT::ModernTexChunk const& tex_chunk,
                                                        Noggit::NoggitRenderContext context,
                                                        bool emit_diag,
                                                        std::size_t chunk_index)
  {
    auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
    modern_texture_bind_result result;
    auto& bound = result.layers_bound;

    for (std::size_t layer_index = 0; layer_index < tex_chunk.layers.size(); ++layer_index)
    {
      auto const& source_layer = tex_chunk.layers[layer_index];
      auto const texture_path = client_data->listfile()->getPath(source_layer.file_data_id);
      if (texture_path.empty())
      {
        // Deliberately silent here: this is a property of the source data, not
        // of the chunk, and every tile has 256 chunks reporting the same
        // FileDataIDs. It used to produce one line per layer per chunk -
        // 18,000+ identical errors for a single map load, which buried every
        // real problem in the log. The tile summary reports it once.
        ++result.unresolved_layers;
        if (!result.first_unresolved_fdid)
          result.first_unresolved_fdid = source_layer.file_data_id;
        result.dropped_layers += tex_chunk.layers.size() - layer_index - 1;
        break;
      }

      BlizzardArchive::Listfile::FileKey const texture_key(texture_path,
                                                            source_layer.file_data_id);
      int const added_layer = chunk->addTexture(scoped_blp_texture_reference(texture_key, context));
      if (added_layer != static_cast<int>(layer_index))
      {
        LogError << "[ModernADT] Modern terrain texture layer order mismatch: expected "
                 << layer_index << ", got " << added_layer << "." << std::endl;
        result.dropped_layers += tex_chunk.layers.size() - layer_index - 1;
        break;
      }

      auto* info = chunk->texture_set->getMCLYEntries();
      info[layer_index].flags = source_layer.flags;
      info[layer_index].effectID = source_layer.effect_id;

      if (layer_index < 4)
      {
        if (source_layer.has_mtxp)
        {
          // MTXP height texturing parameters decoded by the shared reader
          // (scale/height scale/height offset), converted to the render-side
          // structure here.
          texture_heightmapping_data mapping;
          mapping.uvScale = source_layer.mtxp_uv_scale;
          mapping.heightScale = source_layer.mtxp_height_scale;
          mapping.heightOffset = source_layer.mtxp_height_offset;
          chunk->texture_set->setHeightMappingData(layer_index, mapping);
        }

        if (source_layer.has_height_id_table || source_layer.disable_height_texture)
        {
          std::unique_ptr<scoped_blp_texture_reference> height_texture;
          if (!source_layer.disable_height_texture && source_layer.height_file_data_id)
          {
            auto const height_path = client_data->listfile()->getPath(source_layer.height_file_data_id);
            if (!height_path.empty())
            {
              BlizzardArchive::Listfile::FileKey const height_key(height_path,
                                                                  source_layer.height_file_data_id);
              height_texture = std::make_unique<scoped_blp_texture_reference>(height_key, context);
            }
            else
            {
              // 256 chunks per tile reference the same ids: one line per id,
              // not one per chunk.
              static std::unordered_set<std::uint32_t> reported_height_ids;
              if (reported_height_ids.insert(source_layer.height_file_data_id).second)
                LogError << "[ModernADT] Unable to resolve height texture FileDataID "
                         << source_layer.height_file_data_id
                         << " through the project listfile; layers using it render with"
                            " classic alpha blending." << std::endl;
            }
          }
          chunk->texture_set->setHeightTextureOverride(layer_index, std::move(height_texture));
        }
      }
      ++bound;

      if (layer_index > 0 && (source_layer.flags & FLAG_USE_ALPHA))
      {
        std::vector<std::uint8_t> alpha;
        if (!Noggit::Formats::ADT::decodeModernAlpha(tex_chunk, source_layer, layer_index, alpha))
        {
          LogError << "[ModernADT] Failed to decode MCAL for chunk " << chunk_index
                   << " layer " << layer_index
                   << " offset " << source_layer.alpha_offset
                   << " flags=0x" << std::hex << source_layer.flags << std::dec << std::endl;
          continue;
        }

        auto& alphamaps = *chunk->texture_set->getAlphamaps();
        if (!alphamaps[layer_index - 1])
          alphamaps[layer_index - 1] = std::make_unique<Alphamap>();
        alphamaps[layer_index - 1]->setAlpha(alpha.data());

        if (emit_diag)
        {
          auto const minmax = std::minmax_element(alpha.begin(), alpha.end());
          auto const nonzero = std::count_if(alpha.begin(), alpha.end(), [](std::uint8_t v) { return v != 0; });
          LogDebug << "[ModernADT][AlphaDiag] chunk=" << chunk_index
                   << " layer=" << layer_index
                   << " min=" << static_cast<int>(*minmax.first)
                   << " max=" << static_cast<int>(*minmax.second)
                   << " nonzero=" << nonzero << "/4096"
                   << (source_layer.flags & FLAG_ALPHA_COMPRESSED ? " compressed" : " raw")
                   << std::endl;
        }
      }
    }

    chunk->requeueChunkUpdate(ChunkUpdateFlags::ALPHAMAP);
    return result;
  }
}

void MapTile::finishLoadingShadowlandsTerrainOnly()
{
  if (finished)
    return;

  BlizzardArchive::ClientFile root_file(
    _file_key,
    Noggit::Application::NoggitApplication::instance()->clientData());

  auto const* data = root_file.getBuffer();
  auto const file_size = root_file.getSize();

  // One shared scan: top-level walk, MCNK offsets and MVER/MHDR/MFBO
  // extraction - the same reader the AsyncLoader gate uses, so gate and
  // loader can never disagree. Detailed per-MCNK subchunk validation runs
  // in the gate (which reports it); repeating it here would double-log
  // every anomaly without changing the outcome.
  auto scan = Noggit::Formats::ADT::scanAdt(
    data, file_size, Noggit::Formats::ADT::AdtScanOptions{true, false, 0, "ROOT"});

  for (auto const& issue : scan.issues)
  {
    switch (issue.severity)
    {
      case Noggit::Formats::ADT::AdtIssueSeverity::error:
        LogError << "[ModernADT] tile " << index.x << ',' << index.z
                 << ": " << issue.message << std::endl;
        break;
      case Noggit::Formats::ADT::AdtIssueSeverity::warning:
        Log << "[ModernADT] tile " << index.x << ',' << index.z
            << ": " << issue.message << std::endl;
        break;
      case Noggit::Formats::ADT::AdtIssueSeverity::info:
        LogDebug << "[ModernADT] tile " << index.x << ',' << index.z
                 << ": " << issue.message << std::endl;
        break;
    }
  }

  if (scan.mcnk_offsets.size() != 256)
  {
    LogError << "[ModernADT] Terrain loader expected 256 ROOT MCNK chunks but found "
             << scan.mcnk_offsets.size() << " in '" << _file_key.stringRepr() << "'."
             << std::endl;
    throw std::runtime_error("Shadowlands terrain loader requires exactly 256 ROOT MCNK chunks.");
  }

  // MHDR: the tile flag word (bit 0 announces MFBO) is also what the save
  // path writes back, so tiles loaded without it used to lose their flags
  // on the next save.
  if (scan.has_mhdr)
    mFlags = static_cast<int>(scan.mhdr.flags);
  else
    LogError << "[ModernADT] tile " << index.x << ',' << index.z
             << ": ROOT has no MHDR chunk; tile flags default to 0." << std::endl;

  if (mFlags & 1)
  {
    if (scan.mfbo_valid)
    {
      float const xPositions[] = { xbase, xbase + 266.0f, xbase + 533.0f };
      float const yPositions[] = { zbase, zbase + 266.0f, zbase + 533.0f };

      for (int fy = 0; fy < 3; ++fy)
      {
        for (int fx = 0; fx < 3; ++fx)
        {
          int const pos = fx + fy * 3;
          // fix bug with old noggit version inverting values (as in the legacy loader)
          auto const bounds = std::minmax(scan.mfbo.minimum[pos], scan.mfbo.maximum[pos]);
          mMinimumValues[pos] = { xPositions[fx], static_cast<float>(bounds.first), yPositions[fy] };
          mMaximumValues[pos] = { xPositions[fx], static_cast<float>(bounds.second), yPositions[fy] };
        }
      }
    }
    else
    {
      LogError << "[ModernADT] tile " << index.x << ',' << index.z
               << ": MHDR flag 0x1 announces MFBO flight bounds but no valid MFBO"
                  " chunk was found; the bounds stay empty." << std::endl;
    }
  }

  QSettings settings;
  bool const verbose = settings.value("modern_adt_verbose_diagnostics", false).toBool();

  Noggit::Formats::ADT::ModernTex0Data tex0_data;
  if (_file_key.hasFilepath())
    tex0_data = load_modern_tex0(_file_key.filepath(), index.x, index.z, verbose);

  LogDebug << "[ModernADT] Building MapTile " << index.x << ',' << index.z
           << " from 256 ROOT MCNK chunks"
           << (tex0_data.chunks.empty() ? " without TEX0 texture data"
                                        : " with TEX0 MCLY layers and MCAL blending")
           << "." << std::endl;

  std::size_t textured_chunks = 0;
  std::size_t texture_layers_bound = 0;
  std::size_t failed_chunks = 0;
  //! Chunks whose MCLY rows could not be turned into textures (unresolvable
  //! FileDataIDs). Counted separately from failed_chunks, which only covers
  //! chunks that threw while loading.
  std::size_t untextured_chunks = 0;
  std::size_t unresolved_layers = 0;
  std::size_t dropped_layers = 0;
  std::uint32_t first_unresolved_fdid = 0;
  std::string first_chunk_failure;

  for (std::size_t next_chunk = 0; next_chunk < scan.mcnk_offsets.size(); ++next_chunk)
  {
    unsigned const x = static_cast<unsigned>(next_chunk / 16);
    unsigned const z = static_cast<unsigned>(next_chunk % 16);

    try
    {
      auto const raw_height =
        inspect_modern_height_data(data, file_size, scan.mcnk_offsets[next_chunk]);
      root_file.seek(scan.mcnk_offsets[next_chunk]);

      mChunks[x][z] = std::make_unique<MapChunk>(
        this, &root_file, mBigAlpha, _mode, _context, false, 0, false, true);

      auto* chunk = mChunks[x][z].get();
      rebuild_modern_chunk_geometry(chunk, this, raw_height);

      bool const emit_diag = verbose && next_chunk < 4;
      if (emit_diag)
        log_modern_height_diagnostic(this, chunk, next_chunk, raw_height);

      if (next_chunk < tex0_data.chunks.size())
      {
        auto const bind_result = bind_modern_texture_layers(
          chunk, tex0_data.chunks[next_chunk], _context, emit_diag, next_chunk);
        texture_layers_bound += bind_result.layers_bound;
        if (bind_result.layers_bound)
          ++textured_chunks;
        else if (!tex0_data.chunks[next_chunk].layers.empty())
          ++untextured_chunks;

        unresolved_layers += bind_result.unresolved_layers;
        dropped_layers += bind_result.dropped_layers;
        if (!first_unresolved_fdid)
          first_unresolved_fdid = bind_result.first_unresolved_fdid;
      }
    }
    catch (std::exception const& e)
    {
      ++failed_chunks;
      if (first_chunk_failure.empty())
        first_chunk_failure = e.what();

      // Insert a flat placeholder instead of losing the whole tile: the
      // other chunks stay editable, and every consumer keeps finding a
      // valid chunk (with an empty texture set) at each grid position.
      // init_empty derives px = idx % 16 / py = idx / 16, so pass the
      // transposed index to land at px = x, py = z.
      int const placeholder_idx =
        static_cast<int>((next_chunk % 16) * 16 + next_chunk / 16);
      mChunks[x][z] = std::make_unique<MapChunk>(
        this, &root_file, mBigAlpha, _mode, _context, true, placeholder_idx, false, true);

      auto* placeholder = mChunks[x][z].get();
      if (!placeholder->texture_set)
      {
        placeholder->texture_set = std::make_unique<TextureSet>(
          placeholder, &root_file, 0, mBigAlpha, false, true, _context, MapChunkHeader{});
      }
    }
  }

  if (unresolved_layers > 0)
  {
    LogError << "[ModernADT] tile " << index.x << ',' << index.z << ": "
             << unresolved_layers << " terrain texture layer(s) reference FileDataID "
             << first_unresolved_fdid << " (and possibly others) that the project"
                " listfile cannot resolve; " << untextured_chunks
             << "/256 chunk(s) are left without terrain textures"
             << (dropped_layers ? ", " + std::to_string(dropped_layers)
                                 + " further layer(s) dropped with them" : "")
             << ". This is a data problem: the source TEX0/MCLY rows store id 0,"
                " or the CASC listfile for this client build is incomplete."
             << std::endl;
  }

  if (failed_chunks > 0)
  {
    LogError << "[ModernADT] tile " << index.x << ',' << index.z << ": "
             << failed_chunks << "/256 chunk(s) failed to load (first error: "
             << first_chunk_failure
             << "); flat placeholders were inserted. Repair or re-save the source ADT."
             << std::endl;
  }

  LogDebug << "[ModernADT] Recalculating tile-local terrain normals for tile "
           << index.x << ',' << index.z
           << " without cross-tile streaming dependencies." << std::endl;

  for (unsigned x = 0; x < 16; ++x)
  {
    for (unsigned z = 0; z < 16; ++z)
      recalc_modern_tile_local_normals(this, mChunks[x][z].get());
  }

  for (unsigned x = 0; x < 16; ++x)
  {
    for (unsigned z = 0; z < 16; ++z)
      _renderer.initChunkData(mChunks[x][z].get());
  }

  mTextureFilenames.clear();
  _mtxf_entries.clear();
  _textures_finished_loading = true;
  _objects_finished_loading = true;

  root_file.close();
  recalcExtents();

  finished = true;
  _tile_is_being_reloaded = false;
  _state_changed.notify_all();

  LogDebug << "[ModernADT] Shadowlands tile loaded: " << index.x << ',' << index.z
           << ". Direct MCVT terrain, TEX0 textures on " << textured_chunks
           << "/256 chunks (" << texture_layers_bound << " layers bound, "
           << untextured_chunks << " untextured, "
           << failed_chunks << " failed chunk(s)); normals recalculated"
              " tile-locally for streaming safety." << std::endl;
}
