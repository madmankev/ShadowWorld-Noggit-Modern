// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Log.h> // LogDebug
#include <noggit/Model.h> // Model
#include <noggit/ModelManager.h> // ModelManager

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>


namespace
{
  std::string normalized_filename (std::string filename)
  {
    filename = BlizzardArchive::ClientData::normalizeFilenameInternal(filename);

    std::size_t found;
    if ((found = filename.rfind (".mdx")) != std::string::npos)
    {
      filename.replace(found, 4, ".m2");
    }
    else if ((found = filename.rfind (".mdl")) != std::string::npos)
    {
      filename.replace(found, 4, ".m2");
    }

    return filename;
  }
}

// ModelManager::_ is defined in AsyncObjectManagers.cpp to fix static destruction order.

void ModelManager::report()
{
  // Grouped by render context with the live reference count. The managers are
  // global and shared by the map view, the asset browser and every preview
  // renderer, so a flat list of file keys could not say whether an entry was
  // leaked or simply still cached somewhere - the reports grew by a handful of
  // entries on every map load with no way to tell who was holding them.
  std::map<Noggit::NoggitRenderContext, std::vector<std::pair<std::string, std::size_t>>> by_context;

  _.apply_detailed ( [&] ( BlizzardArchive::Listfile::FileKey const& key
                         , Noggit::NoggitRenderContext context
                         , std::size_t references
                         )
                     {
                       by_context[context].emplace_back (key.stringRepr(), references);
                     }
                   );

  std::string output = "Still in the Model manager:\n";

  for (auto const& entry : by_context)
  {
    output += std::string (" ") + Noggit::render_context_name (entry.first) + ": "
            + std::to_string (entry.second.size ()) + " entr"
            + (entry.second.size () == 1 ? "y" : "ies") + "\n";

    for (auto const& element : entry.second)
      output += "  - " + element.first + " (refs " + std::to_string (element.second) + ")\n";
  }

  LogDebug << output;
}

void ModelManager::resetAnim()
{
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const&, Model& model)
            {
              model.anim_calculated = false;
            }
          );
}

void ModelManager::clear_hidden_models()
{
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const&, Model& model)
            {
              model.show();
            }
          );
}

void ModelManager::unload_all(Noggit::NoggitRenderContext context)
{
  _.context_aware_apply(
      [&] (BlizzardArchive::Listfile::FileKey const&, Model& model)
      {
          model.renderer()->unload();
      }
      , context
  );
}

scoped_model_reference::scoped_model_reference(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)

    : _valid(true)
    , _file_key(file_key)
    , _model(ModelManager::_.emplace(_file_key, context))
    , _context(context)

{
}

scoped_model_reference::scoped_model_reference(scoped_model_reference const& other)
    : _valid(other._valid)
    , _file_key(other._file_key)
    , _model(ModelManager::_.emplace(_file_key, other._context))
    , _context(other._context)
{
}

scoped_model_reference& scoped_model_reference::operator=(scoped_model_reference const& other)
{
    _valid = other._valid;
    _file_key = other._file_key;
    _model = ModelManager::_.emplace(_file_key, other._context);
    _context = other._context;
    return *this;
}

scoped_model_reference::scoped_model_reference(scoped_model_reference&& other)
    : _valid(other._valid)
    , _file_key(other._file_key)
    , _model(other._model)
    , _context(other._context)
{
    other._valid = false;
}

scoped_model_reference& scoped_model_reference::operator=(scoped_model_reference&& other)
{
  std::swap(_valid, other._valid);
  std::swap(_file_key, other._file_key);
  std::swap(_model, other._model);
  std::swap(_context, other._context);
  other._valid = false;
  return *this;
}

scoped_model_reference::~scoped_model_reference()
{
  if (_valid)
  {
    ModelManager::_.erase(_file_key, _context);
  }
}

Model* scoped_model_reference::operator->() const
{
  return _model;
}

[[nodiscard]]
Model* scoped_model_reference::get() const
{
  return _model;
}
