////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "vocbase.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Parser.h>
#include <velocypack/velocypack-aliases.h>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/PlanCache.h"
#include "Aql/QueryCache.h"
#include "Aql/QueryList.h"
#include "Basics/ConditionLocker.h"
#include "Basics/Exceptions.h"
#include "Basics/FileUtils.h"
#include "Basics/HybridLogicalClock.h"
#include "Basics/NumberUtils.h"
#include "Basics/ReadLocker.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/conversions.h"
#include "Basics/files.h"
#include "Basics/locks.h"
#include "Basics/memory-map.h"
#include "Basics/threads.h"
#include "Basics/tri-strings.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Logger/Logger.h"
#include "Replication/DatabaseReplicationApplier.h"
#include "Replication/InitialSyncer.h"
#include "RestServer/DatabaseFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Utils/CollectionKeysRepository.h"
#include "Utils/CursorRepository.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utils/VersionTracker.h"
#include "V8Server/v8-user-structures.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ticks.h"

#include <thread>

namespace {

  template<typename T>
  class RecursiveReadLocker {
   public:
    RecursiveReadLocker(
        T& mutex,
        std::atomic<std::thread::id>& owner,
        char const* file,
        int line
    ): _locker(&mutex, arangodb::basics::LockerType::TRY, true, file, line) {
      if (!_locker.isLocked() && owner.load() != std::this_thread::get_id()) {
        _locker.lock();
      }
    }

   private:
    arangodb::basics::ReadLocker<T> _locker;
  };

  template<typename T>
  class RecursiveWriteLocker {
   public:
    RecursiveWriteLocker(
        T& mutex,
        std::atomic<std::thread::id>& owner,
        arangodb::basics::LockerType type,
        bool aquire,
        char const* file,
        int line
    ): _locker(&mutex, type, false, file, line), _owner(owner), _update(noop) {
      if (aquire) {
        lock();
      }
    }

    ~RecursiveWriteLocker() {
      unlock();
    }

    bool isLocked() {
      return _locker.isLocked();
    }

    void lock() {
      // recursive locking of the same instance is not yet supported (create a new instance instead)
      TRI_ASSERT(_update != owned);

      if (_locker.tryLock()) {
        _owner.store(std::this_thread::get_id());
        _update = owned;
      } else if (std::this_thread::get_id() != _owner.load()) { // not recursive
        _locker.lock();
        _owner.store(std::this_thread::get_id());
        _update = owned;
      }
    }

    void unlock() {
      _locker.unlock();
      _update(*this);
    }

   private:
    arangodb::basics::WriteLocker<T> _locker;
    std::atomic<std::thread::id>& _owner;
    void (*_update)(RecursiveWriteLocker& locker);

    static void noop(RecursiveWriteLocker&) {}
    static void owned(RecursiveWriteLocker& locker) {
      static std::thread::id unowned;
      locker._owner.store(unowned);
      locker._update = noop;
    }
  };

  #define NAME__(name, line) name ## line
  #define NAME_EXPANDER__(name, line) NAME__(name, line)
  #define NAME(name) NAME_EXPANDER__(name, __LINE__)
  #define RECURSIVE_READ_LOCKER(lock, owner) RecursiveReadLocker<typename std::decay<decltype (lock)>::type> NAME(RecursiveLocker)(lock, owner, __FILE__, __LINE__)
  #define RECURSIVE_WRITE_LOCKER_NAMED(name, lock, owner, aquire) RecursiveWriteLocker<typename std::decay<decltype (lock)>::type> name(lock, owner, arangodb::basics::LockerType::BLOCKING, aquire, __FILE__, __LINE__)
  #define RECURSIVE_WRITE_LOCKER(lock, owner) RECURSIVE_WRITE_LOCKER_NAMED(NAME(RecursiveLocker), lock, owner, true)

}

using namespace arangodb;
using namespace arangodb::basics;

/// @brief increase the reference counter for a database
bool TRI_vocbase_t::use() {
  auto expected = _refCount.load(std::memory_order_relaxed);
  while (true) {
    if ((expected & 1) != 0) {
      // deleted bit is set
      return false;
    }
    // increase the reference counter by 2.
    // this is because we use odd values to indicate that the database has been
    // marked as deleted
    auto updated = expected + 2;
    TRI_ASSERT((updated & 1) == 0);
    if (_refCount.compare_exchange_weak(expected, updated,
                                        std::memory_order_release,
                                        std::memory_order_relaxed)) {
      // compare-exchange worked. we're done
      return true;
    }
    // compare-exchange failed. try again!
    expected = _refCount.load(std::memory_order_relaxed);
  }
}

void TRI_vocbase_t::forceUse() { _refCount += 2; }

/// @brief decrease the reference counter for a database
void TRI_vocbase_t::release() {
  // decrease the reference counter by 2.
  // this is because we use odd values to indicate that the database has been
  // marked as deleted
  auto oldValue = _refCount.fetch_sub(2);
  TRI_ASSERT(oldValue >= 2);
}

/// @brief returns whether the database can be dropped
bool TRI_vocbase_t::isDangling() const {
  if (isSystem()) {
    return false;
  }
  auto refCount = _refCount.load();
  // we are intentionally comparing with exactly 1 here, because a 1 means
  // that noone else references the database but it has been marked as deleted
  return (refCount == 1);
}

/// @brief whether or not the vocbase has been marked as deleted
bool TRI_vocbase_t::isDropped() const {
  auto refCount = _refCount.load();
  // if the stored value is odd, it means the database has been marked as
  // deleted
  return (refCount % 2 == 1);
}

/// @brief marks a database as deleted
bool TRI_vocbase_t::markAsDropped() {
  TRI_ASSERT(!isSystem());

  auto oldValue = _refCount.fetch_or(1);
  // if the previously stored value is odd, it means the database has already
  // been marked as deleted
  return (oldValue % 2 == 0);
}

/// @brief signal the cleanup thread to wake up
void TRI_vocbase_t::signalCleanup() {
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  engine->signalCleanup(this);
}

void TRI_vocbase_t::checkCollectionInvariants() const {
  TRI_ASSERT(_dataSourceByName.size() == _dataSourceById.size());
  TRI_ASSERT(_dataSourceByUuid.size() <= _dataSourceById.size()); // does not contain views
}

/// @brief adds a new collection
/// caller must hold _dataSourceLock in write mode or set doLock
void TRI_vocbase_t::registerCollection(
    bool doLock,
    std::shared_ptr<arangodb::LogicalCollection> const& collection
) {
  std::string name = collection->name();
  TRI_voc_cid_t cid = collection->id();
  {
    RECURSIVE_WRITE_LOCKER_NAMED(writeLocker, _dataSourceLock, _dataSourceLockWriteOwner, doLock);

    checkCollectionInvariants();
    TRI_DEFER(checkCollectionInvariants());

    // check name
    auto it = _dataSourceByName.emplace(name, collection);

    if (!it.second) {
      std::string msg;
      msg.append(std::string("duplicate entry for collection name '") + name + "'. collection id " + std::to_string(cid) + " has same name as already added collection " + std::to_string(_dataSourceByName[name]->id()));
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << msg;

      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DUPLICATE_NAME, msg);
    }

    // check collection identifier
    try {
      auto it2 = _dataSourceById.emplace(cid, collection);

      if (!it2.second) {
        std::string msg;
        msg.append(std::string("duplicate collection identifier ") + std::to_string(collection->id()) + " for name '" + name + "'");
        LOG_TOPIC(ERR, arangodb::Logger::FIXME) << msg;

        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER, msg);
      }
    } catch (...) {
      _dataSourceByName.erase(name);
      throw;
    }

    try {
      auto it2 = _dataSourceByUuid.emplace(collection->globallyUniqueId(), collection);

      if (!it2.second) {
        std::string msg;
        msg.append(std::string("duplicate entry for collection uuid '") + collection->globallyUniqueId() + "'");
        LOG_TOPIC(ERR, arangodb::Logger::FIXME) << msg;

        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER, msg);
      }
    } catch (...) {
      _dataSourceByName.erase(name);
      _dataSourceById.erase(cid);
      throw;
    }

    try {
      _collections.emplace_back(collection);
    } catch (...) {
      _dataSourceByName.erase(name);
      _dataSourceById.erase(cid);
      _dataSourceByUuid.erase(collection->globallyUniqueId());
      throw;
    }

    collection->setStatus(TRI_VOC_COL_STATUS_UNLOADED);
  }
}

/// @brief removes a collection name from the global list of collections
/// This function is called when a collection is dropped.
/// NOTE: You need a writelock on _dataSourceLock
bool TRI_vocbase_t::unregisterCollection(
    arangodb::LogicalCollection* collection) {
  TRI_ASSERT(collection != nullptr);

  // pre-condition
  checkCollectionInvariants();

  auto itr = _dataSourceById.find(collection->id());

  if (itr == _dataSourceById.end()
      || itr->second->category() != LogicalCollection::category()) {
    return true; // no such collection
  }

  TRI_ASSERT(std::dynamic_pointer_cast<arangodb::LogicalCollection>(itr->second));

  // only if we find the collection by its id, we can delete it by name
  _dataSourceById.erase(itr);

    // this is because someone else might have created a new collection with the
    // same name, but with a different id
  _dataSourceByName.erase(collection->name());
  _dataSourceByUuid.erase(collection->globallyUniqueId());

  // post-condition
  checkCollectionInvariants();

  return true;
}

/// @brief adds a new view
/// caller must hold _viewLock in write mode or set doLock
void TRI_vocbase_t::registerView(
    bool doLock,
    std::shared_ptr<arangodb::LogicalView> const& view
) {
  std::string const name = view->name();
  TRI_voc_cid_t const id = view->id();
  {
    RECURSIVE_WRITE_LOCKER_NAMED(writeLocker, _dataSourceLock, _dataSourceLockWriteOwner, doLock);

    // check name
    auto it = _dataSourceByName.emplace(name, view);

    if (!it.second) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME)
          << "duplicate entry for view name '" << name << "'";
      LOG_TOPIC(ERR, arangodb::Logger::FIXME)
          << "view id " << id << " has same name as already added view "
          << _dataSourceByName[name]->id();

      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DUPLICATE_NAME);
    }

    // check id
    try {
      auto it2 = _dataSourceById.emplace(id, view);

      if (!it2.second) {
        _dataSourceByName.erase(name);

        LOG_TOPIC(ERR, arangodb::Logger::FIXME)
            << "duplicate view identifier " << view->id() << " for name '"
            << name << "'";

        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER);
      }
    } catch (...) {
      _dataSourceByName.erase(name);
      throw;
    }

    checkCollectionInvariants();
  }
}

/// @brief removes a views name from the global list of views
/// This function is called when a view is dropped.
/// NOTE: You need a writelock on _dataSourceLock
bool TRI_vocbase_t::unregisterView(
    std::shared_ptr<arangodb::LogicalView> view) {
  TRI_ASSERT(view != nullptr);

  // pre-condition
  checkCollectionInvariants();

  auto itr = _dataSourceById.find(view->id());

  if (itr == _dataSourceById.end()
      || itr->second->category() != arangodb::LogicalView::category()) {
    return true; // no such view
  }

  TRI_ASSERT(std::dynamic_pointer_cast<arangodb::LogicalView>(itr->second));

  // only if we find the collection by its id, we can delete it by name
  _dataSourceById.erase(itr);

    // this is because someone else might have created a new view with the
    // same name, but with a different id
  _dataSourceByName.erase(view->name());

  // post-condition
  checkCollectionInvariants();

  return true;
}

/// @brief drops a collection
bool TRI_vocbase_t::DropCollectionCallback(
    arangodb::LogicalCollection* collection) {
  std::string const name(collection->name());

  {
    WRITE_LOCKER_EVENTUAL(statusLock, collection->_lock);

    if (collection->status() != TRI_VOC_COL_STATUS_DELETED) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME)
          << "someone resurrected the collection '" << name << "'";
      return false;
    }
  }  // release status lock

  // remove from list of collections
  TRI_vocbase_t* vocbase = collection->vocbase();

  {
    RECURSIVE_WRITE_LOCKER(vocbase->_dataSourceLock, vocbase->_dataSourceLockWriteOwner);
    auto it = vocbase->_collections.begin();

    for (auto end = vocbase->_collections.end(); it != end; ++it) {
      if (it->get() == collection) {
        break;
      }
    }

    if (it != vocbase->_collections.end()) {
      auto col = *it;

      vocbase->_collections.erase(it);

      // we need to clean up the pointers later so we insert it into this vector
      try {
        vocbase->_deadCollections.emplace_back(col);
      } catch (...) {
      }
    }
  }

  collection->drop();

  return true;
}

/// @brief creates a new collection, worker function
std::shared_ptr<arangodb::LogicalCollection> TRI_vocbase_t::createCollectionWorker(
    VPackSlice parameters) {
  std::string name = arangodb::basics::VelocyPackHelper::getStringValue(
      parameters, "name", "");
  TRI_ASSERT(!name.empty());

  // Try to create a new collection. This is not registered yet

  std::shared_ptr<arangodb::LogicalCollection> collection =
      std::make_unique<arangodb::LogicalCollection>(this, parameters, false);
  TRI_ASSERT(collection != nullptr);

  RECURSIVE_WRITE_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);

  // reserve room for the new collection
  _collections.reserve(_collections.size() + 1);
  _deadCollections.reserve(_deadCollections.size() + 1);

  auto it = _dataSourceByName.find(name);

  if (it != _dataSourceByName.end()) {
    events::CreateCollection(name, TRI_ERROR_ARANGO_DUPLICATE_NAME);
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DUPLICATE_NAME);
  }

  registerCollection(basics::ConditionalLocking::DoNotLock, collection);

  try {
    collection->setStatus(TRI_VOC_COL_STATUS_LOADED);
    // set collection version to 3.1, as the collection is just created
    collection->setVersion(LogicalCollection::VERSION_31);

    // Let's try to persist it.
    collection->persistPhysicalCollection();

    events::CreateCollection(name, TRI_ERROR_NO_ERROR);

    return collection;
  } catch (...) {
    unregisterCollection(collection.get());
    throw;
  }
}

/// @brief loads an existing collection
/// Note that this will READ lock the collection. You have to release the
/// collection lock by yourself.
int TRI_vocbase_t::loadCollection(arangodb::LogicalCollection* collection,
                                  TRI_vocbase_col_status_e& status,
                                  bool setStatus) {
  TRI_ASSERT(collection->id() != 0);

  // read lock
  // check if the collection is already loaded
  {
    ExecContext const* exec = ExecContext::CURRENT;
    if (exec != nullptr &&
        !exec->canUseCollection(_name, collection->name(), auth::Level::RO)) {
      return TRI_set_errno(TRI_ERROR_FORBIDDEN);
    }


    READ_LOCKER_EVENTUAL(locker, collection->_lock);

    // return original status to the caller
    if (setStatus) {
      status = collection->status();
    }

    if (collection->status() == TRI_VOC_COL_STATUS_LOADED) {
      // DO NOT release the lock
      locker.steal();
      return TRI_ERROR_NO_ERROR;
    }

    if (collection->status() == TRI_VOC_COL_STATUS_DELETED) {
      return TRI_set_errno(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    }

    if (collection->status() == TRI_VOC_COL_STATUS_CORRUPTED) {
      return TRI_set_errno(TRI_ERROR_ARANGO_CORRUPTED_COLLECTION);
    }
  }
  // release the read lock and acquire a write lock, we have to do some work

  // .............................................................................
  // write lock
  // .............................................................................

  WRITE_LOCKER_EVENTUAL(locker, collection->_lock);

  // someone else loaded the collection, release the WRITE lock and try again
  if (collection->status() == TRI_VOC_COL_STATUS_LOADED) {
    locker.unlock();
    return loadCollection(collection, status, false);
  }

  // someone is trying to unload the collection, cancel this,
  // release the WRITE lock and try again
  if (collection->status() == TRI_VOC_COL_STATUS_UNLOADING) {
    // check if the collection is dropped
    if (collection->deleted()) {
      // drop call going on, we must abort
      locker.unlock();

      // someone requested the collection to be dropped, so it's not there
      // anymore
      return TRI_set_errno(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    }

    // no drop action found, go on
    collection->setStatus(TRI_VOC_COL_STATUS_LOADED);
    locker.unlock();

    return loadCollection(collection, status, false);
  }

  // deleted, give up
  if (collection->status() == TRI_VOC_COL_STATUS_DELETED) {
    return TRI_set_errno(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  // corrupted, give up
  if (collection->status() == TRI_VOC_COL_STATUS_CORRUPTED) {
    return TRI_set_errno(TRI_ERROR_ARANGO_CORRUPTED_COLLECTION);
  }

  // currently loading
  if (collection->status() == TRI_VOC_COL_STATUS_LOADING) {
    locker.unlock();

    // loop until the status changes
    while (true) {
      TRI_vocbase_col_status_e status;
      {
        READ_LOCKER_EVENTUAL(readLocker, collection->_lock);
        status = collection->status();
      }

      if (status != TRI_VOC_COL_STATUS_LOADING) {
        break;
      }

      // only throw this particular error if the server is configured to do so
      auto databaseFeature =
          application_features::ApplicationServer::getFeature<DatabaseFeature>(
              "Database");
      if (databaseFeature->throwCollectionNotLoadedError()) {
        return TRI_ERROR_ARANGO_COLLECTION_NOT_LOADED;
      }

      std::this_thread::sleep_for(std::chrono::microseconds(collectionStatusPollInterval()));
    }

    return loadCollection(collection, status, false);
  }

  // unloaded, load collection
  if (collection->status() == TRI_VOC_COL_STATUS_UNLOADED) {
    // set the status to loading
    collection->setStatus(TRI_VOC_COL_STATUS_LOADING);

    // release the lock on the collection temporarily
    // this will allow other threads to check the collection's
    // status while it is loading (loading may take a long time because of
    // disk activity, index creation etc.)
    locker.unlock();

    bool ignoreDatafileErrors = false;
    if (DatabaseFeature::DATABASE != nullptr) {
      ignoreDatafileErrors = DatabaseFeature::DATABASE->ignoreDatafileErrors();
    }

    try {
      collection->open(ignoreDatafileErrors);
    } catch (arangodb::basics::Exception const& ex) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME)
          << "caught exception while opening collection '" << collection->name()
          << "': " << ex.what();
      collection->setStatus(TRI_VOC_COL_STATUS_CORRUPTED);
      return TRI_ERROR_ARANGO_CORRUPTED_COLLECTION;
    } catch (std::exception const& ex) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME)
          << "caught exception while opening collection '" << collection->name()
          << "': " << ex.what();
      collection->setStatus(TRI_VOC_COL_STATUS_CORRUPTED);
      return TRI_ERROR_ARANGO_CORRUPTED_COLLECTION;
    } catch (...) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME)
          << "caught unknown exception while opening collection '"
          << collection->name() << "'";
      collection->setStatus(TRI_VOC_COL_STATUS_CORRUPTED);
      return TRI_ERROR_ARANGO_CORRUPTED_COLLECTION;
    }

    // lock again to adjust the status
    locker.lockEventual();

    // no one else must have changed the status
    TRI_ASSERT(collection->status() == TRI_VOC_COL_STATUS_LOADING);

    collection->setStatus(TRI_VOC_COL_STATUS_LOADED);
    collection->load();

    // release the WRITE lock and try again
    locker.unlock();

    return loadCollection(collection, status, false);
  }

  std::string const colName(collection->name());
  LOG_TOPIC(ERR, arangodb::Logger::FIXME)
      << "unknown collection status " << collection->status() << " for '"
      << colName << "'";

  return TRI_set_errno(TRI_ERROR_INTERNAL);
}

/// @brief drops a collection, worker function
int TRI_vocbase_t::dropCollectionWorker(arangodb::LogicalCollection* collection,
                                        DropState& state, double timeout) {
  state = DROP_EXIT;
  std::string const colName(collection->name());

  double startTime = TRI_microtime();

  // do not acquire these locks instantly
  RECURSIVE_WRITE_LOCKER_NAMED(
    writeLocker,
    _dataSourceLock,
    _dataSourceLockWriteOwner,
    basics::ConditionalLocking::DoNotLock
  );
  CONDITIONAL_WRITE_LOCKER(locker, collection->_lock,
                           basics::ConditionalLocking::DoNotLock);

  while (true) {
    TRI_ASSERT(!writeLocker.isLocked());
    TRI_ASSERT(!locker.isLocked());

    // block until we have acquired this lock
    writeLocker.lock();
    // we now have the one lock

    TRI_ASSERT(writeLocker.isLocked());

    if (locker.tryLock()) {
      // we now have both locks and can continue outside of this loop
      break;
    }

    // unlock the write locker so we don't block other operations
    writeLocker.unlock();

    TRI_ASSERT(!writeLocker.isLocked());
    TRI_ASSERT(!locker.isLocked());

    if (timeout >= 0.0 && TRI_microtime() > startTime + timeout) {
      events::DropCollection(colName, TRI_ERROR_LOCK_TIMEOUT);
      return TRI_ERROR_LOCK_TIMEOUT;
    }

    // sleep for a while
    std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::microseconds(10000));
  }

  TRI_ASSERT(writeLocker.isLocked());
  TRI_ASSERT(locker.isLocked());

#if USE_PLAN_CACHE
  arangodb::aql::PlanCache::instance()->invalidate(this);
#endif
  arangodb::aql::QueryCache::instance()->invalidate(this);

  switch (collection->status()) {
    case TRI_VOC_COL_STATUS_DELETED: {
      // collection already deleted
      // mark collection as deleted
      unregisterCollection(collection);
      break;
    }
    case TRI_VOC_COL_STATUS_LOADING: {
      // collection is loading
      // loop until status changes
      // try again later
      state = DROP_AGAIN;
      break;
    }
    case TRI_VOC_COL_STATUS_UNLOADED: {
      // collection is unloaded
      StorageEngine* engine = EngineSelectorFeature::ENGINE;
      bool doSync =
          !engine->inRecovery() &&
          application_features::ApplicationServer::getFeature<DatabaseFeature>(
              "Database")
              ->forceSyncProperties();

      if (!collection->deleted()) {
        collection->deleted(true);
        try {
          engine->changeCollection(this, collection->id(), collection, doSync);
        } catch (arangodb::basics::Exception const& ex) {
          collection->deleted(false);
          events::DropCollection(colName, ex.code());
          return ex.code();
        } catch (std::exception const&) {
          collection->deleted(false);
          events::DropCollection(colName, TRI_ERROR_INTERNAL);
          return TRI_ERROR_INTERNAL;
        }
      }

      collection->setStatus(TRI_VOC_COL_STATUS_DELETED);
      unregisterCollection(collection);

      locker.unlock();

      writeLocker.unlock();

      TRI_ASSERT(engine != nullptr);
      engine->dropCollection(this, collection);

      DropCollectionCallback(collection);
      break;
    }
    case TRI_VOC_COL_STATUS_LOADED:
    case TRI_VOC_COL_STATUS_UNLOADING: {
      // collection is loaded
      collection->deleted(true);

      StorageEngine* engine = EngineSelectorFeature::ENGINE;
      bool doSync =
          !engine->inRecovery() &&
          application_features::ApplicationServer::getFeature<DatabaseFeature>(
              "Database")
              ->forceSyncProperties();

      VPackBuilder builder;
      engine->getCollectionInfo(this, collection->id(), builder, false, 0);
      arangodb::Result res = collection->updateProperties(
          builder.slice().get("parameters"), doSync);

      if (!res.ok()) {
        return res.errorNumber();
      }

      collection->setStatus(TRI_VOC_COL_STATUS_DELETED);
      unregisterCollection(collection);

      locker.unlock();
      writeLocker.unlock();

      engine->dropCollection(this, collection);
      state = DROP_PERFORM;
      break;
    }
    default: {
      // unknown status
      events::DropCollection(colName, TRI_ERROR_INTERNAL);
      return TRI_ERROR_INTERNAL;
    }
  }
  events::DropCollection(colName, TRI_ERROR_NO_ERROR);
  return TRI_ERROR_NO_ERROR;
}

/// @brief closes a database and all collections
void TRI_vocbase_t::shutdown() {
  // stop replication
  if (_replicationApplier != nullptr) {
    _replicationApplier->stopAndJoin();
  }

  // mark all cursors as deleted so underlying collections can be freed soon
  _cursorRepository->garbageCollect(true);

  // mark all collection keys as deleted so underlying collections can be freed
  // soon
  _collectionKeys->garbageCollect(true);

  std::vector<std::shared_ptr<arangodb::LogicalCollection>> collections;

  {
    RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);
    collections = _collections;
  }

  // from here on, the vocbase is unusable, i.e. no collections can be
  // created/loaded etc.

  // starts unloading of collections
  for (auto& collection : collections) {
    collection->close(); // required to release indexes
    unloadCollection(collection.get(), true);
  }

  // this will signal the compactor thread to do one last iteration
  setState(TRI_vocbase_t::State::SHUTDOWN_COMPACTOR);

  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  // shutdownDatabase() stops all threads
  engine->shutdownDatabase(this);

  // this will signal the cleanup thread to do one last iteration
  setState(TRI_vocbase_t::State::SHUTDOWN_CLEANUP);

  {
    RECURSIVE_WRITE_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);

    checkCollectionInvariants();
    _dataSourceByName.clear();
    _dataSourceById.clear();
    _dataSourceByUuid.clear();
    checkCollectionInvariants();
  }

  _deadCollections.clear();

  // free collections
  for (auto& collection : _collections) {
    collection->getPhysical()->close();
  }

  _collections.clear();
}

/// @brief returns names of all known (document) collections
std::vector<std::string> TRI_vocbase_t::collectionNames() {
  std::vector<std::string> result;

  RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);

  result.reserve(_dataSourceByName.size());

  for (auto& entry: _dataSourceByName) {
      TRI_ASSERT(entry.second);

      if (entry.second->category() != LogicalCollection::category()) {
        continue;
      }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      auto view = std::dynamic_pointer_cast<arangodb::LogicalCollection>(entry.second);
      TRI_ASSERT(view);
#else
      auto view = std::static_pointer_cast<arangodb::LogicalCollection>(entry.second);
#endif

      result.emplace_back(entry.first);
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns all known (document) collections with their parameters
/// and indexes, up to a specific tick value
/// while the collections are iterated over, there will be a global lock so
/// that there will be consistent view of collections & their properties
/// The list of collections will be sorted if sort function is given
////////////////////////////////////////////////////////////////////////////////

void TRI_vocbase_t::inventory(
    VPackBuilder& result,
    TRI_voc_tick_t maxTick, std::function<bool(arangodb::LogicalCollection const*)> const& nameFilter) {

  // cycle on write-lock
  WRITE_LOCKER_EVENTUAL(writeLock, _inventoryLock);

  std::vector<std::shared_ptr<arangodb::LogicalCollection>> collections;

  // copy collection pointers into vector so we can work with the copy without
  // the global lock
  {
    RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);
    collections = _collections;
  }

  if (collections.size() > 1) {
    // sort by type first and then only name
    // sorting by type ensures that document collections are reported before edge collections
    std::sort(
      collections.begin(),
      collections.end(),
      [](
        std::shared_ptr<LogicalCollection> const& lhs,
        std::shared_ptr<LogicalCollection> const& rhs
      )->bool {
      if (lhs->type() != rhs->type()) {
        return lhs->type() < rhs->type();
      }

      return lhs->name() < rhs->name();
    });
  }

  ExecContext const* exec = ExecContext::CURRENT;
  result.openArray();
  for (auto& collection : collections) {
    READ_LOCKER(readLocker, collection->_lock);

    if (collection->status() == TRI_VOC_COL_STATUS_DELETED ||
        collection->status() == TRI_VOC_COL_STATUS_CORRUPTED) {
      // we do not need to care about deleted or corrupted collections
      continue;
    }

    // In cluster case cids are not created by ticks but by cluster uniqIds
    if (!ServerState::instance()->isRunningInCluster() &&
        collection->id() > maxTick) {
      // collection is too new
      continue;
    }

    // check if we want this collection
    if (!nameFilter(collection.get())) {
      continue;
    }

    if (exec != nullptr &&
        !exec->canUseCollection(_name, collection->name(), auth::Level::RO)) {
      continue;
    }

    if (collection->id() <= maxTick) {
      result.openObject();

      result.add(VPackValue("indexes"));
      collection->getIndexesVPack(result, false, false, [](arangodb::Index const* idx) {
        // we have to exclude the primary and the edge index here, because otherwise
        // at least the MMFiles engine will try to create it
        return (idx->type() != arangodb::Index::TRI_IDX_TYPE_PRIMARY_INDEX && 
                idx->type() != arangodb::Index::TRI_IDX_TYPE_EDGE_INDEX);
      });
      result.add("parameters", VPackValue(VPackValueType::Object));
      collection->toVelocyPackIgnore(result, { "objectId", "path", "statusString" }, true, false);
      result.close();

      result.close();
    }
  }

  result.close();
}

/// @brief gets a collection name by a collection id
/// the name is fetched under a lock to make this thread-safe.
/// returns empty string if the collection does not exist.
std::string TRI_vocbase_t::collectionName(TRI_voc_cid_t id) {
  auto collection = lookupCollection(id);

  return collection ? collection->name() : StaticStrings::Empty;
}

/// @brief gets a view name by a view id
/// the name is fetched under a lock to make this thread-safe.
/// returns empty string if the view does not exist.
std::string TRI_vocbase_t::viewName(TRI_voc_cid_t id) const {
  auto view = lookupView(id);

  return view ? view->name() : StaticStrings::Empty;
}

/// @brief looks up a collection by identifier
std::shared_ptr<arangodb::LogicalCollection> TRI_vocbase_t::lookupCollection(
    TRI_voc_cid_t id
) const noexcept {
  #ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    return std::dynamic_pointer_cast<arangodb::LogicalCollection>(
      lookupDataSource(id)
    );
  #else
    auto dataSource = lookupDataSource(id);

    return dataSource && dataSource->category() == LogicalCollection::category()
      ? std::static_pointer_cast<LogicalCollection>(dataSource) : nullptr;
  #endif
}

/// @brief looks up a collection by name or stringified cid or uuid
std::shared_ptr<arangodb::LogicalCollection> TRI_vocbase_t::lookupCollection(
    std::string const& nameOrId
) const noexcept {
  #ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    return std::dynamic_pointer_cast<arangodb::LogicalCollection>(
      lookupDataSource(nameOrId)
    );
  #else
    auto dataSource = lookupDataSource(nameOrId);

    return dataSource && dataSource->category() == LogicalCollection::category()
      ? std::static_pointer_cast<LogicalCollection>(dataSource) : nullptr;
  #endif
}

/// @brief looks up a collection by uuid
std::shared_ptr<arangodb::LogicalCollection> TRI_vocbase_t::lookupCollectionByUuid(
    std::string const& uuid
) const noexcept {
  // otherwise we'll look up the collection by name
  RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);
  auto itr = _dataSourceByUuid.find(uuid);

  #ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    return itr == _dataSourceByUuid.end()
      ? nullptr
      : std::dynamic_pointer_cast<arangodb::LogicalCollection>(itr->second)
      ;
  #else
    return itr == _dataSourceByUuid.end()
           || itr->second->category() != LogicalCollection::category()
      ? nullptr
      : std::static_pointer_cast<LogicalCollection>(itr->second)
      ;
  #endif
}

/// @brief looks up a data-source by identifier
std::shared_ptr<arangodb::LogicalDataSource> TRI_vocbase_t::lookupDataSource(
    TRI_voc_cid_t id
) const noexcept {
  RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);
  auto itr = _dataSourceById.find(id);

  return itr == _dataSourceById.end() ? nullptr : itr->second;
}

/// @brief looks up a data-source by name
std::shared_ptr<arangodb::LogicalDataSource> TRI_vocbase_t::lookupDataSource(
    std::string const& nameOrId
) const noexcept {
  if (nameOrId.empty()) {
    return nullptr;
  }

  // lookup by id if the data-source name is passed as a stringified id
  bool success = false;
  auto id = arangodb::NumberUtils::atoi<TRI_voc_cid_t>(
    nameOrId.data(), nameOrId.data() + nameOrId.size(), success
  );

  if (success) {
    return lookupDataSource(id);
  }

  RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);

  // otherwise look up the data-source by name
  auto itr = _dataSourceByName.find(nameOrId);

  if (itr != _dataSourceByName.end()) {
    return itr->second;
  }

  // otherwise look up the data-source by UUID
  auto itrUuid = _dataSourceByUuid.find(nameOrId);

  return itrUuid == _dataSourceByUuid.end() ? nullptr : itrUuid->second;
}

/// @brief looks up a view by identifier
std::shared_ptr<arangodb::LogicalView> TRI_vocbase_t::lookupView(
  TRI_voc_cid_t id
) const noexcept {
  if (ServerState::instance()->isCoordinator()) {
    ClusterInfo* ci = ClusterInfo::instance();
    std::string viewId = StringUtils::itoa(id);
    return ci->getView(name(), viewId);
  }

  #ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    return std::dynamic_pointer_cast<arangodb::LogicalView>(
      lookupDataSource(id)
    );
  #else
    auto dataSource = lookupDataSource(id);

    return dataSource && dataSource->category() == LogicalView::category()
      ? std::static_pointer_cast<LogicalView>(dataSource) : nullptr;
  #endif
}

/// @brief looks up a view by name or stringified cid or uuid
std::shared_ptr<arangodb::LogicalView> TRI_vocbase_t::lookupView(
  std::string const& nameOrId
) const noexcept{
  if (ServerState::instance()->isCoordinator()) {
    ClusterInfo* ci = ClusterInfo::instance();
    return ci->getView(name(), nameOrId);
  }

  #ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    return std::dynamic_pointer_cast<arangodb::LogicalView>(
      lookupDataSource(nameOrId)
    );
  #else
    auto dataSource = lookupDataSource(nameOrId);

    return dataSource && dataSource->category() == LogicalView::category()
      ? std::static_pointer_cast<LogicalView>(dataSource) : nullptr;
  #endif
}

/// @brief creates a new collection from parameter set
/// collection id (cid) is normally passed with a value of 0
/// this means that the system will assign a new collection id automatically
/// using a cid of > 0 is supported to import dumps from other servers etc.
/// but the functionality is not advertised
arangodb::LogicalCollection* TRI_vocbase_t::createCollection(
    VPackSlice parameters) {
  // check that the name does not contain any strange characters
  if (!IsAllowedName(parameters)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_ILLEGAL_NAME);
  }

  // augment creation parameters
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  TRI_ASSERT(engine != nullptr);

  VPackBuilder merge;
  merge.openObject();
  engine->addParametersForNewCollection(merge, parameters);
  merge.close();

  merge = VPackCollection::merge(parameters, merge.slice(), true, false);
  parameters = merge.slice();

  READ_LOCKER(readLocker, _inventoryLock);

  // note: cid may be modified by this function call
  auto collection = createCollectionWorker(parameters);

  if (collection == nullptr) {
    // something went wrong... must not continue
    return nullptr;
  }

  arangodb::Result res2 = engine->persistCollection(this, collection.get());
  // API compatibility, we always return the collection, even if creation
  // failed.

  if (DatabaseFeature::DATABASE != nullptr &&
      DatabaseFeature::DATABASE->versionTracker() != nullptr) {
    DatabaseFeature::DATABASE->versionTracker()->track("create collection");
  }

  return collection.get();
}

/// @brief unloads a collection
int TRI_vocbase_t::unloadCollection(arangodb::LogicalCollection* collection,
                                    bool force) {
  {
    WRITE_LOCKER_EVENTUAL(locker, collection->_lock);

    // cannot unload a corrupted collection
    if (collection->status() == TRI_VOC_COL_STATUS_CORRUPTED) {
      return TRI_set_errno(TRI_ERROR_ARANGO_CORRUPTED_COLLECTION);
    }

    // an unloaded collection is unloaded
    if (collection->status() == TRI_VOC_COL_STATUS_UNLOADED) {
      return TRI_ERROR_NO_ERROR;
    }

    // an unloading collection is treated as unloaded
    if (collection->status() == TRI_VOC_COL_STATUS_UNLOADING) {
      return TRI_ERROR_NO_ERROR;
    }

    // a loading collection
    if (collection->status() == TRI_VOC_COL_STATUS_LOADING) {
      // throw away the write locker. we're going to switch to a read locker now
      locker.unlock();

      // loop until status changes
      while (1) {
        TRI_vocbase_col_status_e status;

        {
          READ_LOCKER_EVENTUAL(readLocker, collection->_lock);
          status = collection->status();
        }

        if (status != TRI_VOC_COL_STATUS_LOADING) {
          break;
        }
        // sleep without lock
        std::this_thread::sleep_for(std::chrono::microseconds(collectionStatusPollInterval()));
      }
      // if we get here, the status has changed
      return unloadCollection(collection, force);
    }

    // a deleted collection is treated as unloaded
    if (collection->status() == TRI_VOC_COL_STATUS_DELETED) {
      return TRI_ERROR_NO_ERROR;
    }

    // must be loaded
    if (collection->status() != TRI_VOC_COL_STATUS_LOADED) {
      return TRI_ERROR_INTERNAL;
    }

    // mark collection as unloading
    collection->setStatus(TRI_VOC_COL_STATUS_UNLOADING);
  }  // release locks

  collection->unload();

  // wake up the cleanup thread
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  engine->unloadCollection(this, collection);

  return TRI_ERROR_NO_ERROR;
}

/// @brief drops a collection
int TRI_vocbase_t::dropCollection(arangodb::LogicalCollection* collection,
                                  bool allowDropSystem, double timeout) {
  TRI_ASSERT(collection != nullptr);

  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  if (!allowDropSystem && collection->isSystem() && !engine->inRecovery()) {
    // prevent dropping of system collections
    return TRI_set_errno(TRI_ERROR_FORBIDDEN);
  }

  while (true) {
    DropState state = DROP_EXIT;
    int res;
    {
      READ_LOCKER(readLocker, _inventoryLock);
      res = dropCollectionWorker(collection, state, timeout);
    }

    if (state == DROP_PERFORM) {
      if (engine->inRecovery()) {
        DropCollectionCallback(collection);
      } else {
        collection->deferDropCollection(DropCollectionCallback);
        // wake up the cleanup thread
        engine->signalCleanup(collection->vocbase());
      }
      
      if (DatabaseFeature::DATABASE != nullptr &&
          DatabaseFeature::DATABASE->versionTracker() != nullptr) {
        DatabaseFeature::DATABASE->versionTracker()->track("drop collection");
      }
    }

    if (state == DROP_PERFORM || state == DROP_EXIT) {
      return res;
    }

    // try again in next iteration
    TRI_ASSERT(state == DROP_AGAIN);
    std::this_thread::sleep_for(std::chrono::microseconds(collectionStatusPollInterval()));
  }
}

/// @brief renames a view
int TRI_vocbase_t::renameView(
    std::shared_ptr<arangodb::LogicalView> const& view,
    std::string const& newName
) {
  // lock collection because we are going to copy its current name
  std::string oldName = view->name();

  // old name should be different

  // check if names are actually different
  if (oldName == newName) {
    return TRI_ERROR_NO_ERROR;
  }

  if (!IsAllowedName(IsSystemName(newName), arangodb::velocypack::StringRef(newName))) {
    return TRI_set_errno(TRI_ERROR_ARANGO_ILLEGAL_NAME);
  }

  READ_LOCKER(readLocker, _inventoryLock);

  RECURSIVE_WRITE_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);

  // Check for duplicate name
  auto it = _dataSourceByName.find(newName);

  if (it != _dataSourceByName.end()) {
    // new name already in use
    return TRI_ERROR_ARANGO_DUPLICATE_NAME;
  }

  // get the original pointer and ensure it's a LogicalView
  auto itr1 = _dataSourceByName.find(oldName);

  if (itr1 == _dataSourceByName.end()
      || arangodb::LogicalView::category() == itr1->second->category()) {
    return TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND;
  }

  TRI_ASSERT(std::dynamic_pointer_cast<arangodb::LogicalView>(itr1->second));

  _dataSourceByName.emplace(newName, view);
  _dataSourceByName.erase(oldName);

  // stores the parameters on disk
  auto* databaseFeature =
    application_features::ApplicationServer::getFeature<DatabaseFeature>("Database");
  TRI_ASSERT(databaseFeature);
  auto doSync = databaseFeature->forceSyncProperties();
  auto res = view->rename(std::string(newName), doSync);

  if (!res.ok()) {
    return res.errorNumber(); // rename failed
  }

  // Tell the engine.
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  TRI_ASSERT(engine != nullptr);
  res = engine->renameView(this, view, oldName);

  return res.errorNumber();
}

/// @brief renames a collection
int TRI_vocbase_t::renameCollection(
    arangodb::LogicalCollection* collection,
    std::string const& newName,
    bool doOverride
) {
  if (collection->isSystem()) {
    return TRI_set_errno(TRI_ERROR_FORBIDDEN);
  }

  // lock collection because we are going to copy its current name
  std::string oldName = collection->name();

  // old name should be different

  // check if names are actually different
  if (oldName == newName) {
    return TRI_ERROR_NO_ERROR;
  }

  if (!doOverride) {
    auto isSystem = IsSystemName(oldName);

    if (isSystem && !IsSystemName(newName)) {
      // a system collection shall not be renamed to a non-system collection
      // name
      return TRI_set_errno(TRI_ERROR_ARANGO_ILLEGAL_NAME);
    } else if (!isSystem && IsSystemName(newName)) {
      // a non-system collection shall not be renamed to a system collection
      // name
      return TRI_set_errno(TRI_ERROR_ARANGO_ILLEGAL_NAME);
    }

    if (!IsAllowedName(isSystem, arangodb::velocypack::StringRef(newName))) {
      return TRI_set_errno(TRI_ERROR_ARANGO_ILLEGAL_NAME);
    }
  }

  READ_LOCKER(readLocker, _inventoryLock);

  RECURSIVE_WRITE_LOCKER_NAMED(writeLocker, _dataSourceLock, _dataSourceLockWriteOwner, false);
  CONDITIONAL_WRITE_LOCKER(locker, collection->_lock, false);

  while (true) {
    TRI_ASSERT(!writeLocker.isLocked());
    TRI_ASSERT(!locker.isLocked());

    // block until we have acquired this lock
    writeLocker.lock();
    // we now have the one lock

    TRI_ASSERT(writeLocker.isLocked());

    if (locker.tryLock()) {
      // we now have both locks and can continue outside of this loop
      break;
    }

    // unlock the write locker so we don't block other operations
    writeLocker.unlock();

    TRI_ASSERT(!writeLocker.isLocked());
    TRI_ASSERT(!locker.isLocked());

    // sleep for a while
    std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::microseconds(10000));
  }

  TRI_ASSERT(writeLocker.isLocked());
  TRI_ASSERT(locker.isLocked());

  // Check for duplicate name
  auto itr = _dataSourceByName.find(newName);

  if (itr != _dataSourceByName.end()) {
    return TRI_ERROR_ARANGO_DUPLICATE_NAME;
  }

  // get the original pointer and ensure it's a LogicalCollection
  auto itr1 = _dataSourceByName.find(oldName);

  if (itr1 == _dataSourceByName.end()
      || arangodb::LogicalCollection::category() != itr1->second->category()) {
    return TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND;
  }

  TRI_ASSERT(std::dynamic_pointer_cast<arangodb::LogicalCollection>(itr1->second));

  auto* databaseFeature =
    application_features::ApplicationServer::getFeature<DatabaseFeature>("Database");
  TRI_ASSERT(databaseFeature);
  auto doSync = databaseFeature->forceSyncProperties();
  auto res = collection->rename(std::string(newName), doSync);

  if (!res.ok()) {
    res.errorNumber(); // rename failed
  }

  // The collection is renamed. Now swap cache entries.
  auto it2 = _dataSourceByName.emplace(newName, itr1->second);
  TRI_ASSERT(it2.second);

  try {
    _dataSourceByName.erase(oldName);
  } catch (...) {
    _dataSourceByName.erase(newName);
    throw;
  }

  checkCollectionInvariants();
  locker.unlock();
  writeLocker.unlock();

  if (DatabaseFeature::DATABASE != nullptr &&
      DatabaseFeature::DATABASE->versionTracker() != nullptr) {
    DatabaseFeature::DATABASE->versionTracker()->track("rename collection");
  }

  // invalidate all entries for the two collections
  arangodb::aql::PlanCache::instance()->invalidate(this);
  arangodb::aql::QueryCache::instance()->invalidate(
      this, std::vector<std::string>{oldName, newName});

  // Tell the engine.
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  TRI_ASSERT(engine != nullptr);
  arangodb::Result res2 =
    engine->renameCollection(this, collection, oldName);

  return res2.errorNumber();
}

/// @brief locks a collection for usage, loading or manifesting it
int TRI_vocbase_t::useCollection(arangodb::LogicalCollection* collection,
                                 TRI_vocbase_col_status_e& status) {
  return loadCollection(collection, status);
}

/// @brief locks a (document) collection for usage by id
arangodb::LogicalCollection* TRI_vocbase_t::useCollection(
    TRI_voc_cid_t cid, TRI_vocbase_col_status_e& status) {
  auto collection = lookupCollection(cid);

  return useCollectionInternal(collection.get(), status);
}

/// @brief locks a collection for usage by name
arangodb::LogicalCollection* TRI_vocbase_t::useCollection(
    std::string const& name, TRI_vocbase_col_status_e& status) {
  // check that we have an existing name
  arangodb::LogicalCollection* collection = nullptr;

  {
    RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);

    auto it = _dataSourceByName.find(name);

    if (it != _dataSourceByName.end()
        && it->second->category() == LogicalCollection::category()) {
      TRI_ASSERT(std::dynamic_pointer_cast<LogicalCollection>(it->second));
      collection = static_cast<LogicalCollection*>(it->second.get());
    }
  }

  return useCollectionInternal(collection, status);
}

/// @brief locks a collection for usage by name
arangodb::LogicalCollection* TRI_vocbase_t::useCollectionByUuid(
    std::string const& uuid, TRI_vocbase_col_status_e& status) {
  auto collection = lookupCollectionByUuid(uuid);

  return useCollectionInternal(collection.get(), status);
}

arangodb::LogicalCollection* TRI_vocbase_t::useCollectionInternal(
    arangodb::LogicalCollection* collection, TRI_vocbase_col_status_e& status) {
  if (collection == nullptr) {
    TRI_set_errno(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    return nullptr;
  }

  // try to load the collection
  int res = loadCollection(collection, status);

  if (res == TRI_ERROR_NO_ERROR) {
    return collection;
  }

  TRI_set_errno(res);
  return nullptr;
}

/// @brief releases a collection from usage
void TRI_vocbase_t::releaseCollection(arangodb::LogicalCollection* collection) {
  collection->_lock.unlock();
}

/// @brief creates a new view, worker function
std::shared_ptr<arangodb::LogicalView> TRI_vocbase_t::createViewWorker(
    VPackSlice parameters, TRI_voc_cid_t& id) {
  std::string name = arangodb::basics::VelocyPackHelper::getStringValue(
      parameters, "name", "");

  // check that the name does not contain any strange characters
  if (!IsAllowedName(parameters)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_ILLEGAL_NAME);
  }

  // Try to create a new view. This is not registered yet
  auto const view = LogicalView::create(*this, parameters, true);

  if (!view) {
    auto const message = "failed to instantiate view '" + name + "'";

    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, message.c_str());
  }

  RECURSIVE_WRITE_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);

  auto it = _dataSourceByName.find(name);

  if (it != _dataSourceByName.end()) {
    events::CreateView(name, TRI_ERROR_ARANGO_DUPLICATE_NAME);

    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DUPLICATE_NAME);
  }

  registerView(basics::ConditionalLocking::DoNotLock, view);

  try {
    // And lets open it.
    view->open();

    events::CreateView(name, TRI_ERROR_NO_ERROR);
  } catch (...) {
    unregisterView(view);
    throw;
  }

  // noexcept below
  id = view->id();

  return view;
}

/// @brief creates a new view from parameter set
/// view id is normally passed with a value of 0
/// this means that the system will assign a new id automatically
/// using a cid of > 0 is supported to import dumps from other servers etc.
/// but the functionality is not advertised
std::shared_ptr<arangodb::LogicalView> TRI_vocbase_t::createView(
    VPackSlice parameters, TRI_voc_cid_t id) {

  if (ServerState::instance()->isCoordinator()) {
    ClusterInfo* ci = ClusterInfo::instance();
    if (id == 0) {
      id = ci->uniqid();
    }
    std::string viewId = StringUtils::itoa(id);
    // Now put together the JSON we need for the agency:
    VPackBuilder builder;
    { VPackObjectBuilder guard(&builder);
      builder.add("id", VPackValue(viewId));
      builder.add(VPackValue("properties"));
      std::string name;
      { VPackObjectBuilder guard(&builder);
        for (auto const& p : VPackObjectIterator(parameters)) {
          if (p.key.copyString() == "name" && p.value.isString()) {
            name = p.value.copyString();
          } else {
            builder.add(p.key);
            builder.add(p.value);
          }
        }
      }
      if (name.empty()) {
        LOG_TOPIC(ERR, arangodb::Logger::CLUSTER)
          << "Could not create view in agency, error: no name given.";
        return nullptr;
      }
      builder.add("name", VPackValue(name));
      builder.add(VPackValue("collections"));
      { VPackArrayBuilder guard2(&builder);
      }
    }
    std::string errorMsg;
    int res = ci->createViewCoordinator(name(), viewId, builder.slice(),
        errorMsg);
    if (res == TRI_ERROR_NO_ERROR) {
      return ci->getView(name(), viewId);
    }
    LOG_TOPIC(ERR, arangodb::Logger::CLUSTER)
      << "Could not create view in agency, error: " << errorMsg
      << ", errorCode: " << res;
    return nullptr;
  }

  READ_LOCKER(readLocker, _inventoryLock);

  // note: id may be modified by this function call
  std::shared_ptr<LogicalView> view = createViewWorker(parameters, id);

  if (view == nullptr) {
    // something went wrong... must not continue
    return nullptr;
  }

  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  TRI_ASSERT(engine != nullptr);
  // TODO Review
  arangodb::Result res2 = engine->persistView(this, view.get());
  // API compatibility, we always return the view, even if creation failed.

  if (view) {
    if (DatabaseFeature::DATABASE != nullptr &&
        DatabaseFeature::DATABASE->versionTracker() != nullptr) {
      DatabaseFeature::DATABASE->versionTracker()->track("create view");
    }
  }

  return view;
}

int TRI_vocbase_t::dropView(std::string const& name) {
  std::shared_ptr<LogicalView> view = lookupView(name);

  if (view == nullptr) {
    return TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND;
  }

  return dropView(view);
}

/// @brief drops a view
int TRI_vocbase_t::dropView(std::shared_ptr<arangodb::LogicalView> view) {

  TRI_ASSERT(view != nullptr);

  if (ServerState::instance()->isCoordinator()) {
    ClusterInfo* ci = ClusterInfo::instance();
    std::string errorMsg;
    int res = ci->dropViewCoordinator(name(), StringUtils::itoa(view->id()),
          errorMsg);
    if (res == TRI_ERROR_NO_ERROR) {
      return res;
    }
    LOG_TOPIC(ERR, arangodb::Logger::CLUSTER)
      << "Could not drop view in agency, error: " << errorMsg
      << ", errorCode: " << res;
    return res;
  }

  READ_LOCKER(readLocker, _inventoryLock);

  // do not acquire these locks instantly
  RECURSIVE_WRITE_LOCKER_NAMED(writeLocker, _dataSourceLock, _dataSourceLockWriteOwner,
                           basics::ConditionalLocking::DoNotLock);
  CONDITIONAL_WRITE_LOCKER(locker, view->_lock,
                           basics::ConditionalLocking::DoNotLock);

  while (true) {
    TRI_ASSERT(!writeLocker.isLocked());
    TRI_ASSERT(!locker.isLocked());

    // block until we have acquired this lock
    writeLocker.lock();
    // we now have the one lock

    TRI_ASSERT(writeLocker.isLocked());

    if (locker.tryLock()) {
      // we now have both locks and can continue outside of this loop
      break;
    }

    // unlock the write locker so we don't block other operations
    writeLocker.unlock();

    TRI_ASSERT(!writeLocker.isLocked());
    TRI_ASSERT(!locker.isLocked());

    // sleep for a while
    std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::microseconds(10000));
  }

  TRI_ASSERT(writeLocker.isLocked());
  TRI_ASSERT(locker.isLocked());

  arangodb::aql::PlanCache::instance()->invalidate(this);
  arangodb::aql::QueryCache::instance()->invalidate(this);

  view->drop();
  unregisterView(view);

  locker.unlock();
  writeLocker.unlock();

  events::DropView(view->name(), TRI_ERROR_NO_ERROR);
  if (DatabaseFeature::DATABASE != nullptr &&
      DatabaseFeature::DATABASE->versionTracker() != nullptr) {
    DatabaseFeature::DATABASE->versionTracker()->track("drop view");
  }

  return TRI_ERROR_NO_ERROR;
}

/// @brief create a vocbase object
TRI_vocbase_t::TRI_vocbase_t(TRI_vocbase_type_e type, TRI_voc_tick_t id,
                             std::string const& name)
    : _id(id),
      _name(name),
      _type(type),
      _refCount(0),
      _state(TRI_vocbase_t::State::NORMAL),
      _isOwnAppsDirectory(true),
      _deadlockDetector(false),
      _userStructures(nullptr) {
  _queries.reset(new arangodb::aql::QueryList(this));
  _cursorRepository.reset(new arangodb::CursorRepository(this));
  _collectionKeys.reset(new arangodb::CollectionKeysRepository());

  // init collections
  _collections.reserve(32);
  _deadCollections.reserve(32);

  TRI_CreateUserStructuresVocBase(this);
}

/// @brief destroy a vocbase object
TRI_vocbase_t::~TRI_vocbase_t() {
  if (_userStructures != nullptr) {
    TRI_FreeUserStructuresVocBase(this);
  }

  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  engine->shutdownDatabase(this);

  // do a final cleanup of collections
  for (auto& it : _collections) {
    it->close(); // required to release indexes
  }
}

std::string TRI_vocbase_t::path() const {
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  return engine->databasePath(this);
}

bool TRI_vocbase_t::IsAllowedName(arangodb::velocypack::Slice slice) noexcept {
  return !slice.isObject()
    ? false
    : IsAllowedName(
        arangodb::basics::VelocyPackHelper::readBooleanValue(slice, "isSystem", false),
        arangodb::basics::VelocyPackHelper::getStringRef(slice, "name", "")
      )
    ;
}

/// @brief checks if a database name is allowed
/// returns true if the name is allowed and false otherwise
bool TRI_vocbase_t::IsAllowedName(
    bool allowSystem,
    arangodb::velocypack::StringRef const& name
) noexcept {
  size_t length = 0;

  // check allow characters: must start with letter or underscore if system is
  // allowed
  for (char const* ptr = name.data(); length < name.size(); ++ptr, ++length) {
    bool ok;
    if (length == 0) {
      if (allowSystem) {
        ok = (*ptr == '_') || ('a' <= *ptr && *ptr <= 'z') ||
             ('A' <= *ptr && *ptr <= 'Z');
      } else {
        ok = ('a' <= *ptr && *ptr <= 'z') || ('A' <= *ptr && *ptr <= 'Z');
      }
    } else {
      ok = (*ptr == '_') || (*ptr == '-') || ('0' <= *ptr && *ptr <= '9') ||
           ('a' <= *ptr && *ptr <= 'z') || ('A' <= *ptr && *ptr <= 'Z');
    }

    if (!ok) {
      return false;
    }
  }

  // invalid name length
  if (length == 0 || length > TRI_COL_NAME_LENGTH) {
    return false;
  }

  return true;
}

/// @brief determine whether a collection name is a system collection name
/*static*/ bool TRI_vocbase_t::IsSystemName(std::string const& name) noexcept {
  return !name.empty() && name[0] == '_';
}

void TRI_vocbase_t::addReplicationApplier() {
  DatabaseReplicationApplier* applier = DatabaseReplicationApplier::create(this);
  _replicationApplier.reset(applier);
}

/// @brief note the progress of a connected replication client
/// this only updates the ttl
void TRI_vocbase_t::updateReplicationClient(TRI_server_id_t serverId, double ttl) {
  if (ttl <= 0.0) {
    ttl = InitialSyncer::defaultBatchTimeout;
  }
  double const expires = TRI_microtime() + ttl;

  WRITE_LOCKER(writeLocker, _replicationClientsLock);

  auto it = _replicationClients.find(serverId);

  if (it != _replicationClients.end()) {
    LOG_TOPIC(TRACE, Logger::REPLICATION) << "updating replication client entry for server '" << serverId << "' using TTL " << ttl;
    (*it).second.first = expires;
  } else {
    LOG_TOPIC(TRACE, Logger::REPLICATION) << "replication client entry for server '" << serverId << "' not found";
  }
}

/// @brief note the progress of a connected replication client
void TRI_vocbase_t::updateReplicationClient(TRI_server_id_t serverId,
                                            TRI_voc_tick_t lastFetchedTick,
                                            double ttl) {
  if (ttl <= 0.0) {
    ttl = InitialSyncer::defaultBatchTimeout;
  }
  double const expires = TRI_microtime() + ttl;

  WRITE_LOCKER(writeLocker, _replicationClientsLock);

  try {
    auto it = _replicationClients.find(serverId);

    if (it == _replicationClients.end()) {
      // insert new client entry
      _replicationClients.emplace(
          serverId, std::make_pair(expires, lastFetchedTick));
      LOG_TOPIC(TRACE, Logger::REPLICATION) << "inserting replication client entry for server '" << serverId << "' using TTL " << ttl << ", last tick: " << lastFetchedTick;
    } else {
      // update an existing client entry
      (*it).second.first = expires;
      if (lastFetchedTick > 0) {
        (*it).second.second = lastFetchedTick;
        LOG_TOPIC(TRACE, Logger::REPLICATION) << "updating replication client entry for server '" << serverId << "' using TTL " << ttl << ", last tick: " << lastFetchedTick;
      } else {
        LOG_TOPIC(TRACE, Logger::REPLICATION) << "updating replication client entry for server '" << serverId << "' using TTL " << ttl;
      }
    }
  } catch (...) {
    // silently fail...
    // all we would be missing is the progress information of a slave
  }
}

/// @brief return the progress of all replication clients
std::vector<std::tuple<TRI_server_id_t, double, TRI_voc_tick_t>>
TRI_vocbase_t::getReplicationClients() {
  std::vector<std::tuple<TRI_server_id_t, double, TRI_voc_tick_t>> result;

  READ_LOCKER(readLocker, _replicationClientsLock);

  for (auto& it : _replicationClients) {
    result.emplace_back(
        std::make_tuple(it.first, it.second.first, it.second.second));
  }
  return result;
}

void TRI_vocbase_t::garbageCollectReplicationClients(double expireStamp) {
  LOG_TOPIC(TRACE, Logger::REPLICATION) << "garbage collecting replication client entries";

  WRITE_LOCKER(writeLocker, _replicationClientsLock);

  try {
    auto it = _replicationClients.begin();

    while (it != _replicationClients.end()) {
      double const expires = it->second.first;
      if (expireStamp > expires) {
        LOG_TOPIC(DEBUG, Logger::REPLICATION) << "removing expired replication client for server id " << it->first;
        it = _replicationClients.erase(it);
      } else {
        ++it;
      }
    }
  } catch (...) {
    // silently fail...
    // all we would be missing is the progress information of a slave
  }
}

std::vector<std::shared_ptr<arangodb::LogicalView>> TRI_vocbase_t::views() {
  std::vector<std::shared_ptr<arangodb::LogicalView>> views;

  {
    RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);
    views.reserve(_dataSourceById.size());

    for (auto& entry: _dataSourceById) {
      TRI_ASSERT(entry.second);

      if (entry.second->category() != LogicalView::category()) {
        continue;
      }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      auto view = std::dynamic_pointer_cast<arangodb::LogicalView>(entry.second);
      TRI_ASSERT(view);
#else
      auto view = std::static_pointer_cast<arangodb::LogicalView>(entry.second);
#endif

      views.emplace_back(view);
    }
  }

  return views;
}

void TRI_vocbase_t::processCollections(
    std::function<void(LogicalCollection*)> const& cb, bool includeDeleted) {
  RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);

  if (includeDeleted) {
    for (auto const& it : _collections) {
      cb(it.get());
    }
  } else {
    for (auto& entry: _dataSourceById) {
      TRI_ASSERT(entry.second);

      if (entry.second->category() != LogicalCollection::category()) {
        continue;
      }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      auto collection = std::dynamic_pointer_cast<arangodb::LogicalCollection>(entry.second);
      TRI_ASSERT(collection);
#else
      auto collection = std::static_pointer_cast<arangodb::LogicalCollection>(entry.second);
#endif

      cb(collection.get());
    }
  }
}

std::vector<arangodb::LogicalCollection*> TRI_vocbase_t::collections(
    bool includeDeleted
) {
  std::vector<arangodb::LogicalCollection*> collections;

  {
    RECURSIVE_READ_LOCKER(_dataSourceLock, _dataSourceLockWriteOwner);
    if (includeDeleted) {
      // return deleted collections as well. the cleanup thread needs them
      collections.reserve(_collections.size());

      for (auto const& it : _collections) {
        collections.emplace_back(it.get());
      }
    } else {
      collections.reserve(_dataSourceById.size());

      for (auto& entry: _dataSourceById) {
        TRI_ASSERT(entry.second);

        if (entry.second->category() != LogicalCollection::category()) {
          continue;
        }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        auto collection = std::dynamic_pointer_cast<arangodb::LogicalCollection>(entry.second);
        TRI_ASSERT(collection);
#else
        auto collection = std::static_pointer_cast<arangodb::LogicalCollection>(entry.second);
#endif

        collections.emplace_back(collection.get());
      }
    }
  }

  return collections;
}

/// @brief extract the _rev attribute from a slice
TRI_voc_rid_t TRI_ExtractRevisionId(VPackSlice slice) {
  slice = slice.resolveExternal();
  TRI_ASSERT(slice.isObject());

  VPackSlice r(slice.get(StaticStrings::RevString));
  if (r.isString()) {
    VPackValueLength l;
    char const* p = r.getString(l);
    return TRI_StringToRid(p, l, false);
  }
  if (r.isInteger()) {
    return r.getNumber<TRI_voc_rid_t>();
  }
  return 0;
}

/// @brief extract the _rev attribute from a slice as a slice
VPackSlice TRI_ExtractRevisionIdAsSlice(VPackSlice const slice) {
  if (!slice.isObject()) {
    return VPackSlice();
  }

  return slice.get(StaticStrings::RevString);
}

/// @brief sanitize an object, given as slice, builder must contain an
/// open object which will remain open
/// the result is the object excluding _id, _key and _rev
void TRI_SanitizeObject(VPackSlice const slice, VPackBuilder& builder) {
  TRI_ASSERT(slice.isObject());
  VPackObjectIterator it(slice);
  while (it.valid()) {
    StringRef key(it.key());
    if (key.empty() || key[0] != '_' ||
        (key != StaticStrings::KeyString && key != StaticStrings::IdString &&
         key != StaticStrings::RevString)) {
      builder.add(key.data(), key.size(), it.value());
    }
    it.next();
  }
}

/// @brief sanitize an object, given as slice, builder must contain an
/// open object which will remain open. also excludes _from and _to
void TRI_SanitizeObjectWithEdges(VPackSlice const slice,
                                 VPackBuilder& builder) {
  TRI_ASSERT(slice.isObject());
  VPackObjectIterator it(slice, true);
  while (it.valid()) {
    StringRef key(it.key());
    if (key.empty() || key[0] != '_' ||
        (key != StaticStrings::KeyString && key != StaticStrings::IdString &&
         key != StaticStrings::RevString && key != StaticStrings::FromString &&
         key != StaticStrings::ToString)) {
      builder.add(key.data(), key.length(), it.value());
    }
    it.next();
  }
}

/// @brief Convert a revision ID to a string
constexpr static TRI_voc_rid_t tickLimit =
    static_cast<TRI_voc_rid_t>(2016ULL - 1970ULL) * 1000ULL * 60ULL * 60ULL *
    24ULL * 365ULL;

std::string TRI_RidToString(TRI_voc_rid_t rid) {
  if (rid <= tickLimit) {
    return arangodb::basics::StringUtils::itoa(rid);
  }
  return HybridLogicalClock::encodeTimeStamp(rid);
}

/// @brief Convert a string into a revision ID, no check variant
TRI_voc_rid_t TRI_StringToRid(char const* p, size_t len, bool warn) {
  bool isOld;
  return TRI_StringToRid(p, len, isOld, warn);
}

/// @brief Convert a string into a revision ID, returns 0 if format invalid
TRI_voc_rid_t TRI_StringToRid(std::string const& ridStr, bool& isOld,
                              bool warn) {
  return TRI_StringToRid(ridStr.c_str(), ridStr.size(), isOld, warn);
}

/// @brief Convert a string into a revision ID, returns 0 if format invalid
TRI_voc_rid_t TRI_StringToRid(char const* p, size_t len, bool& isOld,
                              bool warn) {
  if (len > 0 && *p >= '1' && *p <= '9') {
    auto r = NumberUtils::atoi_positive_unchecked<TRI_voc_rid_t>(p, p + len);
    if (warn && r > tickLimit) {
      // An old tick value that could be confused with a time stamp
      LOG_TOPIC(WARN, arangodb::Logger::FIXME)
          << "Saw old _rev value that could be confused with a time stamp!";
    }
    isOld = true;
    return r;
  }
  isOld = false;
  return HybridLogicalClock::decodeTimeStamp(p, len);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
