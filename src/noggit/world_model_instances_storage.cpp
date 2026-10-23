// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/world_model_instances_storage.hpp>
#include <noggit/World.h>
#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>
#include <noggit/TileIndex.hpp>

namespace Noggit
{
  world_model_instances_storage::world_model_instances_storage(World* world)
    : _world(world)
  {

  }

  std::uint32_t world_model_instances_storage::add_model_instance(ModelInstance instance, bool from_reloading, bool action)
  {
    std::uint32_t uid = instance.uid;
    std::uint32_t uid_after;
    ModelInstance* added = nullptr;

    {
      std::lock_guard<std::mutex> const lock (_mutex);
      uid_after = unsafe_add_model_instance_no_world_upd(std::move(instance), action);

      // The lookup has to happen under the same lock: tiles are loaded on the
      // async loader thread while the main thread inserts, moves and erases
      // instances. Reading _m2s outside the lock raced with a rehash and either
      // threw std::out_of_range ("invalid map<K, T> key") or walked a bucket
      // chain that was being rewritten - an access violation in the middle of
      // placing an object. The duplicate paths also return a uid that was never
      // inserted here, so .at() was not guaranteed to find it either.
      if (auto it = _m2s.find(uid_after); it != _m2s.end())
        added = &it->second;
    }

    if (added && (from_reloading || uid_after != uid))
    {
      _world->updateTilesModel(added, model_update::add);
    }

    return uid_after;
  }
  std::uint32_t world_model_instances_storage::unsafe_add_model_instance_no_world_upd(ModelInstance instance, bool action)
  {
    std::uint32_t uid = instance.uid;
    auto existing_instance = unsafe_get_model_instance(uid);

    if (existing_instance)
    {
      // instance already loaded
      if (existing_instance.value()->isDuplicateOf(instance))
      {
        _instance_count_per_uid[uid]++;
        return uid;
      }
      // ADTs list an object in the MDDF of every tile its bounds intersect,
      // always with the same uid. If the object was moved/rotated/scaled this
      // session, rows loaded afterwards from another tile still hold the old
      // transform: same uid + same model is the same object, not a uid
      // collision. Spawning a new uid here would duplicate the object at its
      // old transform (and bake the copy into the map on save). Only applies
      // to objects edited this session - a transform mismatch on disk is a
      // genuine clash and must keep both, or saving would silently delete one.
      if (existing_instance.value()->_transformed_this_session
          && existing_instance.value()->model->file_key() == instance.model->file_key())
      {
        LogDebug << "UID " << uid << " already loaded with a different transform, ignoring stale tile entry ("
                 << instance.model->file_key().stringRepr() << ")" << std::endl;
        _instance_count_per_uid[uid]++;
        return uid;
      }
    }
    else if(!unsafe_uid_is_used(uid))
    {
      // This causes a crash when undoing while loading a tile, those objects get registered to the action stack
      if (action && NOGGIT_CUR_ACTION)
        NOGGIT_CUR_ACTION->registerObjectAdded(&instance);
      _m2s.emplace(uid, instance);
      _instance_count_per_uid[uid] = 1;
      return uid;
    }

    // the uid is already used for another model/wmo, use a new one
    LogDebug << "UID " << uid << " is already in use (" << instance.model->file_key().stringRepr() << ")" << std::endl;
    _uid_duplicates_found = true;
    instance.uid = _world->mapIndex.newGUID();

    return unsafe_add_model_instance_no_world_upd(std::move(instance), action);
  }

  
  std::uint32_t world_model_instances_storage::add_wmo_instance(WMOInstance instance, bool from_reloading, bool action)
  {
    std::uint32_t uid = instance.uid;
    std::uint32_t uid_after;
    WMOInstance* wmo_instance = nullptr;

    {
      std::lock_guard<std::mutex> const lock(_mutex);
      uid_after = unsafe_add_wmo_instance_no_world_upd(std::move(instance), action);

      // Same reasoning as add_model_instance(): resolve the instance under the
      // lock instead of reading _wmos from whichever thread happens to get here
      // while the loader thread is rehashing the map.
      if (auto it = _wmos.find(uid_after); it != _wmos.end())
        wmo_instance = &it->second;
    }

    if (wmo_instance && (from_reloading || uid_after != uid))
    {
      _world->updateTilesWMO(wmo_instance, model_update::add);

      // update WDL uid if it changed
      [[unlikely]]
      if (wmo_instance->lowResWmo.has_value())
      {
        wmo_instance->lowResInstance->uniqueID = uid_after;
      }
    }

    return uid_after;
  }
  std::uint32_t world_model_instances_storage::unsafe_add_wmo_instance_no_world_upd(WMOInstance instance, bool action)
  {
    std::uint32_t uid = instance.uid;
    auto existing_instance = unsafe_get_wmo_instance(uid);

    if (existing_instance)
    {
      // instance already loaded
      if (existing_instance.value()->isDuplicateOf(instance))
      {
        _instance_count_per_uid[uid]++;

        return uid;
      }
      // Same as the m2 case above: a same-uid MODF row pointing to the same
      // WMO is a stale copy of an object edited this session, not a collision.
      if (existing_instance.value()->_transformed_this_session
          && existing_instance.value()->wmo->file_key() == instance.wmo->file_key())
      {
        LogDebug << "UID " << uid << " already loaded with a different transform, ignoring stale tile entry ("
                 << instance.wmo->file_key().stringRepr() << ")" << std::endl;
        _instance_count_per_uid[uid]++;
        return uid;
      }
    }
    else if (!unsafe_uid_is_used(uid))
    {
      if (action && NOGGIT_CUR_ACTION)
        NOGGIT_CUR_ACTION->registerObjectAdded(&instance);
      _wmos.emplace(uid, instance);
      _instance_count_per_uid[uid] = 1;
      return uid;
    }

    // the uid is already used for another model/wmo, use a new one
    LogDebug << "UID " << uid << " is already in use (" << instance.wmo->file_key().stringRepr() << ")" << std::endl;
    _uid_duplicates_found = true;
    instance.uid = _world->mapIndex.newGUID();

    return unsafe_add_wmo_instance_no_world_upd(std::move(instance), action);
  }

  void world_model_instances_storage::delete_instances_from_tile(TileIndex const& tile, bool action)
  {
    // The scan has to hold the lock: the async loader thread inserts into both
    // maps while a tile is streaming in, and iterating an unordered_map that is
    // rehashing on another thread is an access violation. delete_instance()
    // takes the same (non recursive) mutex, so the uids are collected first and
    // the removal happens after the scan is done.
    std::vector<std::uint32_t> uids_to_remove;

    {
      std::lock_guard<std::mutex> const lock(_mutex);

      uids_to_remove.reserve(_m2s.size() + _wmos.size());

      for (auto const& entry : _m2s)
      {
        if (TileIndex(entry.second.pos) == tile)
          uids_to_remove.push_back(entry.first);
      }
      for (auto const& entry : _wmos)
      {
        if (TileIndex(entry.second.pos) == tile)
          uids_to_remove.push_back(entry.first);
      }
    }

    for (auto uid : uids_to_remove)
      delete_instance(uid, action);
  }

  void world_model_instances_storage::delete_instances(std::vector<selected_object_type> const& instances, bool action)
  {
    for (auto& obj : instances)
    {
      // done in delete_instance
      /*
      if (action && NOGGIT_CUR_ACTION)
        NOGGIT_CUR_ACTION->registerObjectRemoved(obj);*/

      if (obj->which() == eMODEL)
      {
        auto instance = static_cast<ModelInstance*>(obj);
        
        delete_instance(instance->uid, action);
      }
      else if (obj->which() == eWMO)
      {
        auto instance = static_cast<WMOInstance*>(obj);

        delete_instance(instance->uid, action);
      }
    }
  }

  void world_model_instances_storage::delete_instance(std::uint32_t uid, bool action)
  {
    std::unique_lock<std::mutex> const lock (_mutex);

    if (auto instance = get_instance(uid, false))
    {
      _world->updateTilesEntry(instance.value(), model_update::remove);
      auto obj = std::get<selected_object_type>(instance.value());

      for (auto& selection_group : _world->_selection_groups)
      {
          if (selection_group.contains_object(obj))
          {
              selection_group.remove_member(obj->uid);
          }
      }
      if (action && NOGGIT_CUR_ACTION)
      {
        NOGGIT_CUR_ACTION->registerObjectRemoved(obj);
      }
    }

    // The instance is about to be freed. Drop it from the world selection first:
    // _current_selection stores raw pointers into these maps, and the object
    // tool dereferences them on the next tick (rotation editor, gizmo, box
    // drawing). Leaving the entry behind turned every later frame into an
    // access violation. Groups were already detached above, so skip them here.
    _world->remove_from_selection(uid, true, true);

    _instance_count_per_uid.erase(uid);
    _m2s.erase(uid);
    _wmos.erase(uid);
  }

  void world_model_instances_storage::unload_instance_and_remove_from_selection_if_necessary(std::uint32_t uid)
  {
    std::unique_lock<std::mutex> const lock (_mutex);

    if (!unsafe_uid_is_used(uid))
    {
      // Expected churn, not a fault: a model instance spanning several ADTs
      // is enumerated by every tile referencing it, and tile unload order is
      // arbitrary (worst at teardown), so an already-removed uid arriving
      // here is normal. The guard below keeps it harmless.
      LogDebug << "Skipping unload of model instance uid " << uid
               << ": already unloaded (or never stored)." << std::endl;
      return;
    }

    if (--_instance_count_per_uid.at(uid) == 0)
    {
      _world->remove_from_selection(uid, false, false);

      _instance_count_per_uid.erase(uid);
      _m2s.erase(uid);
      _wmos.erase(uid);
    }
  }

  void world_model_instances_storage::clear()
  {
    std::unique_lock<std::mutex> const lock (_mutex);

    _instance_count_per_uid.clear();
    _m2s.clear();
    _wmos.clear();
  }

  std::optional<ModelInstance*> world_model_instances_storage::get_model_instance(std::uint32_t uid)
  {
    std::unique_lock<std::mutex> const lock (_mutex);
    return unsafe_get_model_instance(uid);
  }
  std::optional<ModelInstance*> world_model_instances_storage::unsafe_get_model_instance(std::uint32_t uid)
  {
    auto it = _m2s.find(uid);

    if (it != _m2s.end())
    {
      return &it->second;
    }
    else
    {
      return std::nullopt;
    }
  }

  std::optional<WMOInstance*> world_model_instances_storage::get_wmo_instance(std::uint32_t uid)
  {
    std::unique_lock<std::mutex> const lock (_mutex);
    return unsafe_get_wmo_instance(uid);
  }
  std::optional<WMOInstance*> world_model_instances_storage::unsafe_get_wmo_instance(std::uint32_t uid)
  {
    auto it = _wmos.find(uid);

    if (it != _wmos.end())
    {
      return &it->second;
    }
    else
    {
      return std::nullopt;
    }
  }

  std::optional<selection_type> world_model_instances_storage::get_instance(std::uint32_t uid, bool lock)
  {
    if (lock)
    {
      std::unique_lock<std::mutex> const lock(_mutex);

      auto wmo_it = _wmos.find(uid);

      if (wmo_it != _wmos.end())
      {
        return selection_type{ &wmo_it->second };
      }
      else
      {
        auto m2_it = _m2s.find(uid);

        if (m2_it != _m2s.end())
        {
          return selection_type{ &m2_it->second };
        }
        else
        {
          return std::nullopt;
        }
      }
    }
    else
    {
      auto wmo_it = _wmos.find(uid);

      if (wmo_it != _wmos.end())
      {
        return selection_type{ &wmo_it->second };
      }
      else
      {
        auto m2_it = _m2s.find(uid);

        if (m2_it != _m2s.end())
        {
          return selection_type{ &m2_it->second };
        }
        else
        {
            return std::nullopt;
        }
      }
    }
  }

  bool world_model_instances_storage::unsafe_uid_is_used(std::uint32_t uid) const
  {
    return _instance_count_per_uid.find(uid) != _instance_count_per_uid.end();
  }

  void world_model_instances_storage::clear_duplicates(bool action)
  {
    std::unique_lock<std::mutex> const lock (_mutex);

    int deleted_uids = 0;

    for (auto lhs(_wmos.begin()); lhs != _wmos.end(); ++lhs)
    {
      for (auto rhs(std::next(lhs)); rhs != _wmos.end();)
      {
        assert(lhs->first != rhs->first);

        if (lhs->second.isDuplicateOf(rhs->second))
        {
          _world->updateTilesWMO(&rhs->second, model_update::remove);

          _instance_count_per_uid.erase(rhs->second.uid);
          if (action && NOGGIT_CUR_ACTION)
            NOGGIT_CUR_ACTION->registerObjectRemoved(&rhs->second);
          rhs = _wmos.erase(rhs);
          deleted_uids++;
        }
        else
        {
          rhs++;
        }
      }
    }

    for (auto lhs(_m2s.begin()); lhs != _m2s.end(); ++lhs)
    {
      for (auto rhs(std::next(lhs)); rhs != _m2s.end();)
      {
        assert(lhs->first != rhs->first);

        if (lhs->second.isDuplicateOf(rhs->second))
        {
          _world->updateTilesModel(&rhs->second, model_update::remove);

          _instance_count_per_uid.erase(rhs->second.uid);

          if (action && NOGGIT_CUR_ACTION)
            NOGGIT_CUR_ACTION->registerObjectRemoved(&rhs->second);
          rhs = _m2s.erase(rhs);
          deleted_uids++;
        }
        else
        {
          rhs++;
        }
      }
    }

    Log << "Deleted " << deleted_uids << " duplicate Model/WMO" << std::endl;
  }

  bool world_model_instances_storage::uid_duplicates_found() const
  {
    return _uid_duplicates_found.load();
  }

  void world_model_instances_storage::upload()
  {
    if (_transform_storage_uploaded)
      return;

    _buffers.upload();

    gl.genTextures(1, &_m2_instances_transform_buf_tex);
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_BUFFER, _m2_instances_transform_buf_tex);
    gl.bindBuffer(GL_TEXTURE_BUFFER, _m2_instances_transform_buf);
    gl.bufferData(GL_TEXTURE_BUFFER, _n_allocated_m2_transforms * sizeof(glm::mat4x4), nullptr, GL_DYNAMIC_DRAW);
    gl.texBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, _m2_instances_transform_buf);

    _transform_storage_uploaded = true;
  }

  void world_model_instances_storage::unload()
  {
    if (!_transform_storage_uploaded)
      return;

    gl.deleteTextures(1, &_m2_instances_transform_buf_tex);
    _buffers.unload();

    _transform_storage_uploaded = false;
  }

  unsigned int world_model_instances_storage::getTotalModelsCount() const
  {
    // size() is not atomic with respect to a rehash happening on the loader
    // thread; the count is only informational but reading it unsynchronised is
    // still undefined behaviour.
    std::lock_guard<std::mutex> const lock(_mutex);
    return static_cast<unsigned int>(_m2s.size() + _wmos.size());
  }
}
