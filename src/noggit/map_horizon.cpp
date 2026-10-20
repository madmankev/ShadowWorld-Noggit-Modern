// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "map_horizon.h"

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/map_index.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/World.h>

#include <opengl/context.hpp>
#include <opengl/context.inl>
#include <noggit/Misc.h>
#include <noggit/formats/wdl/WdlObjects.hpp>

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <sstream>
#include <vector>

struct color
{
  color(unsigned char r, unsigned char g, unsigned char b)
    : _r(r)
    , _g(g)
    , _b(b)
  {}

  uint32_t to_int() const {
    return (_b) | (_g << 8) | (_r << 16) | (uint32_t)(0xFFu << 24);
  }

  operator uint32_t () const {
    return to_int();
  }

  unsigned char _r;
  unsigned char _g;
  unsigned char _b;
};

struct ranged_color
{
  ranged_color (const color& c, const int16_t& start, const int16_t& stop)
    : _color (c)
    , _start (start)
    , _stop (stop)
  {}

  const color   _color;
  const int16_t _start;
  const int16_t _stop;
};

static inline color lerp_color(const color& start, const color& end, float t)
{
  return color ( (end._r) * t + (start._r) * (1.0 - t)
               , (end._g) * t + (start._g) * (1.0 - t)
               , (end._b) * t + (start._b) * (1.0 - t)
               );
}

static inline uint32_t color_for_height (int16_t height)
{
  static const ranged_color colors[] =
    { ranged_color (color (20, 149, 7), 0, 600)
    , ranged_color (color (137, 84, 21), 600, 1200)
    , ranged_color (color (96, 96, 96), 1200, 1600)
    , ranged_color (color (255, 255, 255), 1600, 0x7FFF)
    };
  static const size_t num_colors (sizeof (colors) / sizeof (ranged_color));

  if (height < colors[0]._start)
  {
    return color (0, 0, 255 + std::max (height / 2.0, -255.0));
  }
  else if (height >= colors[num_colors - 1]._start)
  {
    return colors[num_colors - 1]._color;
  }

  float t (1.0);
  size_t correct_color (num_colors - 1);

  for (size_t i (0); i < num_colors - 1; ++i)
  {
    if (height >= colors[i]._start && height < colors[i]._stop)
    {
      t = float(height - colors[i]._start) / float (colors[i]._stop - colors[i]._start);
      correct_color = i;
      break;
    }
  }

  return lerp_color(colors[correct_color]._color,
                    colors[std::min(correct_color + 1, num_colors - 1)]._color,
                    std::clamp(t, 0.0f, 1.0f));
}
namespace Noggit
{

namespace
{
  //! Renders a chunk id as its four character name, or as a hex value when the
  //! bytes are not printable text.
  std::string wdl_chunk_name(std::uint32_t fourcc)
  {
    char name[5] = {};
    bool printable = true;

    for (int index = 0; index < 4; ++index)
    {
      auto const character = static_cast<char>((fourcc >> (24 - 8 * index)) & 0xFFu);
      if (std::isprint(static_cast<unsigned char>(character)) == 0)
        printable = false;
      name[index] = character;
    }

    if (!printable)
    {
      std::ostringstream hex;
      hex << "0x" << std::hex << std::uppercase << fourcc;
      return hex.str();
    }

    return std::string(name, 4);
  }
}

map_horizon::map_horizon(const std::string& basename, World * const world)
{
  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << ".wdl";
  _filename = filename.str();

  if (!Application::NoggitApplication::instance()->clientData()->exists(_filename))
  {
    LogError << "file \"World\\Maps\\" << basename << "\\" << basename << ".wdl\" does not exist." << std::endl;
    set_minimap(&world->mapIndex);
    return;
  }

  BlizzardArchive::ClientFile wdl_file (_filename, Application::NoggitApplication::instance()->clientData());

  // Keep the raw image plus the MARE/MAHO payload offsets so modern WDLs
  // can be saved by patching heights in place instead of being rebuilt with
  // the legacy writer (which drops the ML*/sky scene/ocean chunks).
  _raw_wdl.assign(wdl_file.getBuffer(), wdl_file.getBuffer() + wdl_file.getSize());
  _wdl_loaded_from_disk = true;

  uint32_t fourcc;
  uint32_t size;

  bool done = false;

  // chunks that are valid, but unused, and chunks that are not valid at all
  std::map<uint32_t, unsigned int> skipped_chunks;
  std::set<uint32_t> unknown_chunks;

  // Legion+ horizon WMO placements (MLMD) and their bounds (MLMX); parsed
  // into mWMOFilenames/lWMOInstances after the chunk walk.
  std::vector<uint8_t> mlmd_payload;
  std::vector<uint8_t> mlmx_payload;

  try
  {
  do
  {
    wdl_file.read(&fourcc, 4);
    wdl_file.read(&size, 4);

    switch (fourcc)
    {
      case 'MVER':
      {
        uint32_t version = 0;
        wdl_file.read(&version, 4);
        if (size != 4 || version != 18)
        {
          LogError << "[WDL Load] \"" << _filename << "\": unexpected WDL version "
                   << version << " (MVER chunk size " << size << ", expected 18/4)"
                   << std::endl;
        }

        break;
      }

      case 'MWMO':
      {
        {
          // TODO : use WMID instead for proper string parsing.

            char const* lCurPos = reinterpret_cast<char const*>(wdl_file.getPointer());
            char const* lEnd = lCurPos + size;
        
            while (lCurPos < lEnd)
            {
                mWMOFilenames.push_back(BlizzardArchive::ClientData::normalizeFilenameInternal(std::string(lCurPos)));
                lCurPos += strlen(lCurPos) + 1;
            }
        }
        wdl_file.seekRelative(size);
        break;
      }
      case 'MWID':
          // TODO
          wdl_file.seekRelative(size); // jump to end of chunk
          break;
      case 'MODF':
      {
        ENTRY_MODF const* modf_ptr = reinterpret_cast<ENTRY_MODF const*>(wdl_file.getPointer());
        for (unsigned int i = 0; i < size / sizeof(ENTRY_MODF); ++i)
        {
            lWMOInstances.push_back(modf_ptr[i]);
            if (lWMOInstances[i].scale == 0.0f)
              lWMOInstances[i].scale = 1024.0f;
        }
        
        wdl_file.seekRelative(size); // jump to end of chunk
        break;
      }
      case 'MAOF':
      {
        if (size != 64 * 64 * sizeof(uint32_t))
        {
          LogError << "[WDL Load] \"" << _filename << "\": MAOF chunk has unexpected size "
                   << size << " (expected " << 64 * 64 * sizeof(uint32_t)
                   << "); falling back to tile index minimap" << std::endl;
          done = true;
          break;
        }

        uint32_t mare_offsets[64][64];
        wdl_file.read(mare_offsets, 64 * 64 * sizeof(uint32_t));

        // - MARE and MAHO by offset ---------------------------
        for (size_t y(0); y < 64; ++y)
        {
          for (size_t x(0); x < 64; ++x)
          {
            if (!mare_offsets[y][x])
            {
              continue;
            }

            if (mare_offsets[y][x] + 8 > wdl_file.getSize())
            {
              LogError << "[WDL Load] \"" << _filename << "\": MAOF entry (" << x << "," << y
                       << ") points past the end of the file; tile skipped" << std::endl;
              continue;
            }

            wdl_file.seek(mare_offsets[y][x]);
            wdl_file.read(&fourcc, 4);
            wdl_file.read(&size, 4);

            if (fourcc != 'MARE')
            {
              LogError << "[WDL Load] \"" << _filename << "\": MAOF entry (" << x << "," << y
                       << ") points at " << wdl_chunk_name(fourcc)
                       << " instead of MARE; tile skipped" << std::endl;
              continue;
            }

            if (size != 0x442)
            {
              LogError << "[WDL Load] \"" << _filename << "\": MARE chunk at offset "
                       << mare_offsets[y][x] << " has unexpected size " << size
                       << " (expected 1090); tile skipped" << std::endl;
              continue;
            }

            if (mare_offsets[y][x] + 8 + size > wdl_file.getSize())
            {
              LogError << "[WDL Load] \"" << _filename << "\": MARE chunk at offset "
                       << mare_offsets[y][x] << " extends past the end of the file; tile skipped"
                       << std::endl;
              continue;
            }

            _mare_payload_offsets[y * 64 + x] = static_cast<uint32_t>(wdl_file.getPos());
            _tiles[y][x] = std::make_unique<map_horizon_tile>();

            wdl_file.read(_tiles[y][x]->height_17, 17 * 17 * sizeof(int16_t));
            wdl_file.read(_tiles[y][x]->height_16, 16 * 16 * sizeof(int16_t));

            if (wdl_file.getPos() + 8 <= wdl_file.getSize())
            {
                auto const pos_before_maho = wdl_file.getPos();
                wdl_file.read(&fourcc, 4);
                if (fourcc == 'MAHO')
                {
                    wdl_file.read(&size, 4);
                    if (size == 0x20 && wdl_file.getPos() + size <= wdl_file.getSize())
                    {
                        _maho_payload_offsets[y * 64 + x] = static_cast<uint32_t>(wdl_file.getPos());
                        wdl_file.read(_tiles[y][x]->holes, 16 * sizeof(int16_t));
                    }
                    else
                    {
                        LogError << "[WDL Load] \"" << _filename << "\": MAHO chunk for tile ("
                                 << x << "," << y << ") has unexpected size " << size
                                 << " (expected 32); hole masks skipped" << std::endl;
                    }
                }
                else
                {
                    // Not a MAHO chunk - restore the position, the 4 bytes
                    // belong to whatever follows this tile.
                    wdl_file.seek(pos_before_maho);
                }
            }

          }
        }
        done = true;
        break;
      }
      case 'MLMD':
      case 'MLMX':
      {
        // Legion+ horizon WMO placements (MLMD) and their bounds (MLMX),
        // referencing models by FileDataID. Captured here, converted into
        // mWMOFilenames/lWMOInstances after the chunk walk.
        if (wdl_file.getPos() > wdl_file.getSize() || size > wdl_file.getSize() - wdl_file.getPos())
        {
          LogError << "[WDL Load] invalid " << wdl_chunk_name(fourcc)
                   << " chunk size in \"" << _filename << "\", skipping rest of file"
                   << std::endl;
          done = true;
          break;
        }
        auto const* ml_payload = reinterpret_cast<uint8_t const*>(wdl_file.getPointer());
        (fourcc == 'MLMD' ? mlmd_payload : mlmx_payload).assign(ml_payload, ml_payload + size);
        wdl_file.seekRelative(size);
        break;
      }
      default:
      {
        // Legion and later WDL files store the world models, doodads, sky scenes
        // and ocean data in chunks that Noggit does not need for the low detail
        // map: only the MARE/MAHO height tiles reached through MAOF are used.
        // Skipping them silently keeps the log readable, they are not errors.
        switch (fourcc)
        {
          case 'MLDD': // M2 placements
          case 'MLDX': // M2 bounds
          case 'MLDL': // M2 lod
          case 'MLDF': // M2 fade ranges
          case 'MLMB': // WMO blend/mask
          case 'MSSN': // sky scene
          case 'MSSC': // sky scene condition
          case 'MSSO': // sky scene object
          case 'MSSF': // sky scene object fade
          case 'MSLD': // living world scene definition
          case 'MSLI': // living world scene index
          case 'MAID': // file data ids
          case 'MWAT': // water
          case 'MAOE': // ocean
          case 'MAOC': // occlusion
          case 'MARE': // height tile, read through MAOF
          case 'MAHO': // holes, read through MAOF
            ++skipped_chunks[fourcc];
            break;

          default:
            if (unknown_chunks.insert(fourcc).second)
              LogDebug << "unknown chunk in wdl \"" << _filename << "\": "
                       << wdl_chunk_name(fourcc) << " (code=" << fourcc << "), skipping "
                       << size << " bytes" << std::endl;
            break;
        }

        if (wdl_file.getPos() > wdl_file.getSize() || size > wdl_file.getSize() - wdl_file.getPos())
        {
          LogError << "invalid WDL chunk size in \"" << _filename << "\", falling back to tile index minimap" << std::endl;
          done = true;
        }
        else
        {
          wdl_file.seekRelative(size);
        }
        break;
      }
    }
  } while (!done && !wdl_file.isEof());
  }
  catch (std::exception const& e)
  {
    LogError << "failed reading WDL \"" << _filename << "\": " << e.what() << std::endl;
  }
  catch (...)
  {
    LogError << "failed reading WDL \"" << _filename << "\": unknown exception" << std::endl;
  }

  if (!skipped_chunks.empty())
  {
    std::ostringstream summary;
    unsigned int total = 0;
    bool first = true;
    for (auto const& [chunk, count] : skipped_chunks)
    {
      total += count;
      summary << (first ? "" : ", ") << wdl_chunk_name(chunk) << " x" << count;
      first = false;
    }

    LogDebug << "wdl \"" << _filename << "\": skipped " << total
             << " unsupported chunk" << (total == 1 ? "" : "s") << " (" << summary.str() << ")"
             << std::endl;
  }

  // - modern horizon WMOs (Legion+ MLMD/MLMX) --------------
  if (!mlmd_payload.empty())
  {
    auto* client_data = Application::NoggitApplication::instance()->clientData();
    BlizzardArchive::Listfile::Listfile const* listfile = client_data->listfile();

    auto objects = Noggit::Formats::WDL::parseModernWdlObjects(
      mlmd_payload, mlmx_payload,
      [listfile](std::uint32_t file_data_id)
      {
        return listfile ? listfile->getPath(file_data_id) : std::string();
      });

    for (auto const& issue : objects.issues)
    {
      switch (issue.severity)
      {
        case Noggit::Formats::WDL::IssueSeverity::error:
          LogError << "[WDL Load] \"" << _filename << "\": " << issue.message << std::endl;
          break;
        case Noggit::Formats::WDL::IssueSeverity::warning:
          Log << "[WDL Load] \"" << _filename << "\": " << issue.message << std::endl;
          break;
        case Noggit::Formats::WDL::IssueSeverity::info:
          LogDebug << "[WDL Load] \"" << _filename << "\": " << issue.message << std::endl;
          break;
      }
    }

    if (!objects.instances.empty())
    {
      auto const name_base = static_cast<uint32_t>(mWMOFilenames.size());
      for (auto const& object_name : objects.filenames)
      {
        mWMOFilenames.push_back(
          BlizzardArchive::ClientData::normalizeFilenameInternal(object_name));
      }
      for (auto& instance : objects.instances)
      {
        instance.nameID += name_base;
        lWMOInstances.push_back(instance);
      }

      Log << "[WDL Load] \"" << _filename << "\": loaded " << objects.instances.size()
          << " horizon WMO placement(s) from MLMD (" << objects.filenames.size()
          << " unique model(s))" << std::endl;
    }
    if (objects.skipped_count > 0)
    {
      Log << "[WDL Load] \"" << _filename << "\": skipped " << objects.skipped_count
          << " horizon WMO placement(s) with unresolvable FileDataIDs" << std::endl;
    }
  }

  constexpr bool _load_models = true;
  if (_load_models)
  {
    // - Load WMOs -----------------------------------------

    // Don't load them to storage, they share UIDs wth regular models

    // for rendering in unloaded tiles
    for (auto const& object : lWMOInstances)
    {
      // world->add_wmo_instance(WMOInstance(mWMOFilenames[object.nameID],
      //   &object, world->getRenderContext()), false, false);

      // auto& filepath = mWMOFilenames[object.nameID];
      // wmos.push_back(scoped_wmo_reference(filepath, world->getRenderContext()));
    }
  }

  wdl_file.close();

  set_minimap(&world->mapIndex);
}

void map_horizon::update_minimap_tile(int y, int x, bool has_data = false )
{
    if (_tiles[y][x])
    {
        //! \todo There also is a second heightmap appended which has additional 16*16 pixels.
        //! \todo There also is MAHO giving holes into this heightmap.

        for (int j(0); j < 16; ++j)
        {
            for (int i(0); i < 16; ++i)
            {
                //! \todo R and B are inverted here
                _qt_minimap.setPixel(x * 16 + i, y * 16 + j, color_for_height(_tiles[y][x]->height_17[j][i]));
            }
        }
    }
    // the adt exist but there's no data in the wdl
    else if (has_data)
    {
        for (int j(0); j < 16; ++j)
        {
            for (int i(0); i < 16; ++i)
            {
                _qt_minimap.setPixel(x * 16 + i, y * 16 + j, color(200, 100, 25));
            }
        }
    }
}

void map_horizon::set_minimap(const MapIndex* const index, bool set_empty)
{
    _qt_minimap = QImage(16 * 64, 16 * 64, QImage::Format_ARGB32);
    _qt_minimap.fill(Qt::transparent);

    if (set_empty)
        return;

    for (int y(0); y < 64; ++y)
    {
        for (int x(0); x < 64; ++x)
        {
            update_minimap_tile(y, x, index->hasTile(TileIndex(x, y)));
        }
    }
}

void map_horizon::remove_horizon_tile(int y, int x)
{
    _tiles[y][x].reset();

    for (int j(0); j < 16; ++j)
    {
        for (int i(0); i < 16; ++i)
        {
            _qt_minimap.setPixel(x * 16 + i, y * 16 + j, color(255, 25, 25));
        }
    }
}

Noggit::map_horizon_tile* map_horizon::get_horizon_tile(int y, int x)
{
    return _tiles[y][x].get();
}

int16_t map_horizon::getWdlheight(MapTile* tile, float x, float y)
{
    int cx = std::min(std::max(static_cast<int>(x / CHUNKSIZE), 0), 15);
    int cy = std::min(std::max(static_cast<int>(y / CHUNKSIZE), 0), 15);

    x -= cx * CHUNKSIZE;
    y -= cy * CHUNKSIZE;

    int row = static_cast<int>(y / (UNITSIZE * 0.5f) + 0.5f);
    int col = static_cast<int>((x - UNITSIZE * 0.5f * (row % 2)) / UNITSIZE + 0.5f);
    bool inner = (row % 2) == 1;

    if (row < 0 || col < 0 || row > 16 || col >(inner ? 8 : 9))
        return 0;

    // truncate and clamp the float value
    auto chunk = tile->getChunk(cx, cy);
    // float height = heights[cy * 16 + cx][17 * (row / 2) + (inner ? 9 : 0) + col];
    if (!chunk)
        return 0.0f;

    float height = chunk->getHeightmap()[17 * (row / 2) + (inner ? 9 : 0) + col].y;
    return std::min(std::max(static_cast<int16_t>(height), static_cast<int16_t>(SHRT_MIN)), static_cast<int16_t>(SHRT_MAX));
}

void map_horizon::update_horizon_tile(MapTile* mTile)
{
    auto tile_index = mTile->index;

    // calculate the heightmap as a short array
    float x, y;
    for (int i = 0; i < 17; i++)
    {
        for (int j = 0; j < 17; j++)
        {
            // outer - correct
            x = j * CHUNKSIZE;
            y = i * CHUNKSIZE;

            if (!_tiles[tile_index.z][tile_index.x].get()) // tile has not been initialised
                //     continue;
            {
                _tiles[tile_index.z][tile_index.x] = std::make_unique<map_horizon_tile>();
                // do we need to use memcpy as well ?
            }
            // only works for initialised
            _tiles[tile_index.z][tile_index.x].get()->height_17[i][j] = getWdlheight(mTile, x, y);

            // inner - close enough; correct values appear to use some form of averaging
            if (i < 16 && j < 16)
                _tiles[tile_index.z][tile_index.x].get()->height_16[i][j] = getWdlheight(mTile, x + CHUNKSIZE / 2.0f, y + CHUNKSIZE / 2.0f);
        }
    }
    // Holes
    for (int i = 0; i < 16; ++i)
    {
        std::bitset<16>wdlHoleMask(0);

        for (int j = 0; j < 16; ++j)
        {
            auto chunk = mTile->getChunk(j, i);
            if (!chunk)
                continue;
            // the ordering seems to be : short array = Y axis, flags values = X axis and the values are for a whole chunk.

            std::bitset<16> holeBits(chunk->getHoleMask());

            if (holeBits.count() == 16) // if all holes are set in a chunk
                wdlHoleMask.set(j, true);
        }
        _tiles[tile_index.z][tile_index.x].get()->holes[i] = static_cast<int16_t>(wdlHoleMask.to_ulong());
    }

    update_minimap_tile(tile_index.z, tile_index.x, true);
}

void map_horizon::save_wdl(World* world, bool regenerate)
{
    world->wait_for_all_tile_updates();

    std::stringstream filename;
    filename << "World\\Maps\\" << world->basename << "\\" << world->basename << ".wdl";
    //Log << "Saving WDL \"" << filename << "\"." << std::endl;

    auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
    bool const modern_client =
      client_data->version() != BlizzardArchive::ClientVersion::WOTLK;

    // Rebuilding the file with the legacy writer below drops every modern
    // chunk (MLDD/MLDX/MLMD/MLMX placements, MSSN/MSSC/MSSO sky scenes,
    // MAOE ocean data, ...) and would leave the modern client with a
    // broken WDL. When a modern WDL was loaded, patch the MARE/MAHO
    // payloads inside the loaded image instead and preserve all other
    // chunks byte for byte.
    if (modern_client && _wdl_loaded_from_disk && !_raw_wdl.empty())
    {
        if (regenerate)
        {
            // Same pre-pass the legacy writer uses: touch every existing
            // tile once so _tiles holds fresh heights.
            for (int y = 0; y < 64; ++y)
            {
                for (int x = 0; x < 64; ++x)
                {
                    TileIndex index(x, y);
                    if (!world->mapIndex.hasTile(index))
                        continue;

                    bool unload = !world->mapIndex.tileLoaded(index)
                       && !world->mapIndex.tileAwaitingLoading(index);
                    MapTile* mTile = world->mapIndex.loadTile(index, false, false, false);
                    if (!mTile)
                        continue;

                    mTile->wait_until_loaded();
                    update_horizon_tile(mTile);
                    if (unload)
                        world->mapIndex.unloadTile(index);
                }
            }
        }

        std::vector<char> patched(_raw_wdl.begin(), _raw_wdl.end());

        constexpr std::size_t height_17_bytes = 17 * 17 * sizeof(int16_t);
        constexpr std::size_t height_16_bytes = 16 * 16 * sizeof(int16_t);
        constexpr std::size_t holes_bytes = 16 * sizeof(int16_t);

        unsigned int patched_tiles = 0;
        unsigned int missing_slots = 0;

        for (std::size_t y = 0; y < 64; ++y)
        {
            for (std::size_t x = 0; x < 64; ++x)
            {
                if (!_tiles[y][x])
                    continue;

                auto const mare_offset = _mare_payload_offsets[y * 64 + x];
                if (!mare_offset)
                {
                    // The source WDL has no MARE slot for this tile, so its
                    // horizon data cannot be persisted without rebuilding
                    // the whole file (which would destroy the modern chunks).
                    ++missing_slots;
                    continue;
                }

                if (mare_offset + height_17_bytes + height_16_bytes > patched.size())
                {
                    LogError << "[WDL Save] MARE payload for tile (" << x << "," << y
                             << ") lies outside the WDL image; tile not updated" << std::endl;
                    continue;
                }

                std::memcpy(patched.data() + mare_offset,
                            _tiles[y][x]->height_17, height_17_bytes);
                std::memcpy(patched.data() + mare_offset + height_17_bytes,
                            _tiles[y][x]->height_16, height_16_bytes);

                auto const maho_offset = _maho_payload_offsets[y * 64 + x];
                if (maho_offset && maho_offset + holes_bytes <= patched.size())
                {
                    std::memcpy(patched.data() + maho_offset,
                                _tiles[y][x]->holes, holes_bytes);
                }

                ++patched_tiles;
            }
        }

        LogDebug << "[WDL Save] Patched " << patched_tiles
                 << " MARE tile(s) in the modern WDL \"" << filename.str()
                 << "\"; all other chunks preserved byte for byte." << std::endl;

        if (missing_slots > 0)
        {
            Log << "[WDL Save] " << missing_slots
                << " horizon tile(s) have no MARE slot in the source WDL \""
                << filename.str() << "\"; their horizon heights are not persisted."
                << std::endl;
        }

        BlizzardArchive::ClientFile f(filename.str(), client_data,
                                      BlizzardArchive::ClientFile::NEW_FILE);
        f.setBuffer(patched);
        f.save();
        f.close();

        _raw_wdl.assign(patched.begin(), patched.end());

        set_minimap(&world->mapIndex);
        return;
    }

    if (modern_client && !_wdl_loaded_from_disk)
    {
        Log << "[WDL Save] No source WDL was loaded for \"" << filename.str()
            << "\"; writing a legacy-format WDL. Modern clients additionally "
               "expect ML*/sky scene chunks in this file." << std::endl;
    }

    util::sExtendableArray wdlFile;
    int curPos = 0;

    // MVER
    //  {
    wdlFile.Extend(8 + 0x4);
    SetChunkHeader(wdlFile, curPos, 'MVER', 4);

    // MVER data
    *(wdlFile.GetPointer<int>(8)) = 18; // write version 18
    curPos += 8 + 0x4;
    //  }

    // WMO objects export code is copy pasta from MapTile

    struct filenameOffsetThing
    {
      int nameID;
      int filenamePosition;
    };

    filenameOffsetThing nullyThing = { 0, 0 };

    std::map<std::string, filenameOffsetThing> lObjects;

    // avoid duplicates, not really necessary here as we directly used MWMO string list
    for (auto const& filename : mWMOFilenames)
    {
      if (lObjects.find(filename) == lObjects.end())
      {
        lObjects.emplace(filename, nullyThing);
      }
    }

    int lID = 0;
    for (auto& object : lObjects)
    {
      object.second.nameID = lID++;
    }

    // MWMO
    //  {
    int lMWMO_Position = curPos;
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MWMO', 0);

    curPos += 8;

    // MWMO data
    for (auto& object : lObjects)
    {
      object.second.filenamePosition = wdlFile.GetPointer<sChunkHeader>(lMWMO_Position)->mSize;
      wdlFile.Insert(curPos, static_cast<unsigned long>(object.first.size() + 1), misc::normalize_adt_filename(object.first).c_str());
      curPos += static_cast<int>(object.first.size() + 1);
      wdlFile.GetPointer<sChunkHeader>(lMWMO_Position)->mSize += static_cast<int>(object.first.size() + 1);
      LogDebug << "Added WDL object \"" << object.first << "\"." << std::endl;
    }
    //  }

    // MWID
    //  {
    int lMWID_Size = static_cast<int>(4 * lObjects.size());
    wdlFile.Extend(8 + lMWID_Size);
    SetChunkHeader(wdlFile, curPos, 'MWID', lMWID_Size);

    // MWID data
    auto const lMWID_Data = wdlFile.GetPointer<int>(curPos + 8);

    lID = 0;
    for (auto const& object : lObjects)
      lMWID_Data[lID++] = object.second.filenamePosition;

    curPos += 8 + lMWID_Size;
    //  }

    // MODF
    //  {
    int lMODF_Size = static_cast<int>(0x40 * lWMOInstances.size());
    wdlFile.Extend(8 + lMODF_Size);
    SetChunkHeader(wdlFile, curPos, 'MODF', lMODF_Size);

    // MODF data
    auto const lMODF_Data = wdlFile.GetPointer<ENTRY_MODF>(curPos + 8);

    lID = 0;
    for (auto const& object : lWMOInstances)
    {
      auto filename_to_offset_and_name = lObjects.find(mWMOFilenames[object.nameID]);
      if (filename_to_offset_and_name == lObjects.end())
      {
        LogError << "There is a problem with saving the WDL objects. We have an object that somehow changed the name during the saving function." << std::endl;
        return;
      }

      lMODF_Data[lID] = object;
      // only need to update name id
      lMODF_Data[lID].nameID = filename_to_offset_and_name->second.nameID;
      lID++;
    }
    LogDebug << "Added " << lID << " wmos to WDL MODF" << std::endl;

    curPos += 8 + lMODF_Size;
    //  }

    //uint32_t mare_offsets[64][64] = { 0 };
    // MAOF
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MAOF', 64 * 64 * 4);
    curPos += 8;
    wdlFile.Extend(64 * 64 * 4);
    uint mareoffset = curPos + 64 * 64 * 4;

    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            TileIndex index(x, y);

            bool has_tile = world->mapIndex.hasTile(index);
            // write offset in MAOF entry
            *(wdlFile.GetPointer<uint>(curPos)) = has_tile ? mareoffset : 0;

            if (has_tile)
            {
                // MARE Header
                //  {
                wdlFile.Extend(8);
                SetChunkHeader(wdlFile, mareoffset, 'MARE', (2 * (17 * 17)) + (2 * (16 * 16))); // outer heights+inner heights
                mareoffset += 8;

                // this might be invalid if map had no WDL
                Noggit::map_horizon_tile* horizon_tile = get_horizon_tile(y, x);

                // laod tile and extract WDL data
                if (!horizon_tile || regenerate)
                {
                    bool unload = !world->mapIndex.tileLoaded(index) && !world->mapIndex.tileAwaitingLoading(index);
                    MapTile* mTile = world->mapIndex.loadTile(index, false, false, false);
                    if (!mTile)
                    {
                        continue;
                    }

                    auto nloadedtiles = world->mapIndex.getNLoadedTiles();

                    if (mTile)
                        mTile->wait_until_loaded();

                    update_horizon_tile(mTile);
                    if (unload)
                        world->mapIndex.unloadTile(index);

                    auto test = get_horizon_tile(y, x);
                    horizon_tile = get_horizon_tile(y, x);
                }
                if (!horizon_tile)
                {
                    return; // failed to generate data somehow
                    LogError << "Failed to generate the WDL file." << std::endl;
                }

                wdlFile.Insert(mareoffset, sizeof(Noggit::map_horizon_tile::height_17), reinterpret_cast<char*>(&horizon_tile->height_17));
                mareoffset += sizeof(Noggit::map_horizon_tile::height_17);
                wdlFile.Insert(mareoffset, sizeof(Noggit::map_horizon_tile::height_16), reinterpret_cast<char*>(&horizon_tile->height_16));
                mareoffset += sizeof(Noggit::map_horizon_tile::height_16);

                // MAHO (maparea holes) MAHO was added in WOTLK ?
                //  {
                wdlFile.Extend(8);
                SetChunkHeader(wdlFile, mareoffset, 'MAHO', (2 * 16)); // 1 hole mask for each chunk
                mareoffset += 8;
                wdlFile.Extend(32);
                for (int i = 0; i < 16; ++i)
                {
                    wdlFile.Insert(mareoffset, 2, (char*)&horizon_tile->holes[i]);
                    mareoffset += 2;
                }
            }
            curPos += 4;
        }
    }
    BlizzardArchive::ClientFile f(filename.str(), Noggit::Application::NoggitApplication::instance()->clientData(),
    BlizzardArchive::ClientFile::NEW_FILE);
    f.setBuffer(wdlFile.all_data());
    f.save();
    f.close();

    set_minimap(&world->mapIndex);
}

bool map_horizon::wmoHasLowRes(WMOInstance* instance)
{
  assert(instance->lowResWmo.has_value() == false);
  if (instance->lowResWmo.has_value())
    return true;

  int i = 0;
  for (auto& lowres_wmo : lWMOInstances)
  {
    if (lowres_wmo.uniqueID == instance->uid)
    {
      auto low_res_model = mWMOFilenames[lowres_wmo.nameID];

      // TODO check positions
      // need to convert coords?
      auto dir = math::degrees::vec3{ math::degrees(
        lowres_wmo.rot[0])._, math::degrees(lowres_wmo.rot[1])._, math::degrees(lowres_wmo.rot[2])._ };

      if (misc::vec3d_equals(glm::vec3(lowres_wmo.pos[0], lowres_wmo.pos[1], lowres_wmo.pos[2]), instance->pos)
        && misc::deg_vec3d_equals(dir, instance->dir)
        && misc::float_equals( (lowres_wmo.scale / 1024.0f), instance->scale))
      {
        // instance->lowResWmo = scoped_wmo_reference(low_res_model, instance->wmo->_context);
        instance->lowResInstance = &lowres_wmo;
        instance->lowResWmo = &wmos[instance->lowResInstance->nameID];

        return true;
      }
      else
      {
        assert(false);
      }
    }
    
    i++;
  }

  return false;
}

map_horizon::minimap::minimap(const map_horizon& horizon)
{
  std::vector<uint32_t> texture(1024 * 1024);

  for (size_t y (0); y < 64; ++y)
  {
    for (size_t x (0); x < 64; ++x)
    {
      if (!horizon._tiles[y][x])
        continue;

      //! \todo There also is a second heightmap appended which has additional 16*16 pixels.

      // use the (nearly) full resolution available to us.
      // the data is layed out as a triangle fans with with 17 outer values
      // and 16 midpoints per tile. which in turn means:
      //      _tiles[y][x]->height_17[16][16] == _tiles[y][x + 1]->height_17[0][0]
      for (size_t j (0); j < 16; ++j)
      {
        for (size_t i (0); i < 16; ++i)
        {
          texture[(y * 16 + j) * 1024 + x * 16 + i] = color_for_height (horizon._tiles[y][x]->height_17[j][i]);
        }
      }
    }
  }

  bind();
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_BGRA, GL_UNSIGNED_BYTE, texture.data());
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

map_horizon::render::render(const map_horizon& horizon)
{
  std::vector<glm::vec3> vertices;

  for (size_t y (0); y < 64; ++y)
  {
    for (size_t x (0); x < 64; ++x)
    {
      if (!horizon._tiles[y][x])
        continue;

      _batches[y][x] = map_horizon_batch (static_cast<std::uint32_t>(vertices.size()), 17 * 17 + 16 * 16);

      for (size_t j (0); j < 17; ++j)
      {
        for (size_t i (0); i < 17; ++i)
        {
          vertices.emplace_back ( TILESIZE * (x + i / 16.0f)
                                , horizon._tiles[y][x]->height_17[j][i]
                                , TILESIZE * (y + j / 16.0f)
                                );
        }
      }

      for (size_t j (0); j < 16; ++j)
      {
        for (size_t i (0); i < 16; ++i)
        {
          vertices.emplace_back ( TILESIZE * (x + (i + 0.5f) / 16.0f)
                                , horizon._tiles[y][x]->height_16[j][i]
                                , TILESIZE * (y + (j + 0.5f) / 16.0f)
                                );
        }
      }
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3> (_vertex_buffer, vertices, GL_STATIC_DRAW);
}

static inline uint32_t outer_index(const map_horizon_batch &batch, int y, int x)
{
  return batch.vertex_start + y * 17 + x;
};

static inline uint32_t inner_index(const map_horizon_batch &batch, int y, int x)
{
  return batch.vertex_start + 17 * 17 + y * 16 + x;
};

void map_horizon::render::draw( glm::mat4x4 const& model_view
                              , glm::mat4x4 const& projection
                              , MapIndex *index
                              , const glm::vec3& color
                              , const float& cull_distance
                              , const math::frustum& frustum
                              , const glm::vec3& camera 
                              , display_mode display
                              )
{
  std::vector<uint32_t> indices;

  const TileIndex current_index(camera);
  const int lrr = 2;

  for (size_t y (current_index.z - lrr); y <= current_index.z + lrr; ++y)
  {
    for (size_t x (current_index.x - lrr); x < current_index.x + lrr; ++x)
    {
      // x and y are unsigned so negative signed int value are positive and > 63
      if (x > 63 || y > 63)
      {
        continue;
      }

      map_horizon_batch const& batch = _batches[y][x];

      if (batch.vertex_count == 0)
        continue;

      for (int j (0); j < 16; ++j)
      {
        for (int i (0); i < 16; ++i)
        {
          // do not draw over visible chunks

          /* TODO: when this optimization is turned off, we end up with inconsistent rendering between chunks and horizon batches.
           * Potentially it is caused by inconsistent coordinate space in visibility checking or chunk update system.
          if (index->tileLoaded({y, x}) && index->getTile({y, x})->getChunk(j, i)->is_visible(cull_distance, frustum, camera, display))
          {
            //continue;
          }
          */

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i));
          indices.push_back (outer_index (batch, j + 1, i + 1));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i + 1));
          indices.push_back (outer_index (batch, j, i + 1));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j, i + 1));
          indices.push_back (outer_index (batch, j, i));
        }
      }
    }
  }

  if (_map_horizon_program)
  {
    gl.bufferSubData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(_index_buffer, 0, indices);
  }
  else
  {
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(_index_buffer, indices, GL_DYNAMIC_DRAW);

    _map_horizon_program.reset
      ( new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("horizon_vs") }
          , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("horizon_fs") }
          }
      );
  
    _vaos.upload();
  }
   

  OpenGL::Scoped::use_program shader {*_map_horizon_program.get()};

  OpenGL::Scoped::vao_binder const _ (_vao);

  shader.uniform ("model_view", model_view);
  shader.uniform ("projection", projection);
  shader.uniform ("color", glm::vec3(color.x, color.y, color.z));

  shader.attrib ("position", _vertex_buffer, 3, GL_FLOAT, GL_FALSE, 0, 0);

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder (_index_buffer);

  
  gl.drawElements (GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
}

}
