// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <string>
#include <Exception.hpp>
#include <ClientFile.hpp>
#include <Listfile.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/AsyncObject.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/errorHandling.h>
#include <noggit/Log.h>
#include <noggit/MapTile.h>
#include <noggit/formats/adt/ModernAdtReader.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <util/exception_to_string.hpp>

#include <QtCore/QSettings>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <list>
#include <sstream>
#include <vector>

namespace
{
  std::string async_object_name(AsyncObject const* object)
  {
    if (!object)
      return "<null>";

    auto const& file_key = object->file_key();
    return file_key.hasFilepath() ? file_key.filepath() : std::to_string(file_key.fileDataID());
  }

  bool string_ends_with(std::string const& value, std::string const& suffix)
  {
    return value.size() >= suffix.size()
      && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  bool is_root_adt_path(std::string const& path)
  {
    if (!string_ends_with(path, ".adt"))
      return false;

    return !string_ends_with(path, "_tex0.adt")
      && !string_ends_with(path, "_obj0.adt")
      && !string_ends_with(path, "_obj1.adt")
      && !string_ends_with(path, "_lod.adt");
  }

  bool is_shadowlands_root_adt(std::string const& path)
  {
    auto* project = Noggit::Project::CurrentProject::get();
    return project
      && (project->projectVersion == Noggit::Project::ProjectVersion::SL
          || project->projectVersion == Noggit::Project::ProjectVersion::RETAIL
          || project->projectVersion == Noggit::Project::ProjectVersion::FOREVER)
      && is_root_adt_path(path);
  }

  std::string split_adt_path(std::string const& root_path, std::string const& suffix)
  {
    auto const extension = root_path.rfind(".adt");
    if (extension == std::string::npos)
      return root_path + suffix;

    return root_path.substr(0, extension) + suffix + ".adt";
  }

  enum class ModernAdtRoute
  {
    refuse,         // ROOT is unusable: do not attempt to load the tile at all.
    legacy_loader,  // File is a legacy single-file ADT: route to finishLoading().
    modern_terrain  // Modern split ROOT: route to finishLoadingShadowlandsTerrainOnly().
  };

  void log_modern_adt_issues(std::string const& path,
                             std::vector<Noggit::Formats::ADT::AdtIssue> const& issues)
  {
    for (auto const& issue : issues)
    {
      switch (issue.severity)
      {
        case Noggit::Formats::ADT::AdtIssueSeverity::error:
          LogError << "[ModernADT] '" << path << "': " << issue.message << std::endl;
          break;
        case Noggit::Formats::ADT::AdtIssueSeverity::warning:
          Log << "[ModernADT] '" << path << "': " << issue.message << std::endl;
          break;
        case Noggit::Formats::ADT::AdtIssueSeverity::info:
          LogDebug << "[ModernADT] '" << path << "': " << issue.message << std::endl;
          break;
      }
    }
  }

  bool load_modern_adt_bytes(std::string const& path,
                             std::vector<std::uint8_t>& out,
                             std::string const& label)
  {
    try
    {
      auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();
      BlizzardArchive::Listfile::FileKey key(path);
      BlizzardArchive::ClientFile file(key, client_data);
      out.assign(reinterpret_cast<std::uint8_t const*>(file.getBuffer()),
                 reinterpret_cast<std::uint8_t const*>(file.getBuffer()) + file.getSize());
      return true;
    }
    catch (std::exception const& e)
    {
      LogError << "[ModernADT] Unable to open the " << label << " file '" << path
               << "': " << e.what() << std::endl;
      return false;
    }
    catch (...)
    {
      LogError << "[ModernADT] Unable to open the " << label << " file '" << path
               << "': unknown error." << std::endl;
      return false;
    }
  }

  // Decides which loader a root ADT goes to and reports every layout problem
  // found on the way. Scanning/validation lives in the Formats::ADT reader so
  // the gate and the terrain loader share one implementation (and one set of
  // unit tests).
  //
  // A tile with a usable ROOT is no longer refused because of TEX0 or OBJ0
  // problems: it loads degraded (without textures / without object
  // placements) so terrain work can continue, which is exactly what the
  // "stabilize ADT loading" baseline asks for.
  ModernAdtRoute run_shadowlands_split_adt_gate(std::string const& root_path)
  {
    std::vector<std::uint8_t> root_bytes;
    if (!load_modern_adt_bytes(root_path, root_bytes, "ROOT"))
      return ModernAdtRoute::refuse;

    auto const root = Noggit::Formats::ADT::scanAdt(
      root_bytes, Noggit::Formats::ADT::AdtScanOptions{true, true, 8, "ROOT"});
    log_modern_adt_issues(root_path, root.issues);
    for (auto const& [chunk, count] : root.unknown_chunks)
      LogDebug << "[ModernADT] ROOT '" << root_path << "': unknown chunk "
               << Noggit::Formats::ADT::adtChunkName(chunk) << " x" << count
               << ", skipping" << std::endl;

    auto const layout = Noggit::Formats::ADT::classifyAdtLayout(root);

    LogDebug << "[ModernADT] ROOT '" << root_path << "': layout="
             << Noggit::Formats::ADT::adtLayoutName(layout)
             << " MCNK=" << root.mcnk_offsets.size()
             << " structurally_valid=" << (root.structurally_valid ? "yes" : "no")
             << std::endl;

    if (layout == Noggit::Formats::ADT::AdtLayout::legacy_single_file)
    {
      LogDebug << "[ModernADT] '" << root_path
               << "' is a legacy single-file ADT (MTEX/MMDX/MWMO name lists present);"
                  " routing it to the legacy loader." << std::endl;
      return ModernAdtRoute::legacy_loader;
    }

    if (layout != Noggit::Formats::ADT::AdtLayout::modern_split_root)
      return ModernAdtRoute::refuse;

    auto const tex0_path = split_adt_path(root_path, "_tex0");
    std::vector<std::uint8_t> part_bytes;
    if (load_modern_adt_bytes(tex0_path, part_bytes, "TEX0"))
    {
      auto const tex0 = Noggit::Formats::ADT::scanAdt(
        part_bytes, Noggit::Formats::ADT::AdtScanOptions{false, true, 8, "TEX0"});
      log_modern_adt_issues(tex0_path, tex0.issues);
      if (!tex0.structurally_valid || tex0.mcnk_offsets.size() != 256)
        LogError << "[ModernADT] TEX0 '" << tex0_path << "' is unusable (MCNK="
                 << tex0.mcnk_offsets.size() << ", valid="
                 << (tex0.structurally_valid ? "yes" : "no")
                 << "); the tile will load without terrain textures." << std::endl;
    }
    else
    {
      LogError << "[ModernADT] The tile '" << root_path
               << "' will load without terrain textures." << std::endl;
    }

    auto const obj0_path = split_adt_path(root_path, "_obj0");
    part_bytes.clear();
    if (load_modern_adt_bytes(obj0_path, part_bytes, "OBJ0"))
    {
      auto const obj0 = Noggit::Formats::ADT::scanAdt(
        part_bytes, Noggit::Formats::ADT::AdtScanOptions{false, false, 8, "OBJ0"});
      log_modern_adt_issues(obj0_path, obj0.issues);
      if (!obj0.structurally_valid)
        LogError << "[ModernADT] OBJ0 '" << obj0_path
                 << "' is unusable; the tile will load without model/WMO placements."
                 << std::endl;
    }
    else
    {
      LogError << "[ModernADT] The tile '" << root_path
               << "' will load without model/WMO placements." << std::endl;
    }

    return ModernAdtRoute::modern_terrain;
  }
}

AsyncLoader* AsyncLoader::instance;

void AsyncLoader::setup(int threads)
{
  // make sure there's always at least one thread otherwise nothing can load
  instance = new AsyncLoader(std::max(1, threads));
}

bool AsyncLoader::is_loading()
{
  std::lock_guard<std::mutex> const lock (_guard);
  return !_currently_loading.empty();
}

void AsyncLoader::process()
{
  AsyncObject* object = nullptr;
  std::string object_name;

  QSettings settings;
  bool additional_log = settings.value("additional_file_loading_log", true).toBool();

  while (!_stop)
  {
    {
      std::unique_lock<std::mutex> lock (_guard);

      _state_changed.wait
      ( lock
      , [&]
        {
          return !!_stop || std::any_of ( _to_load.begin(), _to_load.end()
                                        , [](auto const& to_load) { return !to_load.empty(); }
                                        );
        }
      );

      if (_stop)
      {
        return;
      }

      for (auto& to_load : _to_load)
      {
        if (to_load.empty())
        {
          continue;
        }

        object = to_load.front();
        _currently_loading.emplace_back (object);
        to_load.pop_front();
        object_name = async_object_name(object);

        break;
      }
    }

    try
    {
      if (additional_log)
      {
        std::lock_guard<std::mutex> const lock(_guard);
        LogDebug << "Loading file '" << object_name << "'" << std::endl;
      }

      if (is_shadowlands_root_adt(object_name))
      {
        auto const route = run_shadowlands_split_adt_gate(object_name);

        if (route == ModernAdtRoute::refuse)
        {
          std::lock_guard<std::mutex> const lock(_guard);
          LogError << "[ModernADT] Refusing to load '" << object_name
                   << "': its ROOT part is not a usable modern or legacy ADT"
                      " (see the messages above)." << std::endl;

          if (object->is_required_when_saving())
            _important_object_failed_loading = true;

          _currently_loading.remove(object);
          object->error_on_loading();
          _state_changed.notify_all();
          continue;
        }

        if (route == ModernAdtRoute::modern_terrain)
        {
          if (auto* tile = dynamic_cast<MapTile*>(object))
          {
            tile->finishLoadingShadowlandsTerrainOnly();

            if (additional_log)
            {
              std::lock_guard<std::mutex> const lock(_guard);
              LogDebug << "[ModernADT] Loaded Shadowlands tile '"
                       << object_name << "'." << std::endl;
            }

            {
              std::lock_guard<std::mutex> const lock(_guard);
              _currently_loading.remove(object);
              _state_changed.notify_all();
            }
            continue;
          }

          {
            std::lock_guard<std::mutex> const lock(_guard);
            LogError << "[ModernADT] Verified Shadowlands root ADT was not a MapTile object: '"
                     << object_name << "'." << std::endl;
            _currently_loading.remove(object);
            object->error_on_loading();
            _state_changed.notify_all();
          }
          continue;
        }

        // ModernAdtRoute::legacy_loader: a legacy single-file ADT inside a
        // modern project falls through to the untouched legacy loader below.
      }

      object->finishLoading();

      if (additional_log)
      {
        std::lock_guard<std::mutex> const lock(_guard);
        LogDebug << "Loaded  file '" << object_name << "'" << std::endl;
      }

      {
        std::lock_guard<std::mutex> const lock (_guard);
        _currently_loading.remove (object);
        _state_changed.notify_all();
      }
    }
    catch (BlizzardArchive::Exceptions::FileReadFailedError const& e)
    {
      std::lock_guard<std::mutex> const lock(_guard);

      LogError << "Caught file read error while loading '" << object_name << "': " << e.what() << std::endl;

      if (object->is_required_when_saving())
      {
        _important_object_failed_loading = true;
      }

      _currently_loading.remove(object);
      object->error_on_loading();
      _state_changed.notify_all();
    }
    catch (...)
    {
      std::lock_guard<std::mutex> const lock(_guard);

      std::string const reason{ util::exception_to_string(std::current_exception()) };
      LogError << "Caught unknown error while loading '" << object_name << "': " << reason <<  std::endl;

      if (object->is_required_when_saving())
      {
        _important_object_failed_loading = true;
      }

      _currently_loading.remove(object);
      object->error_on_loading();
      _state_changed.notify_all();
    }
  }
}

void AsyncLoader::queue_for_load (AsyncObject* object)
{
  std::lock_guard<std::mutex> const lock (_guard);
  _to_load[(size_t)object->loading_priority()].push_back (object);
  _state_changed.notify_one();
}

void AsyncLoader::ensure_deletable (AsyncObject* object)
{
  std::unique_lock<std::mutex> lock (_guard);
  _state_changed.wait
  ( lock
  , [&]
    {
      auto& to_load = _to_load[(size_t)object->loading_priority()];
      auto const& it = std::find (to_load.begin(), to_load.end(), object);
      if (it != to_load.end())
      {
        to_load.erase(it);
        return true;
      }
      else
      {
        return std::find (_currently_loading.begin(), _currently_loading.end(), object) == _currently_loading.end();
      }
    }
  );
}

AsyncLoader::AsyncLoader(int numThreads)
  : _stop (false)
{
  for (int i = 0; i < numThreads; ++i)
  {
    _threads.emplace_back (&AsyncLoader::process, this);
  }
}

AsyncLoader::~AsyncLoader()
{
  {
    std::unique_lock<std::mutex> lock(_guard);
    _stop = true;
  }
  _state_changed.notify_all();

  for (auto& thread : _threads)
  {
    thread.join();
  }
}

bool AsyncLoader::important_object_failed_loading() const
{
  return _important_object_failed_loading;
}

void AsyncLoader::reset_object_fail()
{
  _important_object_failed_loading = false;
}
