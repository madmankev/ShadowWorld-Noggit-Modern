// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/frustum.hpp>

#include <noggit/tool_enums.hpp>
#include <noggit/WMO.h>
#include <noggit/MapHeaders.h>

#include <opengl/texture.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.fwd.hpp>

#include <QtGui/QImage>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class MapIndex;
class MapTile;
class MapView;
class World;

namespace Noggit
{

struct map_horizon_tile
{
    int16_t height_17[17][17];
    int16_t height_16[16][16];
    int16_t holes[16];
};

struct map_horizon_batch
{
  map_horizon_batch ()
    : vertex_start (0)
    , vertex_count (0)
  {}

  map_horizon_batch (uint32_t _vertex_start, uint32_t _vertex_count)
    : vertex_start(_vertex_start)
    , vertex_count(_vertex_count)
  {}

  uint32_t vertex_start;
  uint32_t vertex_count;
};

class map_horizon
{
public:
  struct render
  {
    render(const map_horizon& horizon);

    void draw(glm::mat4x4 const& model_view
             , glm::mat4x4 const& projection
             , MapIndex *index
             , const glm::vec3& color
             , const float& cull_distance
             , const math::frustum& frustum
             , const glm::vec3& camera 
             , display_mode display
             );

    map_horizon_batch _batches[64][64];

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vaos;
    GLuint const& _vao = _vaos[0];
    OpenGL::Scoped::buffers<2> _buffers;
    GLuint const& _index_buffer = _buffers[0];
    GLuint const& _vertex_buffer = _buffers[1];
    std::unique_ptr<OpenGL::program> _map_horizon_program;
  };

  class minimap : public OpenGL::texture
  {
  public:
    minimap(const map_horizon& horizon);
  };

  map_horizon(const std::string& basename, World * const world);

  void update_minimap_tile(int y, int x, bool has_data);

  void set_minimap(const MapIndex* const index, bool set_empty = false);

  void remove_horizon_tile(int y, int x);

  Noggit::map_horizon_tile* get_horizon_tile(int y, int x);

  QImage _qt_minimap;

  void update_horizon_tile(MapTile* mTile);

  void save_wdl(World* world, bool regenerate = false);

  bool wmoHasLowRes(WMOInstance* instance);
  // void updateWmoLowRes(WMOInstance* instance);

  // note : access those two with ENTRY_MODF.nameID
  // TODO make a proper structure instead of raw access to wow chunks
  std::vector<std::string> mWMOFilenames;
  std::vector<ENTRY_MODF> lWMOInstances;

  std::vector<scoped_wmo_reference> wmos;

private:
  int16_t getWdlheight(MapTile* tile, float x, float y);

  std::string _filename;

  std::unique_ptr<map_horizon_tile> _tiles[64][64];

  //! Raw image of the WDL as loaded, plus the absolute payload offsets of
  //! each tile's MARE/MAHO chunk inside it (0 = tile has no chunk). Modern
  //! WDLs carry chunks the legacy writer cannot reproduce (MLDD/MLDX/MLMD/
  //! MLMX placements, MSSN/... sky scenes, MAOE ocean data), so saving a
  //! modern map patches the height payloads inside this image and preserves
  //! every other chunk byte for byte.
  std::vector<std::uint8_t> _raw_wdl;
  std::array<std::uint32_t, 4096> _mare_payload_offsets{};
  std::array<std::uint32_t, 4096> _maho_payload_offsets{};
  bool _wdl_loaded_from_disk = false;
};

}
