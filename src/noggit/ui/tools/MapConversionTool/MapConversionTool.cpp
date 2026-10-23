#include <noggit/ui/tools/MapConversionTool/MapConversionTool.hpp>

#include <noggit/Log.h>
#include <noggit/project/ApplicationProject.h>
#include <noggit/project/ModernMapRegistrations.h>

#include <ClientData.hpp>
#include <ClientFile.hpp>
#include <Listfile.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QMetaObject>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <QApplication>

#include <exception>
#include <fstream>
#include <thread>
#include <utility>

namespace Noggit::Ui::Tools::MapConversionTool
{
  using Noggit::Formats::Conversion::ConversionFileSource;
  using Noggit::Formats::Conversion::ConversionOptions;
  using Noggit::Formats::Conversion::MapGeneration;
  using Noggit::Formats::Conversion::ConversionReport;
  using Noggit::Formats::Conversion::MapFormat;

  namespace
  {
    std::string backslashPath(std::string path)
    {
      for (char& c : path)
        if (c == '/')
          c = '\\';
      return path;
    }

    //! Read one client file, trying every key that can resolve it.
    //!
    //! A path-only FileKey carries no FileDataID unless the listfile resolves
    //! one, and the CASC read then fails without a word - with a partial
    //! custom listfile every client-source conversion died on "cannot read
    //! <map>.wdt" before a single tile was attempted. Mirror the editor's
    //! read_casc_original chain: deduced component key, listfile path keys in
    //! both slash styles, then the raw FileDataID.
    std::vector<std::uint8_t> readClientFile(BlizzardArchive::ClientData* client_data,
                                             std::string const& normalized_path)
    {
      auto attempt = [client_data](BlizzardArchive::Listfile::FileKey key)
        -> std::vector<std::uint8_t>
      {
        try
        {
          BlizzardArchive::ClientFile file(key, client_data);
          auto const size = file.getSize();
          if (!size)
            return {};
          auto const* buffer = reinterpret_cast<std::uint8_t const*>(file.getBuffer());
          return std::vector<std::uint8_t>(buffer, buffer + size);
        }
        catch (...)
        {
          return {};
        }
      };

      BlizzardArchive::Listfile::FileKey deduced(normalized_path);
      if (client_data->listfile())
        deduced.deduceOtherComponent(client_data->listfile());
      if (auto bytes = attempt(deduced); !bytes.empty())
        return bytes;

      BlizzardArchive::Listfile::FileKey listed(backslashPath(normalized_path),
                                                client_data->listfile());
      if (auto bytes = attempt(listed); !bytes.empty())
        return bytes;

      if (client_data->listfile())
      {
        std::uint32_t const fdid =
          client_data->listfile()->getFileDataID(backslashPath(normalized_path));
        if (fdid)
        {
          if (auto bytes = attempt(BlizzardArchive::Listfile::FileKey(fdid)); !bytes.empty())
            return bytes;
        }
      }
      return {};
    }
  }

  ConversionFileSource makeClientDataSource(BlizzardArchive::ClientData* client_data)
  {
    ConversionFileSource source;
    source.read_file = [client_data](std::string const& path)
      -> std::optional<std::vector<std::uint8_t>>
    {
      // ClientData resolves both slash styles; try the normalized form and
      // the backslash spelling the listfile prefers.
      auto bytes = readClientFile(client_data, path);
      if (bytes.empty())
        bytes = readClientFile(client_data, backslashPath(path));
      if (bytes.empty())
        return std::nullopt;
      return bytes;
    };
    source.path_to_fdid = [client_data](std::string const& path)
      -> std::optional<std::uint32_t>
    {
      if (!client_data->listfile())
        return std::nullopt;
      std::uint32_t fdid = client_data->listfile()->getFileDataID(backslashPath(path));
      if (!fdid)
        fdid = client_data->listfile()->getFileDataID(path);
      if (!fdid)
        return std::nullopt;
      return fdid;
    };
    //! Direct FileDataID read: with a partial custom listfile many genuine
    //! client files resolve by no path at all, yet the WDT's MAID entries
    //! carry their true ids and CASC can still serve them.
    source.read_fdid = [client_data](std::uint32_t fdid)
      -> std::optional<std::vector<std::uint8_t>>
    {
      if (!fdid)
        return std::nullopt;
      try
      {
        BlizzardArchive::ClientFile file(BlizzardArchive::Listfile::FileKey(fdid),
                                         client_data);
        auto const size = file.getSize();
        if (!size)
          return std::nullopt;
        auto const* buffer = reinterpret_cast<std::uint8_t const*>(file.getBuffer());
        return std::vector<std::uint8_t>(buffer, buffer + size);
      }
      catch (...)
      {
        return std::nullopt;
      }
    };
    source.fdid_to_path = [client_data](std::uint32_t fdid) -> std::string
    {
      if (!fdid || !client_data->listfile())
        return {};
      return client_data->listfile()->getPath(fdid);
    };
    return source;
  }

  ConversionFileSource makeFolderSource(std::filesystem::path root)
  {
    ConversionFileSource source;
    source.read_file = [root](std::string const& path)
      -> std::optional<std::vector<std::uint8_t>>
    {
      std::error_code ec;
      auto const full = root / path;
      std::ifstream file(full, std::ios::binary);
      if (!file)
        return std::nullopt;
      std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
      (void)ec;
      if (bytes.empty())
        return std::nullopt;
      return bytes;
    };
    source.path_to_fdid = [](std::string const&) -> std::optional<std::uint32_t>
    {
      return std::nullopt;
    };
    source.fdid_to_path = [](std::uint32_t) -> std::string
    {
      return {};
    };
    return source;
  }

  WriteResult writeConversionOutput(std::filesystem::path const& output_directory,
                                    ConversionReport const& report)
  {
    WriteResult result;
    std::error_code ec;
    std::filesystem::create_directories(output_directory, ec);
    if (ec)
    {
      result.errors.push_back("cannot create output directory: " + ec.message());
      return result;
    }

    for (auto const& file : report.files)
    {
      std::filesystem::path const full = output_directory / file.relative_path;
      std::filesystem::create_directories(full.parent_path(), ec);
      if (ec)
      {
        result.errors.push_back(file.relative_path + ": " + ec.message());
        continue;
      }
      std::ofstream out(full, std::ios::binary | std::ios::trunc);
      if (!out)
      {
        result.errors.push_back(file.relative_path + ": cannot open for writing");
        continue;
      }
      out.write(reinterpret_cast<char const*>(file.bytes.data()),
                static_cast<std::streamsize>(file.bytes.size()));
      if (!out)
      {
        result.errors.push_back(file.relative_path + ": write failed");
        continue;
      }
      ++result.files_written;
    }

    std::ofstream manifest(output_directory / "conversion_manifest.json",
                           std::ios::binary | std::ios::trunc);
    if (manifest)
    {
      std::string const json = report.manifest_json();
      manifest.write(json.data(), static_cast<std::streamsize>(json.size()));
    }
    else
    {
      result.errors.push_back("conversion_manifest.json: cannot open for writing");
    }

    return result;
  }

  // --------------------------------------------------------------------
  // Dialog
  // --------------------------------------------------------------------

  MapConversionDialog::MapConversionDialog(
    std::shared_ptr<Noggit::Project::NoggitProject> project, QWidget* parent)
    : QDialog(parent)
    , _project(std::move(project))
  {
    setWindowTitle("Convert map (WotLK <-> modern)");
    setMinimumWidth(720);

    if (_project)
      _project_path = std::filesystem::path(_project->ProjectPath);

    auto* root_layout = new QVBoxLayout(this);

    auto* form_group = new QGroupBox("Conversion", this);
    auto* form = new QFormLayout(form_group);

    _map_name_edit = new QLineEdit(form_group);
    _map_name_edit->setPlaceholderText("Directory name as used under World\\Maps, e.g. Azeroth");
    form->addRow("Map name", _map_name_edit);

    _target_combo = new QComboBox(form_group);
    _target_combo->addItem("WotLK (3.3.5 monolithic ADT)", static_cast<int>(MapFormat::wotlk));
    _target_combo->addItem("Modern (Shadowlands 9.2.7 split ADT)", static_cast<int>(MapFormat::modern));
    // Default to the direction that leaves the project's own format:
    bool const project_is_wotlk =
      !_project || _project->projectVersion == Noggit::Project::ProjectVersion::WOTLK;
    _target_combo->setCurrentIndex(project_is_wotlk ? 1 : 0);
    form->addRow("Convert to", _target_combo);

    // Which modern family "modern" means for this run. The post-WotLK
    // clients are distinct formats (WDT MAI2, WDL MSSF, different
    // listfiles); the converter steps the map-level files to the family
    // picked here and reports what it strips and what it cannot author.
    _generation_combo = new QComboBox(form_group);
    _generation_combo->addItem("Shadowlands 9.2.7",
                               static_cast<int>(MapGeneration::shadowlands_9_2_7));
    _generation_combo->addItem("Retail 12.x",
                               static_cast<int>(MapGeneration::retail_12_x));
    _generation_combo->addItem("Forever 1.60.x (beta; Shadowlands-family files)",
                               static_cast<int>(MapGeneration::forever_1_60_x));
    {
      MapGeneration const client_generation =
        _project
          ? Noggit::Formats::Conversion::generationFromBuildString(_project->ClientBuild)
          : MapGeneration::shadowlands_9_2_7;
      int const generation_index =
        _generation_combo->findData(static_cast<int>(client_generation));
      _generation_combo->setCurrentIndex(generation_index >= 0 ? generation_index : 0);
    }
    form->addRow("Modern family", _generation_combo);

    _map_id_spin = new QSpinBox(form_group);
    _map_id_spin->setRange(-1, 1000000);
    _map_id_spin->setValue(-1);
    _map_id_spin->setSpecialValueText("unknown");
    form->addRow("Map.db2 id", _map_id_spin);

    _fdid_base_edit = new QLineEdit("900000000", form_group);
    form->addRow("Custom FileDataID base", _fdid_base_edit);

    _include_wdl_check = new QCheckBox("Convert <map>.wdl (horizon)", form_group);
    _include_wdl_check->setChecked(true);
    _convert_liquids_check = new QCheckBox("Convert MCLQ liquids to MH2O", form_group);
    _convert_liquids_check->setChecked(true);
    _big_alpha_check = new QCheckBox("Big alpha maps (WotLK side)", form_group);
    _big_alpha_check->setChecked(true);
    _green_lava_check = new QCheckBox("MCLQ magma as green lava (LiquidType 15)", form_group);
    auto* options_layout = new QVBoxLayout();
    options_layout->addWidget(_include_wdl_check);
    options_layout->addWidget(_convert_liquids_check);
    options_layout->addWidget(_big_alpha_check);
    options_layout->addWidget(_green_lava_check);
    form->addRow("Options", options_layout);

    auto* output_row = new QHBoxLayout();
    _output_dir_edit = new QLineEdit(form_group);
    if (!_project_path.empty())
      _output_dir_edit->setText(QString::fromStdString((_project_path / "converted").string()));
    _browse_button = new QPushButton("Browse...", form_group);
    output_row->addWidget(_output_dir_edit);
    output_row->addWidget(_browse_button);
    form->addRow("Output directory", output_row);

    root_layout->addWidget(form_group);

    _report_view = new QPlainTextEdit(this);
    _report_view->setReadOnly(true);
    _report_view->setMinimumHeight(240);
    root_layout->addWidget(_report_view, 1);

    auto* button_row = new QHBoxLayout();
    _status_label = new QLabel("Ready.", this);
    _convert_button = new QPushButton("Convert", this);
    button_row->addWidget(_status_label, 1);
    button_row->addWidget(_convert_button);
    root_layout->addLayout(button_row);

    connect(_browse_button, &QPushButton::clicked, this, [this]
    {
      QString const dir = QFileDialog::getExistingDirectory(
        this, "Choose the conversion output directory", _output_dir_edit->text());
      if (!dir.isEmpty())
        _output_dir_edit->setText(dir);
    });

    connect(_convert_button, &QPushButton::clicked, this, &MapConversionDialog::startConversion);
  }

  void MapConversionDialog::startConversion()
  {
    if (_converting)
      return;

    std::string const map_name = _map_name_edit->text().trimmed().toStdString();
    if (map_name.empty())
    {
      QMessageBox::warning(this, "Convert map", "Enter the map's directory name first.");
      return;
    }
    std::string const output_dir = _output_dir_edit->text().trimmed().toStdString();
    if (output_dir.empty())
    {
      QMessageBox::warning(this, "Convert map", "Choose an output directory first.");
      return;
    }

    ConversionOptions options;
    options.target = static_cast<MapFormat>(_target_combo->currentData().toInt());
    options.target_generation =
      static_cast<MapGeneration>(_generation_combo->currentData().toInt());
    options.map_name = map_name;
    options.map_id = _map_id_spin->value();
    options.include_wdl = _include_wdl_check->isChecked();
    options.convert_liquids = _convert_liquids_check->isChecked();
    options.big_alpha = _big_alpha_check->isChecked();
    options.green_lava = _green_lava_check->isChecked();

    bool fdid_base_ok = false;
    unsigned long long const fdid_base = _fdid_base_edit->text().toULongLong(&fdid_base_ok);
    if (!fdid_base_ok || fdid_base == 0 || fdid_base > 0xFFFFFFFEull)
    {
      QMessageBox::warning(this, "Convert map",
                           "The custom FileDataID base must be a number between 1 and 4294967294.");
      return;
    }
    options.custom_fdid_base = static_cast<std::uint32_t>(fdid_base);
    if (_project)
      options.target_build = _project->ClientBuild;

    // The source is always the project's client data: a WotLK project
    // converts its MPQ maps to modern, a modern project converts its CASC
    // maps to WotLK. Cross-installing the other family's files into the
    // project client is how reconversions are fed back in.
    ConversionFileSource source = _project && _project->ClientData
      ? makeClientDataSource(_project->ClientData.get())
      : makeFolderSource(_project_path);

    _converting = true;
    _convert_button->setEnabled(false);
    _status_label->setText(QString("Converting %1...").arg(QString::fromStdString(map_name)));
    _report_view->clear();

    // Context object so the queued result is dropped safely when the
    // dialog is closed before the worker finishes.
    QPointer<MapConversionDialog> self(this);
    std::thread worker([self, options, source = std::move(source)]() mutable
    {
      ConversionReport report;
      try
      {
        report = Noggit::Formats::Conversion::convertMap(options, source);
      }
      catch (std::exception const& e)
      {
        report.ok = false;
        report.errors.emplace_back(std::string("conversion threw: ") + e.what());
      }
      catch (...)
      {
        report.ok = false;
        report.errors.emplace_back("conversion threw an unknown exception");
      }
      QMetaObject::invokeMethod(qApp, [self, report = std::move(report)]() mutable
      {
        if (self)
          self->onConversionFinished(std::move(report));
      }, Qt::QueuedConnection);
    });
    worker.detach();
  }

  void MapConversionDialog::onConversionFinished(ConversionReport report)
  {
    _converting = false;
    _convert_button->setEnabled(true);
    _last_report = report;

    appendReportText(report);

    if (!report.ok)
    {
      _status_label->setText("Conversion failed - see the report.");
      return;
    }

    std::string const output_dir = _output_dir_edit->text().trimmed().toStdString();
    WriteResult const written = writeConversionOutput(output_dir, report);
    for (std::string const& error : written.errors)
      _report_view->appendPlainText(QString::fromStdString("write error: " + error));

    _status_label->setText(QString("Wrote %1 file(s) to %2")
                             .arg(written.files_written)
                             .arg(QString::fromStdString(output_dir)));

    // Registration merge for the modern direction.
    if (report.target == MapFormat::modern
        && report.map_id > 0
        && !report.suggested_registration_fields.empty()
        && !_project_path.empty())
    {
      auto const answer = QMessageBox::question(
        this, "Register converted map",
        QString("Merge the Map.db2 registration for map id %1 into this project's "
                "extraData/modernMapRegistrations.json?\n\n"
                "The suggested fields are conservative defaults - review them in the "
                "manifest before relying on them in game.")
          .arg(report.map_id));
      if (answer == QMessageBox::Yes)
      {
        Noggit::Project::ModernMapRegistration registration;
        registration.id = report.map_id;
        registration.target_build = report.target_build;
        registration.fields = report.suggested_registration_fields;
        registration.array_fields = report.suggested_registration_array_fields;

        std::string const problem =
          Noggit::Project::ModernMapRegistrations::validate(registration);
        if (!problem.empty())
        {
          _report_view->appendPlainText(
            QString::fromStdString("registration NOT merged: " + problem));
        }
        else
        {
          auto registrations = Noggit::Project::ModernMapRegistrations::load(_project_path);
          Noggit::Project::ModernMapRegistrations::upsert(registrations, registration);
          bool const saved =
            Noggit::Project::ModernMapRegistrations::save(_project_path, registrations);
          _report_view->appendPlainText(
            saved ? "Registration merged into extraData/modernMapRegistrations.json."
                  : "Registration merge failed (file not writable).");
        }
      }
    }
  }

  void MapConversionDialog::appendReportText(ConversionReport const& report)
  {
    _report_view->appendPlainText(
      QString("Conversion %1 -> %2: %3")
        .arg(report.source == MapFormat::modern ? "modern" : "WotLK")
        .arg(report.target == MapFormat::modern ? "modern" : "WotLK")
        .arg(report.ok ? "succeeded" : "FAILED"));
    _report_view->appendPlainText(
      QString("Tiles: %1 total, %2 converted, %3 skipped. Files: %4. FileDataIDs: %5.")
        .arg(report.tiles_total)
        .arg(report.tiles_converted)
        .arg(report.tiles_skipped)
        .arg(report.files.size())
        .arg(report.fdid_assignments.size()));

    for (std::string const& error : report.errors)
      _report_view->appendPlainText(QString::fromStdString("ERROR: " + error));

    // Cap the displayed warnings; the manifest keeps them all.
    constexpr std::size_t max_shown_warnings = 200;
    for (std::size_t i = 0; i < report.warnings.size() && i < max_shown_warnings; ++i)
      _report_view->appendPlainText(QString::fromStdString("warning: " + report.warnings[i]));
    if (report.warnings.size() > max_shown_warnings)
      _report_view->appendPlainText(
        QString("... %1 more warnings in conversion_manifest.json")
          .arg(report.warnings.size() - max_shown_warnings));

    std::size_t allocated = 0;
    for (auto const& assignment : report.fdid_assignments)
      if (!assignment.existed_in_client)
        ++allocated;
    if (allocated)
      _report_view->appendPlainText(
        QString("%1 FileDataID(s) were newly allocated and must be registered "
                "(listfile/server) - see conversion_manifest.json.").arg(allocated));

    // The report view is transient and a failed conversion was undiagnosable
    // from the application log ("... 1 errors" with no content at all).
    // Mirror the first errors and warnings into the log as well.
    constexpr std::size_t max_logged_issues = 20;
    for (std::size_t i = 0; i < report.errors.size() && i < max_logged_issues; ++i)
      LogError << "[MapConversion] error: " << report.errors[i] << std::endl;
    if (report.errors.size() > max_logged_issues)
      LogError << "[MapConversion] ... " << (report.errors.size() - max_logged_issues)
               << " more error(s) in the report view / conversion_manifest.json"
               << std::endl;
    for (std::size_t i = 0; i < report.warnings.size() && i < max_logged_issues; ++i)
      Log << "[MapConversion] warning: " << report.warnings[i] << std::endl;

    Log << "[MapConversion] " << (report.ok ? "converted" : "failed to convert")
        << " map \"" << report.map_name << "\": " << report.tiles_converted << "/"
        << report.tiles_total << " tiles, " << report.files.size() << " files, "
        << report.warnings.size() << " warnings, " << report.errors.size()
        << " errors" << std::endl;
  }
}
