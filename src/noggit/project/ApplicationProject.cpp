#include "ApplicationProject.h"
#include "ApplicationProjectReader.h"
#include "ApplicationProjectWriter.h"
#include "ClientBuildDetection.hpp"
#include "ListfileUpdate.hpp"
#include <noggit/Log.h>

#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/World.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>

#include <blizzard-database-library/include/BlizzardDatabase.h>
#include <blizzard-archive-library/include/CASCArchive.hpp>
#include <string>
#include <blizzard-archive-library/include/Exception.hpp>
#include <blizzard-archive-library/include/ClientFile.hpp>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <exception>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <system_error>

namespace
{
  bool refreshWowdevListfile(std::filesystem::path const& project_path, std::string& error_message)
  {
    QString error;
    bool const result = Noggit::Project::refreshListfile(QString::fromStdWString(project_path.wstring()), error,
      [](QString const& message) { Log << "[Listfile] " << message.toStdString() << std::endl; });
    error_message = error.toStdString();
    return result;
  }

}
namespace Noggit::Project
{
  ApplicationProject::ApplicationProject(std::shared_ptr<Application::NoggitApplicationConfiguration> configuration)
  {
    _active_project = nullptr;
    _configuration = configuration;
  }

  void ApplicationProject::createProject(std::filesystem::path const& project_path, std::filesystem::path const& client_path, std::string const& client_version, std::string const& project_name)
  {
    if (!std::filesystem::exists(project_path))
      std::filesystem::create_directory(project_path);

    auto project = NoggitProject();
    project.ProjectName = project_name;
    project.projectVersion = ClientVersionFactory::mapToEnumVersion(client_version);
    project.ClientPath = client_path.generic_string();
    project.ProjectPath = project_path.generic_string();

    auto project_writer = ApplicationProjectWriter();
    project_writer.saveProject(&project, project_path);
  }

  std::shared_ptr<NoggitProject> ApplicationProject::loadProject(std::filesystem::path const& project_path)
  {
    ApplicationProjectReader project_reader{};
    auto project = project_reader.readProject(project_path);

    if (!project.has_value())
    {
      LogError << "loadProject() failed, Project is null" << std::endl;
      return {};
    }
    else
    {
      Log << "loadProject(): Loading Project Data" << std::endl;
    }


    project_reader.readPalettes(&project.value());
    project_reader.readObjectSelectionGroups(&project.value());

    std::string dbd_file_directory = _configuration->ApplicationDatabaseDefinitionsPath;
    if (project->projectVersion == ProjectVersion::FOREVER)
    {
      // Forever (1.60.x) layouts are only described by the extended native
      // definitions that ship with Noggit; a user-provided "definitions-forever"
      // drop-in wins when present. Never point the database library at a
      // directory that does not exist: its .dbd parser dereferences the first
      // line of a failed file open (lines[0] of an empty vector) and crashes
      // the process instead of reporting the missing table.
      if (std::filesystem::exists("definitions-forever"))
      {
        dbd_file_directory = "definitions-forever";
        Log << "[Definitions] Forever project: using 'definitions-forever'." << std::endl;
      }
      else if (std::filesystem::exists("native-db2-definitions"))
      {
        dbd_file_directory = "native-db2-definitions";
        Log << "[Definitions] Forever project: using bundled 'native-db2-definitions'"
               " (carries 1.60.x builds and modern layout hashes)." << std::endl;
      }
      else
      {
        LogError << "[Definitions] Forever project: neither 'definitions-forever' nor"
                    " 'native-db2-definitions' exists; falling back to '"
                 << dbd_file_directory << "', which does not describe 1.60.x layouts."
                 << std::endl;
      }
    }

    BlizzardDatabaseLib::Structures::Build client_build("3.3.5.12340");
    std::string client_build_string("3.3.5.12340");
    auto client_archive_version = BlizzardArchive::ClientVersion::WOTLK;
    auto client_archive_locale = BlizzardArchive::Locale::AUTO;
    if (project->projectVersion == ProjectVersion::SL || project->projectVersion == ProjectVersion::RETAIL
        || project->projectVersion == ProjectVersion::FOREVER)
    {
      client_archive_version = project->projectVersion == ProjectVersion::FOREVER
        ? BlizzardArchive::ClientVersion::FOREVER
        : (project->projectVersion == ProjectVersion::RETAIL
            ? BlizzardArchive::ClientVersion::RETAIL
            : BlizzardArchive::ClientVersion::SL);
      // Detect the build from the product line the project actually needs.
      // Install-root .build.info files list every flavor and mark retail as
      // active; without the product filter every client selected from the
      // same install detected as the retail build.
      std::string const preferred_product =
        project->projectVersion == ProjectVersion::FOREVER ? "wow_classic_beta" : "wow";
      auto detected_build = detectClientBuildVersion(project->ClientPath, preferred_product);
      if (detected_build.empty())
      {
        detected_build = "9.2.7.45745";
        LogError << "Could not detect client build from .build.info or Data/config, falling back to "
                 << detected_build << std::endl;
      }
      else
      {
        Log << "Detected modern client build: " << detected_build << std::endl;
      }

      client_build = BlizzardDatabaseLib::Structures::Build(detected_build);
      client_build_string = detected_build;
      try
      {
        std::size_t parsed_length = 0;
        auto const major_version = std::stoi(detected_build, &parsed_length);
        if (parsed_length != 0)
        {
          // The project's selected version is authoritative for the CASC
          // profile; the detected build only cross-checks it. Deriving the
          // profile from the build silently turned Shadowlands projects into
          // Retail ones whenever detection landed on a retail build number.
          bool const looks_retail = major_version >= 10;
          bool const looks_forever = major_version == 1;
          bool mismatch = false;
          switch (project->projectVersion)
          {
            case ProjectVersion::SL: mismatch = looks_retail || looks_forever; break;
            case ProjectVersion::RETAIL: mismatch = !looks_retail; break;
            case ProjectVersion::FOREVER: mismatch = !looks_forever; break;
            default: break;
          }
          if (mismatch)
            LogError << "[Client] Project version '"
                     << ClientVersionFactory::MapToStringVersion(project->projectVersion)
                     << "' does not match the detected client build " << detected_build
                     << " (client directory: " << project->ClientPath
                     << "). Verify the selected client directory; continuing with the"
                        " project-selected profile." << std::endl;
        }
      }
      catch (std::exception const& e)
      {
        LogError << "Could not cross-check the CASC profile against build " << detected_build
                 << ": " << e.what() << std::endl;
      }
      Log << "Selected modern CASC profile from project version: "
          << ClientVersionFactory::MapToStringVersion(project->projectVersion) << std::endl;
      client_archive_locale = BlizzardArchive::Locale::enUS;
    }

    else if (project->projectVersion == ProjectVersion::WOTLK)
    {
      client_archive_version = BlizzardArchive::ClientVersion::WOTLK;
      client_build = BlizzardDatabaseLib::Structures::Build("3.3.5.12340");
      client_archive_locale = BlizzardArchive::Locale::AUTO;
    }

    else
    {
      LogError << "Unsupported project version" << std::endl;
      return {};
    }

    project->DatabaseDefinitionsPath = dbd_file_directory;
    project->ClientBuildString = client_build_string;
    project->ClientDatabase = std::make_shared<BlizzardDatabaseLib::BlizzardDatabase>(
      dbd_file_directory, client_build, project->projectVersion == ProjectVersion::FOREVER);

    Log << "Loading Client Path : " << project->ClientPath << std::endl;

    std::string listfile_error;
    if (!refreshWowdevListfile(project_path, listfile_error))
    {
      LogError << listfile_error << std::endl;
      QMessageBox::critical(nullptr, "Error", QString::fromStdString(listfile_error));
      return {};
    }

    try
    {
      project->ClientData = std::make_shared<BlizzardArchive::ClientData>(
        project->ClientPath, client_archive_version, client_archive_locale, project_path.generic_string());
    }
    catch (BlizzardArchive::Exceptions::Locale::LocaleNotFoundError& e)
    {
      LogError << e.what() << std::endl;
      QMessageBox::critical(nullptr, "Error", e.what());
      return {};
    }
    catch (BlizzardArchive::Exceptions::Locale::IncorrectLocaleModeError& e)
    {
      LogError << e.what() << std::endl;
      QMessageBox::critical(nullptr, "Error", e.what());
      return {};
    }
    catch (BlizzardArchive::Exceptions::Archive::ArchiveOpenError& e)
    {
      LogError << e.what() << std::endl;
      QMessageBox::critical(nullptr, "Error", e.what());
      return {};
    }
    catch (std::exception const& e)
    {
      LogError << "Failed loading Client data: " << e.what() << std::endl;
      QMessageBox::critical(nullptr, "Error", QString("Failed loading Client data:\n%1").arg(e.what()));
      return {};
    }
    catch (...)
    {
      LogError << "Failed loading Client data. Unhandled exception." << std::endl;
      QMessageBox::critical(nullptr, "Error", "Failed loading Client data. Unhandled exception.");
      return {};
    }

    if (!project->ClientData)
    {
      LogError << "Failed loading Client data." << std::endl;
      return {};
    }

    if ((project->projectVersion == ProjectVersion::SL || project->projectVersion == ProjectVersion::RETAIL
         || project->projectVersion == ProjectVersion::FOREVER)
        && !project->ClientData->exists(BlizzardArchive::Listfile::FileKey("dbfilesclient/map.db2")))
    {
      auto const message = QString(
        "The CASC archive opened, but DBFilesClient/Map.db2 is not available locally.\n\n"
        "This usually means the selected client is only partially downloaded. "
        "Finish downloading the client data, or place the required DB2 in the Noggit project override directory.");
      LogError << message.toStdString() << std::endl;
      QMessageBox::critical(nullptr, "Incomplete CASC client", message);
      return {};
    }

    // Log << "Client Version: " << static_cast<int>(project->ClientData->version()) << std::endl;

    Log << "Client Locale: " << project->ClientData->locale_name() << std::endl;

    for (auto const loaded_achive : *project->ClientData->loadedArchives())
    {
      Log << "Loaded client Archive: " << loaded_achive->path() << std::endl;
    }

    // QSettings settings;
    // bool modern_features = settings.value("modern_features", false).toBool();
    bool modern_features = _configuration->modern_features;
    if (modern_features)
    {
      Log << "Modern Features Enabled" << std::endl;
      loadExtraData(project.value());
    }
    else
    {
      Log << "Modern Features Disabled" << std::endl;
    }
    return std::make_shared<NoggitProject>(project.value());
  }

  void ApplicationProject::loadExtraData(NoggitProject& project)
    {
        std::filesystem::path extraDataFolder = (project.ProjectPath);
        extraDataFolder /= "extraData";

        Log << "Loading extra data from " << extraDataFolder << std::endl;

        if (std::filesystem::exists(extraDataFolder) && std::filesystem::is_directory(extraDataFolder))
        {
            for (const auto& entry : std::filesystem::directory_iterator(extraDataFolder))
            {
                if (entry.path().extension() == ".cfg")
                {
                    QFile input_file(QString::fromStdString(entry.path().generic_string()));
                    input_file.open(QIODevice::ReadOnly);
                    QJsonParseError err;
                    auto document = QJsonDocument().fromJson(input_file.readAll(), &err);
                    auto root = document.object();
                    auto keys = root.keys();
                    if (entry.path().stem() == "global")
                    {
                        for (auto const& entry : keys)
                        {
                            texture_heightmapping_data newData;
                            newData.uvScale = root[entry].toObject()["Scale"].toInt();
                            newData.heightOffset = root[entry].toObject()["HeightOffset"].toDouble();
                            newData.heightScale = root[entry].toObject()["HeightScale"].toDouble();
                            project.ExtraMapData.SetTextureHeightData_Global(entry.toStdString(), newData);
                        }
                    }
                }
            }
        }
    }
    void NoggitExtraMapData::SetTextureHeightData_Global(const std::string& texture, texture_heightmapping_data data, World* worldToUpdate)
    {
        TextureHeightData_Global[texture] = data;
        if (worldToUpdate)
        {
            for (MapTile* tile : worldToUpdate->mapIndex.loaded_tiles())
            {
                tile->registerChunkUpdate(ChunkUpdateFlags::ALPHAMAP);
                tile->forceAlphaUpdate();
                tile->forceRecalcExtents();
            }
        }
    }
    void NoggitExtraMapData::SetTextureHeightDataForADT(int mapID, const TileIndex& ti, const std::string& texture, texture_heightmapping_data data, World* worldToUpdate)
    {
        TextureHeightData_ADT[mapID][ti.x][ti.z][texture] = data;
        if (worldToUpdate)
        {
            MapTile* tile = worldToUpdate->mapIndex.getTile(ti);
            tile->registerChunkUpdate(ChunkUpdateFlags::ALPHAMAP);
            tile->forceAlphaUpdate();
            tile->forceRecalcExtents();
        }
    }
    const texture_heightmapping_data NoggitExtraMapData::GetTextureHeightDataForADT(int mapID, const TileIndex& tileIndex, const std::string& texture) const
    {
        static texture_heightmapping_data defaultValue;
        auto foundMapIter = TextureHeightData_ADT.find(mapID);
        if (foundMapIter != TextureHeightData_ADT.end())
        {
            auto foundXIter = foundMapIter->second.find(tileIndex.x);
            if (foundXIter != foundMapIter->second.end())
            {
                auto foundYIter = foundXIter->second.find(tileIndex.z);
                if (foundYIter != foundXIter->second.end())
                {
                    auto foundTexData = foundYIter->second.find(texture);
                    if (foundTexData != foundYIter->second.end())
                    {
                        return foundTexData->second;
                    }
                }
            }
        }
        auto foundGenericSettings = TextureHeightData_Global.find(texture);
        if (foundGenericSettings != TextureHeightData_Global.end())
        {
            return foundGenericSettings->second;
        }
        return defaultValue;
    }

    ProjectVersion ClientVersionFactory::mapToEnumVersion(std::string const& projectVersion)
    {
      if (projectVersion == "Wrath Of The Lich King")
        return ProjectVersion::WOTLK;
      if (projectVersion == "Shadowlands" || projectVersion == "Modern")
        return ProjectVersion::SL;
      if (projectVersion == "Retail")
        return ProjectVersion::RETAIL;
      if (projectVersion == "Forever")
        return ProjectVersion::FOREVER;

      LogError << "Unknown project version '" << projectVersion << "', falling back to Shadowlands compatibility." << std::endl;
      return ProjectVersion::SL;
    }

    std::string ClientVersionFactory::MapToStringVersion(ProjectVersion const& projectVersion)
    {
      if (projectVersion == ProjectVersion::WOTLK)
        return std::string("Wrath Of The Lich King");
      if (projectVersion == ProjectVersion::SL)
        return std::string("Shadowlands");
      if (projectVersion == ProjectVersion::RETAIL)
        return std::string("Retail");
      if (projectVersion == ProjectVersion::FOREVER)
        return std::string("Forever");

      LogError << "Unknown project version enum, falling back to Shadowlands." << std::endl;
      return std::string("Shadowlands");
    }

    NoggitProject::NoggitProject()
    {
      _projectWriter = std::make_shared<ApplicationProjectWriter>();
    }

    unsigned int NoggitProject::buildId()
    {
      return ClientDatabase->getBuild();
    }

    void NoggitProject::createBookmark(const NoggitProjectBookmarkMap& bookmark)
    {
      Bookmarks.push_back(bookmark);

      _projectWriter->saveProject(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::deleteBookmark()
    {
    }

    void NoggitProject::pinMap(int map_id, const std::string& map_name)
    {
      auto pinnedMap = NoggitProjectPinnedMap();
      pinnedMap.MapName = map_name;
      pinnedMap.MapId = map_id;

      auto pinnedMapFound = std::find_if(std::begin(PinnedMaps), std::end(PinnedMaps),
        [&](Project::NoggitProjectPinnedMap pinnedMap)
        {
          return pinnedMap.MapId == map_id;
        });

      if (pinnedMapFound != std::end(PinnedMaps))
        return;

      PinnedMaps.push_back(pinnedMap);

      _projectWriter->saveProject(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::unpinMap(int mapId)
    {
      PinnedMaps.erase(std::remove_if(PinnedMaps.begin(), PinnedMaps.end(),
        [=](NoggitProjectPinnedMap pinnedMap)
        {
          return pinnedMap.MapId == mapId;
        }),
        PinnedMaps.end());

      _projectWriter->saveProject(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::saveTexturePalette(const NoggitProjectTexturePalette& new_texture_palette)
    {
      TexturePalettes.erase(std::remove_if(TexturePalettes.begin(), TexturePalettes.end(),
        [=](NoggitProjectTexturePalette texture_palette)
        {
          return texture_palette.MapId == new_texture_palette.MapId;
        }),
        TexturePalettes.end());

      TexturePalettes.push_back(new_texture_palette);

      _projectWriter->savePalettes(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::saveObjectPalette(const NoggitProjectObjectPalette& new_object_palette)
    {
      ObjectPalettes.erase(std::remove_if(ObjectPalettes.begin(), ObjectPalettes.end(),
        [=](NoggitProjectObjectPalette obj_palette)
        {
          return obj_palette.MapId == new_object_palette.MapId;
        }),
        ObjectPalettes.end());

      ObjectPalettes.push_back(new_object_palette);

      _projectWriter->savePalettes(this, std::filesystem::path(ProjectPath));
    }

    void NoggitProject::saveObjectSelectionGroups(const NoggitProjectSelectionGroups& new_selection_groups)
    {
      ObjectSelectionGroups.erase(std::remove_if(ObjectSelectionGroups.begin(), ObjectSelectionGroups.end(),
        [=](NoggitProjectSelectionGroups proj_selection_group)
        {
          return proj_selection_group.MapId == new_selection_groups.MapId;
        }),
        ObjectSelectionGroups.end());

      ObjectSelectionGroups.push_back(new_selection_groups);

      _projectWriter->saveObjectSelectionGroups(this, std::filesystem::path(ProjectPath));
    }
};
