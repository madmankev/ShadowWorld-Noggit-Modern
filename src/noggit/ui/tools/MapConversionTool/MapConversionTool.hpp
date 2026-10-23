#pragma once
// Qt integration for the two-way WotLK/modern map conversion
// (docs/MAP_CONVERSION.md).
//
// The conversion core (Noggit::Formats::Conversion) is Qt-free; this module
// supplies the application side:
//
//  * makeClientDataSource(): adapts the project's BlizzardArchive client
//    data (CASC or MPQ) to the ConversionFileSource callbacks - files are
//    read through ClientFile, FileDataIDs resolve through the listfile.
//  * writeConversionOutput(): writes a ConversionReport's files under an
//    output directory (creating subdirectories), plus
//    conversion_manifest.json.
//  * MapConversionDialog: the "Convert map..." tool window - map name,
//    target format, options, output directory; runs the conversion on a
//    worker thread and shows the report (files, FileDataID assignments,
//    warnings) with an option to merge the suggested Map.db2 registration
//    into the project's extraData/modernMapRegistrations.json.

#include <noggit/formats/conversion/MapConversion.hpp>

#include <QDialog>

#include <filesystem>
#include <memory>
#include <string>

class QLabel;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace BlizzardArchive
{
  class ClientData;
}

namespace Noggit::Project
{
  class NoggitProject;
}

namespace Noggit::Ui::Tools::MapConversionTool
{
  //! Adapt client data (CASC/MPQ + listfile) to the converter's file
  //! source. Unknown FileDataIDs return 0 from getFileDataID; the adapter
  //! maps that to "unknown" so the converter allocates custom ids.
  Noggit::Formats::Conversion::ConversionFileSource makeClientDataSource(
    BlizzardArchive::ClientData* client_data);

  //! Loose-folder source: reads files relative to \p root, no listfile.
  //! FileDataID lookups always report "unknown" (everything gets custom
  //! ids recorded in the manifest).
  Noggit::Formats::Conversion::ConversionFileSource makeFolderSource(
    std::filesystem::path root);

  struct WriteResult
  {
    unsigned files_written = 0;
    std::vector<std::string> errors;
  };

  //! Write every converted file under \p output_directory (relative paths
  //! are recreated) plus conversion_manifest.json next to them.
  WriteResult writeConversionOutput(
    std::filesystem::path const& output_directory,
    Noggit::Formats::Conversion::ConversionReport const& report);

  class MapConversionDialog : public QDialog
  {
  public:
    MapConversionDialog(std::shared_ptr<Noggit::Project::NoggitProject> project,
                        QWidget* parent = nullptr);

  private:
    void startConversion();
    void onConversionFinished(Noggit::Formats::Conversion::ConversionReport report);
    void appendReportText(Noggit::Formats::Conversion::ConversionReport const& report);

    std::shared_ptr<Noggit::Project::NoggitProject> _project;

    QLineEdit* _map_name_edit = nullptr;
    QComboBox* _target_combo = nullptr;
    QComboBox* _generation_combo = nullptr;
    QCheckBox* _include_wdl_check = nullptr;
    QCheckBox* _convert_liquids_check = nullptr;
    QCheckBox* _big_alpha_check = nullptr;
    QCheckBox* _green_lava_check = nullptr;
    QSpinBox* _map_id_spin = nullptr;
    QLineEdit* _fdid_base_edit = nullptr;
    QLineEdit* _output_dir_edit = nullptr;
    QPushButton* _browse_button = nullptr;
    QPushButton* _convert_button = nullptr;
    QPlainTextEdit* _report_view = nullptr;
    QLabel* _status_label = nullptr;

    std::filesystem::path _project_path;
    //! Set while the worker thread runs.
    bool _converting = false;
    Noggit::Formats::Conversion::ConversionReport _last_report;
  };
}
