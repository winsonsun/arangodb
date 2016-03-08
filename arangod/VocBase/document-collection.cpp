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

#include "document-collection.h"
#include "Aql/QueryCache.h"
#include "Basics/Barrier.h"
#include "Basics/conversions.h"
#include "Basics/Exceptions.h"
#include "Basics/FileUtils.h"
#include "Basics/files.h"
#include "Basics/Logger.h"
#include "Basics/tri-strings.h"
#include "Basics/ThreadPool.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Cluster/ServerState.h"
#include "FulltextIndex/fulltext-index.h"
#include "Indexes/EdgeIndex.h"
#include "Indexes/FulltextIndex.h"
#include "Indexes/GeoIndex2.h"
#include "Indexes/HashIndex.h"
#include "Indexes/PrimaryIndex.h"
#include "Indexes/SkiplistIndex.h"
#include "RestServer/ArangoServer.h"
#include "Utils/CollectionReadLocker.h"
#include "Utils/CollectionWriteLocker.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/DatafileHelper.h"
#include "VocBase/Ditch.h"
#include "VocBase/edge-collection.h"
#include "VocBase/ExampleMatcher.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/MasterPointers.h"
#include "VocBase/server.h"
#include "VocBase/update-policy.h"
#include "VocBase/VocShaper.h"
#include "Wal/DocumentOperation.h"
#include "Wal/LogfileManager.h"
#include "Wal/Marker.h"
#include "Wal/Slots.h"

#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <velocypack/Value.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
   
////////////////////////////////////////////////////////////////////////////////
/// @brief create a document collection
////////////////////////////////////////////////////////////////////////////////

TRI_document_collection_t::TRI_document_collection_t()
    : _lock(),
      _shaper(nullptr),
      _nextCompactionStartIndex(0),
      _lastCompactionStatus(nullptr),
      _useSecondaryIndexes(true),
      _ditches(this),
      _masterPointers(),
      _keyGenerator(nullptr),
      _uncollectedLogfileEntries(0),
      _cleanupIndexes(0) {
  _tickMax = 0;

  setCompactionStatus("compaction not yet started");
  if (ServerState::instance()->isDBServer()) {
    _followers.reset(new FollowerInfo(this));
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy a document collection
////////////////////////////////////////////////////////////////////////////////

TRI_document_collection_t::~TRI_document_collection_t() {
  delete _keyGenerator;
}

std::string TRI_document_collection_t::label() const {
  return std::string(_vocbase->_name) + " / " + _info.name();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief update statistics for a collection
/// note: the write-lock for the collection must be held to call this
////////////////////////////////////////////////////////////////////////////////

void TRI_document_collection_t::setLastRevision(TRI_voc_rid_t rid, bool force) {
  if (rid > 0) {
    _info.setRevision(rid, force);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not a collection is fully collected
////////////////////////////////////////////////////////////////////////////////

bool TRI_document_collection_t::isFullyCollected() {
  READ_LOCKER(readLocker, _lock);

  int64_t uncollected = _uncollectedLogfileEntries.load();

  return (uncollected == 0);
}

void TRI_document_collection_t::setNextCompactionStartIndex(size_t index) {
  MUTEX_LOCKER(mutexLocker, _compactionStatusLock);
  _nextCompactionStartIndex = index;
}

size_t TRI_document_collection_t::getNextCompactionStartIndex() {
  MUTEX_LOCKER(mutexLocker, _compactionStatusLock);
  return _nextCompactionStartIndex;
}

void TRI_document_collection_t::setCompactionStatus(char const* reason) {
  TRI_ASSERT(reason != nullptr);
  struct tm tb;
  time_t tt = time(nullptr);
  TRI_gmtime(tt, &tb);

  MUTEX_LOCKER(mutexLocker, _compactionStatusLock);
  _lastCompactionStatus = reason;

  strftime(&_lastCompactionStamp[0], sizeof(_lastCompactionStamp),
           "%Y-%m-%dT%H:%M:%SZ", &tb);
}

void TRI_document_collection_t::getCompactionStatus(char const*& reason,
                                                    char* dst, size_t maxSize) {
  memset(dst, 0, maxSize);
  if (maxSize > sizeof(_lastCompactionStamp)) {
    maxSize = sizeof(_lastCompactionStamp);
  }
  MUTEX_LOCKER(mutexLocker, _compactionStatusLock);
  reason = _lastCompactionStatus;
  memcpy(dst, &_lastCompactionStamp[0], maxSize);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read locks a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::beginRead() {
  if (arangodb::Transaction::_makeNolockHeaders != nullptr) {
    std::string collName(_info.name());
    auto it = arangodb::Transaction::_makeNolockHeaders->find(collName);
    if (it != arangodb::Transaction::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "BeginRead blocked: " << document->_info._name <<
      // std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }
  // LOCKING-DEBUG
  // std::cout << "BeginRead: " << document->_info._name << std::endl;
  TRI_READ_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this);
  
  try { 
    _vocbase->_deadlockDetector.addReader(this, false);
  }
  catch (...) {
    TRI_READ_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this);
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read unlocks a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::endRead() {
  if (arangodb::Transaction::_makeNolockHeaders != nullptr) {
    std::string collName(_info.name());
    auto it = arangodb::Transaction::_makeNolockHeaders->find(collName);
    if (it != arangodb::Transaction::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "EndRead blocked: " << document->_info._name << std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }
  
  try { 
    _vocbase->_deadlockDetector.unsetReader(this);
  }
  catch (...) {
  }
  
  // LOCKING-DEBUG
  // std::cout << "EndRead: " << document->_info._name << std::endl;
  TRI_READ_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write locks a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::beginWrite() {
  if (arangodb::Transaction::_makeNolockHeaders != nullptr) {
    std::string collName(_info.name());
    auto it = arangodb::Transaction::_makeNolockHeaders->find(collName);
    if (it != arangodb::Transaction::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "BeginWrite blocked: " << document->_info._name <<
      // std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }
  // LOCKING_DEBUG
  // std::cout << "BeginWrite: " << document->_info._name << std::endl;
  TRI_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this);
  
  // register writer
  try { 
    _vocbase->_deadlockDetector.addWriter(this, false);
  }
  catch (...) {
    TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this);
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write unlocks a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::endWrite() {
  if (arangodb::Transaction::_makeNolockHeaders != nullptr) {
    std::string collName(_info.name());
    auto it = arangodb::Transaction::_makeNolockHeaders->find(collName);
    if (it != arangodb::Transaction::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "EndWrite blocked: " << document->_info._name <<
      // std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }
  
  // unregister writer
  try {
    _vocbase->_deadlockDetector.unsetWriter(this);
  }
  catch (...) {
    // must go on here to unlock the lock
  }

  // LOCKING-DEBUG
  // std::cout << "EndWrite: " << document->_info._name << std::endl;
  TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief read locks a collection, with a timeout (in µseconds)
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::beginReadTimed(uint64_t timeout,
                                              uint64_t sleepPeriod) {
  if (arangodb::Transaction::_makeNolockHeaders != nullptr) {
    std::string collName(_info.name());
    auto it = arangodb::Transaction::_makeNolockHeaders->find(collName);
    if (it != arangodb::Transaction::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "BeginReadTimed blocked: " << document->_info._name <<
      // std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }
  uint64_t waited = 0;
  if (timeout == 0) {
    // we don't allow looping forever. limit waiting to 15 minutes max.
    timeout = 15 * 60 * 1000 * 1000;
  }

  // LOCKING-DEBUG
  // std::cout << "BeginReadTimed: " << document->_info._name << std::endl;
  int iterations = 0;
  bool wasBlocked = false;

  while (!TRI_TRY_READ_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this)) {
    try {
      if (!wasBlocked) {
        // insert reader
        wasBlocked = true;
        if (_vocbase->_deadlockDetector.setReaderBlocked(this) == TRI_ERROR_DEADLOCK) {
          // deadlock
          LOG(TRACE) << "deadlock detected while trying to acquire read-lock on collection '" << _info.namec_str() << "'";
          return TRI_ERROR_DEADLOCK;
        }
        LOG(TRACE) << "waiting for read-lock on collection '" << _info.namec_str() << "'";
      } else if (++iterations >= 5) {
        // periodically check for deadlocks
        TRI_ASSERT(wasBlocked);
        iterations = 0;
        if (_vocbase->_deadlockDetector.detectDeadlock(this, false) == TRI_ERROR_DEADLOCK) {
          // deadlock
          _vocbase->_deadlockDetector.unsetReaderBlocked(this);
          LOG(TRACE) << "deadlock detected while trying to acquire read-lock on collection '" << _info.namec_str() << "'";
          return TRI_ERROR_DEADLOCK;
        }
      }
    } catch (...) {
      // clean up!
      if (wasBlocked) {
        _vocbase->_deadlockDetector.unsetReaderBlocked(this);
      }
      // always exit
      return TRI_ERROR_OUT_OF_MEMORY;
    }

#ifdef _WIN32
    usleep((unsigned long)sleepPeriod);
#else
    usleep((useconds_t)sleepPeriod);
#endif

    waited += sleepPeriod;

    if (waited > timeout) {
      _vocbase->_deadlockDetector.unsetReaderBlocked(this);
      LOG(TRACE) << "timed out waiting for read-lock on collection '" << _info.namec_str() << "'";
      return TRI_ERROR_LOCK_TIMEOUT;
    }
  }
  
  try { 
    // when we are here, we've got the read lock
    _vocbase->_deadlockDetector.addReader(this, wasBlocked);
  }
  catch (...) {
    TRI_READ_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this);
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write locks a collection, with a timeout
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::beginWriteTimed(uint64_t timeout,
                                               uint64_t sleepPeriod) {
  if (arangodb::Transaction::_makeNolockHeaders != nullptr) {
    std::string collName(_info.name());
    auto it = arangodb::Transaction::_makeNolockHeaders->find(collName);
    if (it != arangodb::Transaction::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "BeginWriteTimed blocked: " << document->_info._name <<
      // std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }
  uint64_t waited = 0;
  if (timeout == 0) {
    // we don't allow looping forever. limit waiting to 15 minutes max.
    timeout = 15 * 60 * 1000 * 1000;
  }

  // LOCKING-DEBUG
  // std::cout << "BeginWriteTimed: " << document->_info._name << std::endl;
  int iterations = 0;
  bool wasBlocked = false;

  while (!TRI_TRY_WRITE_LOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this)) {
    try {
      if (!wasBlocked) {
        // insert writer
        wasBlocked = true;
        if (_vocbase->_deadlockDetector.setWriterBlocked(this) == TRI_ERROR_DEADLOCK) {
          // deadlock
          LOG(TRACE) << "deadlock detected while trying to acquire write-lock on collection '" << _info.namec_str() << "'";
          return TRI_ERROR_DEADLOCK;
        }
        LOG(TRACE) << "waiting for write-lock on collection '" << _info.namec_str() << "'";
      } else if (++iterations >= 5) {
        // periodically check for deadlocks
        TRI_ASSERT(wasBlocked);
        iterations = 0;
        if (_vocbase->_deadlockDetector.detectDeadlock(this, true) == TRI_ERROR_DEADLOCK) {
          // deadlock
          _vocbase->_deadlockDetector.unsetWriterBlocked(this);
          LOG(TRACE) << "deadlock detected while trying to acquire write-lock on collection '" << _info.namec_str() << "'";
          return TRI_ERROR_DEADLOCK;
        }
      }
    } catch (...) {
      // clean up!
      if (wasBlocked) {
        _vocbase->_deadlockDetector.unsetWriterBlocked(this);
      }
      // always exit
      return TRI_ERROR_OUT_OF_MEMORY;
    }

#ifdef _WIN32
    usleep((unsigned long)sleepPeriod);
#else
    usleep((useconds_t)sleepPeriod);
#endif

    waited += sleepPeriod;

    if (waited > timeout) {
      _vocbase->_deadlockDetector.unsetWriterBlocked(this);
      LOG(TRACE) << "timed out waiting for write-lock on collection '" << _info.namec_str() << "'";
      return TRI_ERROR_LOCK_TIMEOUT;
    }
  }

  try { 
    // register writer 
    _vocbase->_deadlockDetector.addWriter(this, wasBlocked);
  }
  catch (...) {
    TRI_WRITE_UNLOCK_DOCUMENTS_INDEXES_PRIMARY_COLLECTION(this);
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the number of documents in collection
///
/// the caller must have read-locked the collection!
////////////////////////////////////////////////////////////////////////////////

uint64_t TRI_document_collection_t::size() {
  return static_cast<uint64_t>(_numberDocuments);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns information about the collection
/// note: the collection lock must be held when calling this function
////////////////////////////////////////////////////////////////////////////////

TRI_doc_collection_info_t* TRI_document_collection_t::figures() {
  // prefill with 0's to init counters
  TRI_doc_collection_info_t* info =
      static_cast<TRI_doc_collection_info_t*>(TRI_Allocate(
          TRI_UNKNOWN_MEM_ZONE, sizeof(TRI_doc_collection_info_t), true));

  if (info == nullptr) {
    return nullptr;
  }

  DatafileStatisticsContainer dfi = _datafileStatistics.all();
  info->_numberAlive += static_cast<TRI_voc_ssize_t>(dfi.numberAlive);
  info->_numberDead += static_cast<TRI_voc_ssize_t>(dfi.numberDead);
  info->_numberDeletions += static_cast<TRI_voc_ssize_t>(dfi.numberDeletions);
  info->_numberShapes += static_cast<TRI_voc_ssize_t>(dfi.numberShapes);
  info->_numberAttributes += static_cast<TRI_voc_ssize_t>(dfi.numberAttributes);

  info->_sizeAlive += dfi.sizeAlive;
  info->_sizeDead += dfi.sizeDead;
  info->_sizeShapes += dfi.sizeShapes;
  info->_sizeAttributes += dfi.sizeAttributes;

  // add the file sizes for datafiles and journals
  TRI_collection_t* base = this;

  for (auto& df : base->_datafiles) {
    info->_datafileSize += (int64_t)df->_initSize;
    ++info->_numberDatafiles;
  }

  for (auto& df : base->_journals) {
    info->_journalfileSize += (int64_t)df->_initSize;
    ++info->_numberJournalfiles;
  }

  for (auto& df : base->_compactors) {
    info->_compactorfileSize += (int64_t)df->_initSize;
    ++info->_numberCompactorfiles;
  }

  // add index information
  info->_numberIndexes = 0;
  info->_sizeIndexes = 0;

  info->_sizeIndexes += static_cast<int64_t>(_masterPointers.memory());

  for (auto& idx : allIndexes()) {
    info->_sizeIndexes += idx->memory();
    info->_numberIndexes++;
  }

  // get information about shape files (DEPRECATED, thus hard-coded to 0)
  info->_shapefileSize = 0;
  info->_numberShapefiles = 0;

  info->_uncollectedLogfileEntries = _uncollectedLogfileEntries;
  info->_tickMax = _tickMax;

  info->_numberDocumentDitches = _ditches.numDocumentDitches();
  info->_waitingForDitch = _ditches.head();

  // fills in compaction status
  getCompactionStatus(info->_lastCompactionStatus,
                      &info->_lastCompactionStamp[0],
                      sizeof(info->_lastCompactionStamp));

  return info;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add an index to the collection
/// note: this may throw. it's the caller's responsibility to catch and clean up
////////////////////////////////////////////////////////////////////////////////

void TRI_document_collection_t::addIndex(arangodb::Index* idx) {
  _indexes.emplace_back(idx);

  if (idx->type() == arangodb::Index::TRI_IDX_TYPE_FULLTEXT_INDEX) {
    ++_cleanupIndexes;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get an index by id
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_document_collection_t::removeIndex(TRI_idx_iid_t iid) {
  size_t const n = _indexes.size();

  for (size_t i = 0; i < n; ++i) {
    auto idx = _indexes[i];

    if (idx->type() == arangodb::Index::TRI_IDX_TYPE_PRIMARY_INDEX ||
        idx->type() == arangodb::Index::TRI_IDX_TYPE_EDGE_INDEX) {
      continue;
    }

    if (idx->id() == iid) {
      // found!
      _indexes.erase(_indexes.begin() + i);

      if (idx->type() == arangodb::Index::TRI_IDX_TYPE_FULLTEXT_INDEX) {
        --_cleanupIndexes;
      }

      return idx;
    }
  }

  // not found
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get all indexes of the collection
////////////////////////////////////////////////////////////////////////////////

std::vector<arangodb::Index*> TRI_document_collection_t::allIndexes() const {
  return _indexes;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the primary index
////////////////////////////////////////////////////////////////////////////////

arangodb::PrimaryIndex* TRI_document_collection_t::primaryIndex() {
  TRI_ASSERT(!_indexes.empty());
  // the primary index must be the index at position #0
  return static_cast<arangodb::PrimaryIndex*>(_indexes[0]);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection's edge index, if it exists
////////////////////////////////////////////////////////////////////////////////

arangodb::EdgeIndex* TRI_document_collection_t::edgeIndex() {
  if (_indexes.size() >= 2 &&
      _indexes[1]->type() == arangodb::Index::TRI_IDX_TYPE_EDGE_INDEX) {
    // edge index must be the index at position #1
    return static_cast<arangodb::EdgeIndex*>(_indexes[1]);
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get an index by id
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_document_collection_t::lookupIndex(
    TRI_idx_iid_t iid) const {
  for (auto const& it : _indexes) {
    if (it->id() == iid) {
      return it;
    }
  }
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return a pointer to the shaper
////////////////////////////////////////////////////////////////////////////////

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
VocShaper* TRI_document_collection_t::getShaper() const {
  if (!_ditches.contains(arangodb::Ditch::TRI_DITCH_DOCUMENT)) {
  }
  return _shaper;
}
#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief add a WAL operation for a transaction collection
////////////////////////////////////////////////////////////////////////////////

int TRI_AddOperationTransaction(TRI_transaction_t*,
                                arangodb::wal::DocumentOperation&, bool&);

static int FillIndex(arangodb::Transaction*, TRI_document_collection_t*,
                     arangodb::Index*);

static int GeoIndexFromVelocyPack(arangodb::Transaction*,
                                  TRI_document_collection_t*, VPackSlice const&,
                                  TRI_idx_iid_t, arangodb::Index**);

static int HashIndexFromVelocyPack(arangodb::Transaction*,
                                   TRI_document_collection_t*,
                                   VPackSlice const&, TRI_idx_iid_t,
                                   arangodb::Index**);

static int SkiplistIndexFromVelocyPack(arangodb::Transaction*,
                                       TRI_document_collection_t*,
                                       VPackSlice const&, TRI_idx_iid_t,
                                       arangodb::Index**);

static int FulltextIndexFromVelocyPack(arangodb::Transaction*,
                                       TRI_document_collection_t*,
                                       VPackSlice const&, TRI_idx_iid_t,
                                       arangodb::Index**);

////////////////////////////////////////////////////////////////////////////////
/// @brief set the collection tick with the marker's tick value
////////////////////////////////////////////////////////////////////////////////

static inline void SetRevision(TRI_document_collection_t* document,
                               TRI_voc_rid_t rid, bool force) {
  document->_info.setRevision(rid, force);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that an error code is set in all required places
////////////////////////////////////////////////////////////////////////////////

static void EnsureErrorCode(int code) {
  if (code == TRI_ERROR_NO_ERROR) {
    // must have an error code
    code = TRI_ERROR_INTERNAL;
  }

  TRI_set_errno(code);
  errno = code;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief removes an index file
////////////////////////////////////////////////////////////////////////////////

static bool RemoveIndexFile(TRI_document_collection_t* collection,
                            TRI_idx_iid_t id) {
  // construct filename
  std::string name("index-" + std::to_string(id) + ".json");
  std::string filename = arangodb::basics::FileUtils::buildFilename(collection->_directory, name);

  int res = TRI_UnlinkFile(filename.c_str());

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(ERR) << "cannot remove index definition: " << TRI_last_error();
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief garbage-collect a collection's indexes
////////////////////////////////////////////////////////////////////////////////

static int CleanupIndexes(TRI_document_collection_t* document) {
  int res = TRI_ERROR_NO_ERROR;

  // cleaning indexes is expensive, so only do it if the flag is set for the
  // collection
  if (document->_cleanupIndexes > 0) {
    WRITE_LOCKER(writeLocker, document->_lock);

    for (auto& idx : document->allIndexes()) {
      if (idx->type() == arangodb::Index::TRI_IDX_TYPE_FULLTEXT_INDEX) {
        res = idx->cleanup();

        if (res != TRI_ERROR_NO_ERROR) {
          break;
        }
      }
    }
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief state during opening of a collection
////////////////////////////////////////////////////////////////////////////////

struct open_iterator_state_t {
  TRI_document_collection_t* _document;
  TRI_voc_tid_t _tid;
  TRI_voc_fid_t _fid;
  std::unordered_map<TRI_voc_fid_t, DatafileStatisticsContainer*> _stats;
  DatafileStatisticsContainer* _dfi;
  TRI_vocbase_t* _vocbase;
  arangodb::Transaction* _trx;
  uint64_t _deletions;
  uint64_t _documents;
  int64_t _initialCount;

  open_iterator_state_t(TRI_document_collection_t* document,
                        TRI_vocbase_t* vocbase)
      : _document(document),
        _tid(0),
        _fid(0),
        _stats(),
        _dfi(nullptr),
        _vocbase(vocbase),
        _trx(nullptr),
        _deletions(0),
        _documents(0),
        _initialCount(-1) {}

  ~open_iterator_state_t() {
    for (auto& it : _stats) {
      delete it.second;
    }
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief find a statistics container for a given file id
////////////////////////////////////////////////////////////////////////////////

static DatafileStatisticsContainer* FindDatafileStats(
    open_iterator_state_t* state, TRI_voc_fid_t fid) {
  auto it = state->_stats.find(fid);

  if (it != state->_stats.end()) {
    return (*it).second;
  }

  auto stats = std::make_unique<DatafileStatisticsContainer>();

  state->_stats.emplace(fid, stats.get());
  auto p = stats.release();

  return p;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief process a document (or edge) marker when opening a collection
////////////////////////////////////////////////////////////////////////////////

static int OpenIteratorHandleDocumentMarker(TRI_df_marker_t const* marker,
                                            TRI_datafile_t* datafile,
                                            open_iterator_state_t* state) {
  auto const fid = datafile->_fid;
  TRI_document_collection_t* document = state->_document;
  arangodb::Transaction* trx = state->_trx;

  VPackSlice const slice(reinterpret_cast<char const*>(marker) + DatafileHelper::VPackOffset(TRI_DF_MARKER_VPACK_DOCUMENT));
  VPackSlice const keySlice = slice.get(TRI_VOC_ATTRIBUTE_KEY);
  std::string const key(keySlice.copyString());
  TRI_voc_rid_t const rid = std::stoull(slice.get(TRI_VOC_ATTRIBUTE_REV).copyString());
 
  SetRevision(document, rid, false);
  document->_keyGenerator->track(key);

  ++state->_documents;
 
  if (state->_fid != fid) {
    // update the state
    state->_fid = fid; // when we're here, we're looking at a datafile
    state->_dfi = FindDatafileStats(state, fid);
  }

  auto primaryIndex = document->primaryIndex();

  // no primary index lock required here because we are the only ones reading
  // from the index ATM
  auto found = primaryIndex->lookupKey(trx, keySlice);

  // it is a new entry
  if (found == nullptr) {
    TRI_doc_mptr_t* header = document->_masterPointers.request();

    if (header == nullptr) {
      return TRI_ERROR_OUT_OF_MEMORY;
    }

    header->setFid(fid, false);
    header->setHash(primaryIndex->calculateHash(trx, keySlice));
    header->setDataPtr(marker);  // ONLY IN OPENITERATOR

    // insert into primary index
    void const* result = nullptr;
    int res = primaryIndex->insertKey(trx, header, &result);

    if (res != TRI_ERROR_NO_ERROR) {
      document->_masterPointers.release(header);
      LOG(ERR) << "inserting document into primary index failed with error: " << TRI_errno_string(res);

      return res;
    }

    ++document->_numberDocuments;

    // update the datafile info
    state->_dfi->numberAlive++;
    state->_dfi->sizeAlive += DatafileHelper::AlignedMarkerSize<int64_t>(marker);
  }

  // it is an update, but only if found has a smaller revision identifier
  else if (found->revisionId() < rid ||
           (found->revisionId() == rid && found->getFid() <= fid)) {
    // save the old data
    TRI_doc_mptr_t oldData = *found;

    // update the header info
    found->setFid(fid, false); // when we're here, we're looking at a datafile
    found->setDataPtr(marker);

    // update the datafile info
    DatafileStatisticsContainer* dfi;
    if (oldData.getFid() == state->_fid) {
      dfi = state->_dfi;
    } else {
      dfi = FindDatafileStats(state, oldData.getFid());
    }

    if (oldData.getDataPtr() != nullptr) { 
      int64_t size = static_cast<int64_t>(oldData.getMarkerPtr()->getSize());

      dfi->numberAlive--;
      dfi->sizeAlive -= DatafileHelper::AlignedSize<int64_t>(size);
      dfi->numberDead++;
      dfi->sizeDead += DatafileHelper::AlignedSize<int64_t>(size);
    }

    state->_dfi->numberAlive++;
    state->_dfi->sizeAlive += DatafileHelper::AlignedMarkerSize<int64_t>(marker);
  }

  // it is a stale update
  else {
    TRI_ASSERT(found->getDataPtr() != nullptr);

    state->_dfi->numberDead++;
    state->_dfi->sizeDead += DatafileHelper::AlignedSize<int64_t>(found->getMarkerPtr()->getSize());
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief process a deletion marker when opening a collection
////////////////////////////////////////////////////////////////////////////////

static int OpenIteratorHandleDeletionMarker(TRI_df_marker_t const* marker,
                                            TRI_datafile_t* datafile,
                                            open_iterator_state_t* state) {
  TRI_document_collection_t* document = state->_document;
  arangodb::Transaction* trx = state->_trx;

  VPackSlice const slice(reinterpret_cast<char const*>(marker) + DatafileHelper::VPackOffset(TRI_DF_MARKER_VPACK_REMOVE));
  VPackSlice const keySlice = slice.get(TRI_VOC_ATTRIBUTE_KEY);
  std::string const key(keySlice.copyString());
  TRI_voc_rid_t const rid = std::stoull(slice.get(TRI_VOC_ATTRIBUTE_REV).copyString());
 
  document->setLastRevision(rid, false);
  document->_keyGenerator->track(key);

  ++state->_deletions;

  if (state->_fid != datafile->_fid) {
    // update the state
    state->_fid = datafile->_fid;
    state->_dfi = FindDatafileStats(state, datafile->_fid);
  }

  // no primary index lock required here because we are the only ones reading
  // from the index ATM
  auto primaryIndex = document->primaryIndex();
  TRI_doc_mptr_t* found = primaryIndex->lookupKey(trx, keySlice);

  // it is a new entry, so we missed the create
  if (found == nullptr) {
    // update the datafile info
    state->_dfi->numberDeletions++;
  }

  // it is a real delete
  else {
    // update the datafile info
    DatafileStatisticsContainer* dfi;

    if (found->getFid() == state->_fid) {
      dfi = state->_dfi;
    } else {
      dfi = FindDatafileStats(state, found->getFid());
    }

    TRI_ASSERT(found->getDataPtr() != nullptr);

    int64_t size = DatafileHelper::AlignedSize<int64_t>(found->getMarkerPtr()->getSize());

    dfi->numberAlive--;
    dfi->sizeAlive -= DatafileHelper::AlignedSize<int64_t>(size);
    dfi->numberDead++;
    dfi->sizeDead += DatafileHelper::AlignedSize<int64_t>(size);
    state->_dfi->numberDeletions++;

    document->deletePrimaryIndex(trx, found);
    --document->_numberDocuments;

    // free the header
    document->_masterPointers.release(found);
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief iterator for open
////////////////////////////////////////////////////////////////////////////////

static bool OpenIterator(TRI_df_marker_t const* marker, void* data,
                         TRI_datafile_t* datafile) {
  TRI_document_collection_t* document =
      static_cast<open_iterator_state_t*>(data)->_document;
  TRI_voc_tick_t const tick = marker->getTick();
  TRI_df_marker_type_t const type = marker->getType();

  int res;

  if (type == TRI_DF_MARKER_VPACK_DOCUMENT) {
    res = OpenIteratorHandleDocumentMarker(marker, datafile,
                                           static_cast<open_iterator_state_t*>(data));

    if (datafile->_dataMin == 0) {
      datafile->_dataMin = tick;
    }

    if (tick > datafile->_dataMax) {
      datafile->_dataMax = tick;
    }
  } else if (type == TRI_DF_MARKER_VPACK_REMOVE) {
    res = OpenIteratorHandleDeletionMarker(marker, datafile,
                                           static_cast<open_iterator_state_t*>(data));
  } else {
    if (type == TRI_DF_MARKER_HEADER) {
      // ensure there is a datafile info entry for each datafile of the
      // collection
      FindDatafileStats(static_cast<open_iterator_state_t*>(data), datafile->_fid);
    }

    LOG(TRACE) << "skipping marker type " << TRI_NameMarkerDatafile(marker);
    res = TRI_ERROR_NO_ERROR;
  }

  if (datafile->_tickMin == 0) {
    datafile->_tickMin = tick;
  }

  if (tick > datafile->_tickMax) {
    datafile->_tickMax = tick;
  }

  if (tick > document->_tickMax) {
    if (type != TRI_DF_MARKER_HEADER &&
        type != TRI_DF_MARKER_FOOTER &&
        type != TRI_DF_MARKER_COL_HEADER &&
        type != TRI_DF_MARKER_PROLOGUE) {
      document->_tickMax = tick;
    }
  }

  return (res == TRI_ERROR_NO_ERROR);
}

struct OpenIndexIteratorContext {
  arangodb::Transaction* trx;
  TRI_document_collection_t* collection;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief iterator for index open
////////////////////////////////////////////////////////////////////////////////

static bool OpenIndexIterator(char const* filename, void* data) {
  // load VelocyPack description of the index
  std::shared_ptr<VPackBuilder> builder;
  try {
    builder = arangodb::basics::VelocyPackHelper::velocyPackFromFile(filename);
  } catch (...) {
    // Failed to parse file
    LOG(ERR) << "failed to parse index definition from '" << filename << "'";
    return false;
  }

  VPackSlice description = builder->slice();
  // VelocyPack must be a index description
  if (!description.isObject()) {
    LOG(ERR) << "cannot read index definition from '" << filename << "'";
    return false;
  }

  auto ctx = static_cast<OpenIndexIteratorContext*>(data);
  arangodb::Transaction* trx = ctx->trx;
  TRI_document_collection_t* collection = ctx->collection;

  int res = TRI_FromVelocyPackIndexDocumentCollection(trx, collection,
                                                      description, nullptr);

  if (res != TRI_ERROR_NO_ERROR) {
    // error was already printed if we get here
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes a document collection
////////////////////////////////////////////////////////////////////////////////

static int InitBaseDocumentCollection(TRI_document_collection_t* document,
                                      VocShaper* shaper) {
  TRI_ASSERT(document != nullptr);

  document->setShaper(shaper);
  document->_numberDocuments = 0;
  document->_lastCompaction = 0.0;

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a primary collection
////////////////////////////////////////////////////////////////////////////////

static void DestroyBaseDocumentCollection(TRI_document_collection_t* document) {
  if (document->_keyGenerator != nullptr) {
    delete document->_keyGenerator;
    document->_keyGenerator = nullptr;
  }

  if (document->getShaper() != nullptr) {  // PROTECTED by trx here
    delete document->getShaper();          // PROTECTED by trx here
  }

  document->ditches()->destroy();
  TRI_DestroyCollection(document);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes a document collection
////////////////////////////////////////////////////////////////////////////////

static bool InitDocumentCollection(TRI_document_collection_t* document,
                                   VocShaper* shaper) {
  TRI_ASSERT(document != nullptr);

  document->_cleanupIndexes = false;

  document->_uncollectedLogfileEntries.store(0);

  int res = InitBaseDocumentCollection(document, shaper);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_DestroyCollection(document);
    TRI_set_errno(res);

    return false;
  }

  // create primary index
  std::unique_ptr<arangodb::Index> primaryIndex(
      new arangodb::PrimaryIndex(document));

  try {
    document->addIndex(primaryIndex.get());
    primaryIndex.release();
  } catch (...) {
    DestroyBaseDocumentCollection(document);
    TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);

    return false;
  }

  // create edges index
  if (document->_info.type() == TRI_COL_TYPE_EDGE) {
    TRI_idx_iid_t iid = document->_info.id();
    if (document->_info.planId() > 0) {
      iid = document->_info.planId();
    }

    try {
      std::unique_ptr<arangodb::Index> edgeIndex(
          new arangodb::EdgeIndex(iid, document));

      document->addIndex(edgeIndex.get());
      edgeIndex.release();
    } catch (...) {
      DestroyBaseDocumentCollection(document);
      TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);

      return false;
    }
  }

  TRI_InitCondition(&document->_journalsCondition);

  // crud methods
  document->cleanupIndexes = CleanupIndexes;

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief iterate all markers of the collection
////////////////////////////////////////////////////////////////////////////////

static int IterateMarkersCollection(arangodb::Transaction* trx,
                                    TRI_collection_t* collection) {
  auto document = reinterpret_cast<TRI_document_collection_t*>(collection);

  // initialize state for iteration
  open_iterator_state_t openState(document, collection->_vocbase);

  if (collection->_info.initialCount() != -1) {
    auto primaryIndex = document->primaryIndex();

    int res = primaryIndex->resize(
        trx, static_cast<size_t>(collection->_info.initialCount() * 1.1));

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }

    openState._initialCount = collection->_info.initialCount();
  }

  // read all documents and fill primary index
  TRI_IterateCollection(collection, OpenIterator, &openState);

  LOG(TRACE) << "found " << openState._documents << " document markers, " << openState._deletions << " deletion markers for collection '" << collection->_info.namec_str() << "'";

  // update the real statistics for the collection
  try {
    for (auto& it : openState._stats) {
      document->_datafileStatistics.create(it.first, *(it.second));
    }
  } catch (basics::Exception const& ex) {
    return ex.code();
  } catch (...) {
    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new collection
////////////////////////////////////////////////////////////////////////////////

TRI_document_collection_t* TRI_CreateDocumentCollection(
    TRI_vocbase_t* vocbase, char const* path, VocbaseCollectionInfo& parameters,
    TRI_voc_cid_t cid) {
  if (cid > 0) {
    TRI_UpdateTickServer(cid);
  } else {
    cid = TRI_NewTickServer();
  }

  parameters.setCollectionId(cid);

  // check if we can generate the key generator
  std::shared_ptr<arangodb::velocypack::Buffer<uint8_t> const> buffer =
      parameters.keyOptions();

  VPackSlice slice;
  if (buffer != nullptr) {
    slice = VPackSlice(buffer->data());
  }
  KeyGenerator* keyGenerator = KeyGenerator::factory(slice);

  if (keyGenerator == nullptr) {
    TRI_set_errno(TRI_ERROR_ARANGO_INVALID_KEY_GENERATOR);
    return nullptr;
  }

  // first create the document collection
  TRI_document_collection_t* document;
  try {
    document = new TRI_document_collection_t();
  } catch (std::exception&) {
    document = nullptr;
  }

  if (document == nullptr) {
    delete keyGenerator;
    LOG(WARN) << "cannot create document collection";
    TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);

    return nullptr;
  }

  TRI_ASSERT(document != nullptr);

  document->_keyGenerator = keyGenerator;

  TRI_collection_t* collection =
      TRI_CreateCollection(vocbase, document, path, parameters);

  if (collection == nullptr) {
    delete document;
    LOG(ERR) << "cannot create document collection";

    return nullptr;
  }

  auto shaper = new VocShaper(TRI_UNKNOWN_MEM_ZONE, document);

  // create document collection and shaper
  if (false == InitDocumentCollection(document, shaper)) {
    LOG(ERR) << "cannot initialize document collection";

    TRI_CloseCollection(collection);
    TRI_DestroyCollection(collection);
    delete document;
    return nullptr;
  }

  document->_keyGenerator = keyGenerator;

  // save the parameters block (within create, no need to lock)
  bool doSync = vocbase->_settings.forceSyncProperties;
  int res = parameters.saveToFile(collection->_directory, doSync);

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(ERR) << "cannot save collection parameters in directory '" << collection->_directory << "': '" << TRI_last_error() << "'";

    TRI_CloseCollection(collection);
    TRI_DestroyCollection(collection);
    delete document;
    return nullptr;
  }

  // remove the temporary file
  std::string tmpfile = collection->_directory + ".tmp";
  TRI_UnlinkFile(tmpfile.c_str());

  return document;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief frees the memory allocated, but does not free the pointer
///
/// Note that the collection must be closed first.
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroyDocumentCollection(TRI_document_collection_t* document) {
  TRI_DestroyCondition(&document->_journalsCondition);

  // free memory allocated for indexes
  for (auto& idx : document->allIndexes()) {
    delete idx;
  }

  DestroyBaseDocumentCollection(document);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief frees the memory allocated and frees the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeDocumentCollection(TRI_document_collection_t* document) {
  TRI_DestroyDocumentCollection(document);
  delete document;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a journal
///
/// Note that the caller must hold a lock protecting the _journals entry.
////////////////////////////////////////////////////////////////////////////////

TRI_datafile_t* TRI_CreateDatafileDocumentCollection(
    TRI_document_collection_t* document, TRI_voc_fid_t fid,
    TRI_voc_size_t journalSize, bool isCompactor) {
  TRI_ASSERT(fid > 0);

  // create a datafile entry for the new journal
  try {
    document->_datafileStatistics.create(fid);
  } catch (...) {
    EnsureErrorCode(TRI_ERROR_OUT_OF_MEMORY);
    return nullptr;
  }

  TRI_datafile_t* journal;

  if (document->_info.isVolatile()) {
    // in-memory collection
    journal = TRI_CreateDatafile(nullptr, fid, journalSize, true);
  } else {
    // construct a suitable filename (which may be temporary at the beginning)
    std::string jname;
    if (isCompactor) {
      jname = "compaction-";
    } else {
      jname = "temp-";
    }

    jname.append(std::to_string(fid) + ".db");
    std::string filename = arangodb::basics::FileUtils::buildFilename(document->_directory, jname);

    TRI_IF_FAILURE("CreateJournalDocumentCollection") {
      // simulate disk full
      document->_lastError = TRI_set_errno(TRI_ERROR_ARANGO_FILESYSTEM_FULL);

      EnsureErrorCode(TRI_ERROR_ARANGO_FILESYSTEM_FULL);

      return nullptr;
    }

    // remove an existing temporary file first
    if (TRI_ExistsFile(filename.c_str())) {
      // remove an existing file first
      TRI_UnlinkFile(filename.c_str());
    }

    journal = TRI_CreateDatafile(filename.c_str(), fid, journalSize, true);
  }

  if (journal == nullptr) {
    if (TRI_errno() == TRI_ERROR_OUT_OF_MEMORY_MMAP) {
      document->_lastError = TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY_MMAP);
    } else {
      document->_lastError = TRI_set_errno(TRI_ERROR_ARANGO_NO_JOURNAL);
    }

    EnsureErrorCode(document->_lastError);

    return nullptr;
  }

  // journal is there now
  TRI_ASSERT(journal != nullptr);

  if (isCompactor) {
    LOG(TRACE) << "created new compactor '" << journal->getName(journal) << "'";
  } else {
    LOG(TRACE) << "created new journal '" << journal->getName(journal) << "'";
  }

  // create a collection header, still in the temporary file
  TRI_df_marker_t* position;
  int res = TRI_ReserveElementDatafile(journal, sizeof(TRI_col_header_marker_t),
                                       &position, journalSize);

  TRI_IF_FAILURE("CreateJournalDocumentCollectionReserve1") {
    res = TRI_ERROR_DEBUG;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    document->_lastError = journal->_lastError;
    LOG(ERR) << "cannot create collection header in file '" << journal->getName(journal) << "': " << TRI_errno_string(res);

    // close the journal and remove it
    TRI_CloseDatafile(journal);
    TRI_UnlinkFile(journal->getName(journal));
    TRI_FreeDatafile(journal);

    EnsureErrorCode(res);

    return nullptr;
  }

  TRI_col_header_marker_t cm;
  DatafileHelper::InitMarker(reinterpret_cast<TRI_df_marker_t*>(&cm), TRI_DF_MARKER_COL_HEADER,
                         sizeof(TRI_col_header_marker_t), static_cast<TRI_voc_tick_t>(fid));
  cm._type = static_cast<TRI_col_type_t>(document->_info.type());
  cm._cid = document->_info.id();

  res = TRI_WriteCrcElementDatafile(journal, position, &cm.base, false);

  TRI_IF_FAILURE("CreateJournalDocumentCollectionReserve2") {
    res = TRI_ERROR_DEBUG;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    document->_lastError = journal->_lastError;
    LOG(ERR) << "cannot create collection header in file '" << journal->getName(journal) << "': " << TRI_last_error();

    // close the journal and remove it
    TRI_CloseDatafile(journal);
    TRI_UnlinkFile(journal->getName(journal));
    TRI_FreeDatafile(journal);

    EnsureErrorCode(document->_lastError);

    return nullptr;
  }

  TRI_ASSERT(fid == journal->_fid);

  // if a physical file, we can rename it from the temporary name to the correct
  // name
  if (!isCompactor) {
    if (journal->isPhysical(journal)) {
      // and use the correct name
      std::string jname("journal-" + std::to_string(journal->_fid) + ".db");
      std::string filename = arangodb::basics::FileUtils::buildFilename(document->_directory, jname);

      bool ok = TRI_RenameDatafile(journal, filename.c_str());

      if (!ok) {
        LOG(ERR) << "failed to rename journal '" << journal->getName(journal) << "' to '" << filename << "': " << TRI_last_error();

        TRI_CloseDatafile(journal);
        TRI_UnlinkFile(journal->getName(journal));
        TRI_FreeDatafile(journal);

        EnsureErrorCode(document->_lastError);

        return nullptr;
      } else {
        LOG(TRACE) << "renamed journal from '" << journal->getName(journal) << "' to '" << filename << "'";
      }
    }

    document->_journals.emplace_back(journal);
  }

  return journal;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create an index, based on a VelocyPack description
////////////////////////////////////////////////////////////////////////////////

int TRI_FromVelocyPackIndexDocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    VPackSlice const& slice, arangodb::Index** idx) {
  TRI_ASSERT(slice.isObject());

  if (idx != nullptr) {
    *idx = nullptr;
  }

  // extract the type
  VPackSlice type = slice.get("type");

  if (!type.isString()) {
    return TRI_ERROR_INTERNAL;
  }
  std::string typeStr = type.copyString();

  // extract the index identifier
  VPackSlice iis = slice.get("id");

  TRI_idx_iid_t iid;
  if (iis.isNumber()) {
    iid = iis.getNumericValue<TRI_idx_iid_t>();
  } else if (iis.isString()) {
    std::string tmp = iis.copyString();
    iid = static_cast<TRI_idx_iid_t>(StringUtils::uint64(tmp));
  } else {
    LOG(ERR) << "ignoring index, index identifier could not be located";

    return TRI_ERROR_INTERNAL;
  }

  TRI_UpdateTickServer(iid);

  // ...........................................................................
  // GEO INDEX (list or attribute)
  // ...........................................................................
  if (typeStr == "geo1" || typeStr == "geo2") {
    return GeoIndexFromVelocyPack(trx, document, slice, iid, idx);
  }

  // ...........................................................................
  // HASH INDEX
  // ...........................................................................
  if (typeStr == "hash") {
    return HashIndexFromVelocyPack(trx, document, slice, iid, idx);
  }

  // ...........................................................................
  // SKIPLIST INDEX
  // ...........................................................................
  if (typeStr == "skiplist") {
    return SkiplistIndexFromVelocyPack(trx, document, slice, iid, idx);
  }

  // ...........................................................................
  // FULLTEXT INDEX
  // ...........................................................................
  if (typeStr == "fulltext") {
    return FulltextIndexFromVelocyPack(trx, document, slice, iid, idx);
  }

  // ...........................................................................
  // EDGES INDEX
  // ...........................................................................
  if (typeStr == "edge") {
    // we should never get here, as users cannot create their own edge indexes
    LOG(ERR) << "logic error. there should never be a JSON file describing an edges index";
    return TRI_ERROR_INTERNAL;
  }

  // default:
  LOG(WARN) << "index type '" << typeStr << "' is not supported in this version of ArangoDB and is ignored";

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief closes an existing datafile
/// Note that the caller must hold a lock protecting the _datafiles and
/// _journals entry.
////////////////////////////////////////////////////////////////////////////////

bool TRI_CloseDatafileDocumentCollection(TRI_document_collection_t* document,
                                         size_t position, bool isCompactor) {
  std::vector<TRI_datafile_t*>* vector;

  // either use a journal or a compactor
  if (isCompactor) {
    vector = &document->_compactors;
  } else {
    vector = &document->_journals;
  }

  // no journal at this position
  if (vector->size() <= position) {
    TRI_set_errno(TRI_ERROR_ARANGO_NO_JOURNAL);
    return false;
  }

  // seal and rename datafile
  TRI_datafile_t* journal =
      static_cast<TRI_datafile_t*>(vector->at(position));
  int res = TRI_SealDatafile(journal);

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(ERR) << "failed to seal datafile '" << journal->getName(journal) << "': " << TRI_last_error();

    if (!isCompactor) {
      vector->erase(vector->begin() + position);
      document->_datafiles.emplace_back(journal);
    }

    return false;
  }

  if (!isCompactor && journal->isPhysical(journal)) {
    // rename the file
    std::string dname("datafile-" + std::to_string(journal->_fid) + ".db");
    std::string filename = arangodb::basics::FileUtils::buildFilename(document->_directory, dname);

    bool ok = TRI_RenameDatafile(journal, filename.c_str());

    if (!ok) {
      LOG(ERR) << "failed to rename datafile '" << journal->getName(journal) << "' to '" << filename << "': " << TRI_last_error();

      vector->erase(vector->begin() + position);
      document->_datafiles.emplace_back(journal);

      return false;
    }

    LOG(TRACE) << "closed file '" << journal->getName(journal) << "'";
  }

  if (!isCompactor) {
    vector->erase(vector->begin() + position);
    document->_datafiles.emplace_back(journal);
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief helper struct for filling indexes
////////////////////////////////////////////////////////////////////////////////

class IndexFiller {
 public:
  IndexFiller(arangodb::Transaction* trx, TRI_document_collection_t* document,
              arangodb::Index* idx, std::function<void(int)> callback)
      : _trx(trx), _document(document), _idx(idx), _callback(callback) {}

  void operator()() {
    int res = TRI_ERROR_INTERNAL;

    try {
      res = FillIndex(_trx, _document, _idx);
    } catch (...) {
    }

    _callback(res);
  }

 private:
  arangodb::Transaction* _trx;
  TRI_document_collection_t* _document;
  arangodb::Index* _idx;
  std::function<void(int)> _callback;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief fill the additional (non-primary) indexes
////////////////////////////////////////////////////////////////////////////////

int TRI_FillIndexesDocumentCollection(arangodb::Transaction* trx,
                                      TRI_vocbase_col_t* collection,
                                      TRI_document_collection_t* document) {
  auto old = document->useSecondaryIndexes();

  // turn filling of secondary indexes off. we're now only interested in getting
  // the indexes' definition. we'll fill them below ourselves.
  document->useSecondaryIndexes(false);

  try {
    OpenIndexIteratorContext ctx;
    ctx.trx = trx;
    ctx.collection = document;

    TRI_IterateIndexCollection(reinterpret_cast<TRI_collection_t*>(document),
                               OpenIndexIterator, static_cast<void*>(&ctx));
    document->useSecondaryIndexes(old);
  } catch (...) {
    document->useSecondaryIndexes(old);
    return TRI_ERROR_INTERNAL;
  }

  // distribute the work to index threads plus this thread
  auto const& indexes = document->allIndexes();
  size_t const n = indexes.size();

  if (n == 1) {
    return TRI_ERROR_NO_ERROR;
  }

  double start = TRI_microtime();

  // only log performance infos for indexes with more than this number of
  // entries
  static size_t const NotificationSizeThreshold = 131072;
  auto primaryIndex = document->primaryIndex();

  if (primaryIndex->size() > NotificationSizeThreshold) {
    LOG_TOPIC(TRACE, Logger::PERFORMANCE) <<
        "fill-indexes-document-collection { collection: " << document->_vocbase->_name << "/" << document->_info.name() << " }, indexes: " << (n - 1);
  }

  TRI_ASSERT(n > 1);

  std::atomic<int> result(TRI_ERROR_NO_ERROR);

  {
    arangodb::basics::Barrier barrier(n - 1);

    auto indexPool = document->_vocbase->_server->_indexPool;

    auto callback = [&barrier, &result](int res) -> void {
      // update the error code
      if (res != TRI_ERROR_NO_ERROR) {
        int expected = TRI_ERROR_NO_ERROR;
        result.compare_exchange_strong(expected, res,
                                       std::memory_order_acquire);
      }

      barrier.join();
    };

    // now actually fill the secondary indexes
    for (size_t i = 1; i < n; ++i) {
      auto idx = indexes[i];

      // index threads must come first, otherwise this thread will block the
      // loop and
      // prevent distribution to threads
      if (indexPool != nullptr && i != (n - 1)) {
        try {
          // move task into thread pool
          IndexFiller indexTask(trx, document, idx, callback);

          static_cast<arangodb::basics::ThreadPool*>(indexPool)
              ->enqueue(indexTask);
        } catch (...) {
          // set error code
          int expected = TRI_ERROR_NO_ERROR;
          result.compare_exchange_strong(expected, TRI_ERROR_INTERNAL,
                                         std::memory_order_acquire);

          barrier.join();
        }
      } else {
        // fill index in this thread
        int res;

        try {
          res = FillIndex(trx, document, idx);
        } catch (...) {
          res = TRI_ERROR_INTERNAL;
        }

        if (res != TRI_ERROR_NO_ERROR) {
          int expected = TRI_ERROR_NO_ERROR;
          result.compare_exchange_strong(expected, res,
                                         std::memory_order_acquire);
        }

        barrier.join();
      }
    }

    // barrier waits here until all threads have joined
  }

  LOG_TOPIC(TRACE, Logger::PERFORMANCE) << "[timer] " << Logger::DURATION(TRI_microtime() - start) << " s, fill-indexes-document-collection { collection: " << document->_vocbase->_name << "/" << document->_info.name() << " }, indexes: " << (n - 1);

  return result.load();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief opens an existing collection
////////////////////////////////////////////////////////////////////////////////

TRI_document_collection_t* TRI_OpenDocumentCollection(TRI_vocbase_t* vocbase,
                                                      TRI_vocbase_col_t* col,
                                                      bool ignoreErrors) {
  char const* path = col->pathc_str();

  // first open the document collection
  TRI_document_collection_t* document = nullptr;
  try {
    document = new TRI_document_collection_t();
  } catch (std::exception&) {
  }

  if (document == nullptr) {
    return nullptr;
  }

  TRI_ASSERT(document != nullptr);

  double start = TRI_microtime();
  LOG_TOPIC(TRACE, Logger::PERFORMANCE) <<
      "open-document-collection { collection: " << vocbase->_name << "/" << col->name() << " }";

  TRI_collection_t* collection =
      TRI_OpenCollection(vocbase, document, path, ignoreErrors);

  if (collection == nullptr) {
    delete document;
    LOG(ERR) << "cannot open document collection from path '" << path << "'";

    return nullptr;
  }

  auto shaper = new VocShaper(TRI_UNKNOWN_MEM_ZONE, document);

  // create document collection and shaper
  if (false == InitDocumentCollection(document, shaper)) {
    TRI_CloseCollection(collection);
    TRI_FreeCollection(collection);
    LOG(ERR) << "cannot initialize document collection";

    return nullptr;
  }

  // check if we can generate the key generator
  std::shared_ptr<arangodb::velocypack::Buffer<uint8_t> const> buffer =
      collection->_info.keyOptions();

  VPackSlice slice;
  if (buffer.get() != nullptr) {
    slice = VPackSlice(buffer->data());
  }

  KeyGenerator* keyGenerator = KeyGenerator::factory(slice);

  if (keyGenerator == nullptr) {
    TRI_CloseCollection(collection);
    TRI_FreeCollection(collection);
    TRI_set_errno(TRI_ERROR_ARANGO_INVALID_KEY_GENERATOR);

    return nullptr;
  }

  document->_keyGenerator = keyGenerator;

  arangodb::SingleCollectionTransaction trx(
      arangodb::StandaloneTransactionContext::Create(vocbase),
      document->_info.id(), TRI_TRANSACTION_WRITE);

  // build the primary index
  {
    double start = TRI_microtime();

    LOG_TOPIC(TRACE, Logger::PERFORMANCE) <<
        "iterate-markers { collection: " << vocbase->_name << "/" << document->_info.name() << " }";

    // iterate over all markers of the collection
    int res = IterateMarkersCollection(&trx, collection);

    LOG_TOPIC(TRACE, Logger::PERFORMANCE) << "[timer] " << Logger::DURATION(TRI_microtime() - start) << " s, iterate-markers { collection: " << vocbase->_name << "/" << document->_info.name() << " }";

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_CloseCollection(collection);
      TRI_FreeCollection(collection);

      LOG(ERR) << "cannot iterate data of document collection";
      TRI_set_errno(res);

      return nullptr;
    }
  }

  TRI_ASSERT(document->getShaper() !=
             nullptr);  // ONLY in OPENCOLLECTION, PROTECTED by fake trx here

  if (!arangodb::wal::LogfileManager::instance()->isInRecovery()) {
    TRI_FillIndexesDocumentCollection(&trx, col, document);
  }

  LOG_TOPIC(TRACE, Logger::PERFORMANCE) << "[timer] " << Logger::DURATION(TRI_microtime() - start) << " s, open-document-collection { collection: " << vocbase->_name << "/" << document->_info.name() << " }";

  return document;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief closes an open collection
////////////////////////////////////////////////////////////////////////////////

int TRI_CloseDocumentCollection(TRI_document_collection_t* document,
                                bool updateStats) {
  auto primaryIndex = document->primaryIndex();
  auto idxSize = primaryIndex->size();

  if (!document->_info.deleted() &&
      document->_info.initialCount() != static_cast<int64_t>(idxSize)) {
    document->_info.updateCount(idxSize);

    bool doSync = document->_vocbase->_settings.forceSyncProperties;
    // Ignore the error?
    document->_info.saveToFile(document->_directory, doSync);
  }

  // closes all open compactors, journals, datafiles
  int res = TRI_CloseCollection(document);

  delete document
      ->getShaper();  // ONLY IN CLOSECOLLECTION, PROTECTED by fake trx here
  document->setShaper(nullptr);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief converts extracts a field list from a VelocyPack object
///        Does not copy any data, caller has to make sure that data
///        in slice stays valid until this return value is destroyed.
////////////////////////////////////////////////////////////////////////////////

static VPackSlice ExtractFields(VPackSlice const& slice, TRI_idx_iid_t iid) {
  VPackSlice fld = slice.get("fields");
  if (!fld.isArray()) {
    LOG(ERR) << "ignoring index " << iid << ", 'fields' must be an array";
    THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
  }

  for (auto const& sub : VPackArrayIterator(fld)) {
    if (!sub.isString()) {
      LOG(ERR) << "ignoring index " << iid << ", 'fields' must be an array of attribute paths";
      THROW_ARANGO_EXCEPTION(TRI_ERROR_BAD_PARAMETER);
    }
  }
  return fld;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fill an index in batches
////////////////////////////////////////////////////////////////////////////////

static int FillIndexBatch(arangodb::Transaction* trx,
                          TRI_document_collection_t* document,
                          arangodb::Index* idx) {
  auto indexPool = document->_vocbase->_server->_indexPool;
  TRI_ASSERT(indexPool != nullptr);

  double start = TRI_microtime();

  LOG_TOPIC(TRACE, Logger::PERFORMANCE) <<
      "fill-index-batch { collection: " << document->_vocbase->_name << "/" << document->_info.name() << " }, " << 
      idx->context() << ", threads: " << indexPool->numThreads() << ", buckets: " << document->_info.indexBuckets();

  // give the index a size hint
  auto primaryIndex = document->primaryIndex();

  auto nrUsed = primaryIndex->size();

  idx->sizeHint(trx, nrUsed);

  // process documents a million at a time
  size_t blockSize = 1024 * 1024;

  if (nrUsed < blockSize) {
    blockSize = nrUsed;
  }
  if (blockSize == 0) {
    blockSize = 1;
  }

  int res = TRI_ERROR_NO_ERROR;

  std::vector<TRI_doc_mptr_t const*> documents;
  documents.reserve(blockSize);

  if (nrUsed > 0) {
    arangodb::basics::BucketPosition position;
    uint64_t total = 0;
    while (true) {
      TRI_doc_mptr_t const* mptr =
          primaryIndex->lookupSequential(trx, position, total);

      if (mptr == nullptr) {
        break;
      }

      documents.emplace_back(mptr);

      if (documents.size() == blockSize) {
        res = idx->batchInsert(trx, &documents, indexPool->numThreads());
        documents.clear();

        // some error occurred
        if (res != TRI_ERROR_NO_ERROR) {
          break;
        }
      }
    }
  }

  // process the remainder of the documents
  if (res == TRI_ERROR_NO_ERROR && !documents.empty()) {
    res = idx->batchInsert(trx, &documents, indexPool->numThreads());
  }

  LOG_TOPIC(TRACE, Logger::PERFORMANCE) << "[timer] " << Logger::DURATION(TRI_microtime() - start) << " s, fill-index-batch { collection: " << document->_vocbase->_name << "/" << document->_info.name() << " }, " << idx->context() << ", threads: " << indexPool->numThreads() << ", buckets: " << document->_info.indexBuckets();

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief fill an index sequentially
////////////////////////////////////////////////////////////////////////////////

static int FillIndexSequential(arangodb::Transaction* trx,
                               TRI_document_collection_t* document,
                               arangodb::Index* idx) {
  double start = TRI_microtime();

  LOG_TOPIC(TRACE, Logger::PERFORMANCE) << 
      "fill-index-sequential { collection: " << document->_vocbase->_name << "/" << document->_info.name() << " }, " <<
      idx->context() << ", buckets: " << document->_info.indexBuckets();

  // give the index a size hint
  auto primaryIndex = document->primaryIndex();
  size_t nrUsed = primaryIndex->size();

  idx->sizeHint(trx, nrUsed);

  if (nrUsed > 0) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    static int const LoopSize = 10000;
    int counter = 0;
    int loops = 0;
#endif

    arangodb::basics::BucketPosition position;
    uint64_t total = 0;

    while (true) {
      TRI_doc_mptr_t const* mptr =
          primaryIndex->lookupSequential(trx, position, total);

      if (mptr == nullptr) {
        break;
      }

      int res = idx->insert(trx, mptr, false);

      if (res != TRI_ERROR_NO_ERROR) {
        return res;
      }
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      if (++counter == LoopSize) {
        counter = 0;
        ++loops;
        LOG(TRACE) << "indexed " << (LoopSize * loops) << " documents of collection " << document->_info.id();
      }
#endif
    }
  }

  LOG_TOPIC(TRACE, Logger::PERFORMANCE) << "[timer] " << Logger::DURATION(TRI_microtime() - start) << " s, fill-index-sequential { collection: " << document->_vocbase->_name << "/" << document->_info.name() << " }, " << idx->context() << ", buckets: " << document->_info.indexBuckets();

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes an index with all existing documents
////////////////////////////////////////////////////////////////////////////////

static int FillIndex(arangodb::Transaction* trx,
                     TRI_document_collection_t* document,
                     arangodb::Index* idx) {
  if (!document->useSecondaryIndexes()) {
    return TRI_ERROR_NO_ERROR;
  }

  try {
    size_t nrUsed = document->primaryIndex()->size();
    auto indexPool = document->_vocbase->_server->_indexPool;

    int res;

    if (indexPool != nullptr && idx->hasBatchInsert() && nrUsed > 256 * 1024 &&
        document->_info.indexBuckets() > 1) {
      // use batch insert if there is an index pool,
      // the collection has more than one index bucket
      // and it contains a significant amount of documents
      res = FillIndexBatch(trx, document, idx);
    } else {
      res = FillIndexSequential(trx, document, idx);
    }

    return res;
  } catch (arangodb::basics::Exception const& ex) {
    return ex.code();
  } catch (std::bad_alloc&) {
    return TRI_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return TRI_ERROR_INTERNAL;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a path based, unique or non-unique index
////////////////////////////////////////////////////////////////////////////////

static arangodb::Index* LookupPathIndexDocumentCollection(
    TRI_document_collection_t* collection,
    std::vector<std::vector<arangodb::basics::AttributeName>> const& paths,
    arangodb::Index::IndexType type, int sparsity, bool unique,
    bool allowAnyAttributeOrder) {
  for (auto const& idx : collection->allIndexes()) {
    if (idx->type() != type) {
      continue;
    }

    // .........................................................................
    // Now perform checks which are specific to the type of index
    // .........................................................................

    switch (idx->type()) {
      case arangodb::Index::TRI_IDX_TYPE_HASH_INDEX: {
        auto hashIndex = static_cast<arangodb::HashIndex*>(idx);

        if (unique != hashIndex->unique() ||
            (sparsity != -1 && sparsity != (hashIndex->sparse() ? 1 : 0))) {
          continue;
        }
        break;
      }

      case arangodb::Index::TRI_IDX_TYPE_SKIPLIST_INDEX: {
        auto skiplistIndex = static_cast<arangodb::SkiplistIndex*>(idx);

        if (unique != skiplistIndex->unique() ||
            (sparsity != -1 && sparsity != (skiplistIndex->sparse() ? 1 : 0))) {
          continue;
        }
        break;
      }

      default: { continue; }
    }

    // .........................................................................
    // check that the number of paths (fields) in the index matches that
    // of the number of attributes
    // .........................................................................

    auto const& idxFields = idx->fields();
    size_t const n = idxFields.size();

    if (n != paths.size()) {
      continue;
    }

    // .........................................................................
    // go through all the attributes and see if they match
    // .........................................................................

    bool found = true;

    if (allowAnyAttributeOrder) {
      // any permutation of attributes is allowed
      for (size_t i = 0; i < n; ++i) {
        found = false;
        size_t fieldSize = idxFields[i].size();

        for (size_t j = 0; j < n; ++j) {
          if (fieldSize == paths[j].size()) {
            bool allEqual = true;
            for (size_t k = 0; k < fieldSize; ++k) {
              if (idxFields[j][k] != paths[j][k]) {
                allEqual = false;
                break;
              }
            }
            if (allEqual) {
              found = true;
              break;
            }
          }
        }

        if (!found) {
          break;
        }
      }
    } else {
      // attributes need to be present in a given order
      for (size_t i = 0; i < n; ++i) {
        size_t fieldSize = idxFields[i].size();
        if (fieldSize == paths[i].size()) {
          for (size_t k = 0; k < fieldSize; ++k) {
            if (idxFields[i][k] != paths[i][k]) {
              found = false;
              break;
            }
          }
          if (!found) {
            break;
          }
        } else {
          found = false;
          break;
        }
      }
    }

    // stop if we found a match
    if (found) {
      return idx;
    }
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores a path based index (template)
////////////////////////////////////////////////////////////////////////////////

static int PathBasedIndexFromVelocyPack(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    VPackSlice const& definition, TRI_idx_iid_t iid,
    arangodb::Index* (*creator)(arangodb::Transaction*,
                                TRI_document_collection_t*,
                                std::vector<std::string> const&, TRI_idx_iid_t,
                                bool, bool, bool&),
    arangodb::Index** dst) {
  if (dst != nullptr) {
    *dst = nullptr;
  }

  // extract fields
  VPackSlice fld;
  try {
    fld = ExtractFields(definition, iid);
  } catch (arangodb::basics::Exception const& e) {
    return TRI_set_errno(e.code());
  }
  VPackValueLength fieldCount = fld.length();

  // extract the list of fields
  if (fieldCount < 1) {
    LOG(ERR) << "ignoring index " << iid << ", need at least one attribute path";

    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }

  // determine if the index is unique or non-unique
  VPackSlice bv = definition.get("unique");

  if (!bv.isBoolean()) {
    LOG(ERR) << "ignoring index " << iid << ", could not determine if unique or non-unique";
    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }

  bool unique = bv.getBoolean();

  // determine sparsity
  bool sparse = false;

  bv = definition.get("sparse");

  if (bv.isBoolean()) {
    sparse = bv.getBoolean();
  } else {
    // no sparsity information given for index
    // now use pre-2.5 defaults: unique hash indexes were sparse, all other
    // indexes were non-sparse
    bool isHashIndex = false;
    VPackSlice typeSlice = definition.get("type");
    if (typeSlice.isString()) {
      isHashIndex = typeSlice.copyString() == "hash";
    }

    if (isHashIndex && unique) {
      sparse = true;
    }
  }

  // Initialize the vector in which we store the fields on which the hashing
  // will be based.
  std::vector<std::string> attributes;
  attributes.reserve(fieldCount);

  // find fields
  for (auto const& fieldStr : VPackArrayIterator(fld)) {
    attributes.emplace_back(fieldStr.copyString());
  }

  // create the index
  bool created;
  auto idx = creator(trx, document, attributes, iid, sparse, unique, created);

  if (dst != nullptr) {
    *dst = idx;
  }

  if (idx == nullptr) {
    LOG(ERR) << "cannot create index " << iid << " in collection '" << document->_info.namec_str() << "'";
    return TRI_errno();
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief saves an index
////////////////////////////////////////////////////////////////////////////////

int TRI_SaveIndex(TRI_document_collection_t* document, arangodb::Index* idx,
                  bool writeMarker) {
  // convert into JSON
  std::shared_ptr<VPackBuilder> builder;
  try {
    builder = idx->toVelocyPack(false);
  } catch (...) {
    LOG(ERR) << "cannot save index definition.";
    return TRI_set_errno(TRI_ERROR_INTERNAL);
  }
  if (builder == nullptr) {
    LOG(ERR) << "cannot save index definition.";
    return TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
  }

  // construct filename
  std::string name("index-" + std::to_string(idx->id()) + ".json");
  std::string filename = arangodb::basics::FileUtils::buildFilename(document->_directory, name);

  TRI_vocbase_t* vocbase = document->_vocbase;

  VPackSlice const idxSlice = builder->slice();
  // and save
  bool ok = arangodb::basics::VelocyPackHelper::velocyPackToFile(
      filename.c_str(), idxSlice, document->_vocbase->_settings.forceSyncProperties);

  if (!ok) {
    LOG(ERR) << "cannot save index definition: " << TRI_last_error();

    return TRI_errno();
  }

  if (!writeMarker) {
    return TRI_ERROR_NO_ERROR;
  }

  int res = TRI_ERROR_NO_ERROR;

  try {
    arangodb::wal::CollectionMarker marker(TRI_DF_MARKER_VPACK_CREATE_INDEX, vocbase->_id, document->_info.id(), idxSlice);

    arangodb::wal::SlotInfoCopy slotInfo =
        arangodb::wal::LogfileManager::instance()->allocateAndWrite(marker,
                                                                    false);

    if (slotInfo.errorCode != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(slotInfo.errorCode);
    }

    return TRI_ERROR_NO_ERROR;
  } catch (arangodb::basics::Exception const& ex) {
    res = ex.code();
  } catch (...) {
    res = TRI_ERROR_INTERNAL;
  }

  // TODO: what to do here?
  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a description of all indexes
///
/// the caller must have read-locked the underlying collection!
////////////////////////////////////////////////////////////////////////////////

std::vector<std::shared_ptr<VPackBuilder>> TRI_IndexesDocumentCollection(
    TRI_document_collection_t* document, bool withFigures) {
  auto const& indexes = document->allIndexes();

  std::vector<std::shared_ptr<VPackBuilder>> result;
  result.reserve(indexes.size());

  for (auto const& idx : indexes) {
    auto builder = idx->toVelocyPack(withFigures);

    // shouldn't fail because of reserve
    result.emplace_back(builder);
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief drops an index, including index file removal and replication
////////////////////////////////////////////////////////////////////////////////

bool TRI_DropIndexDocumentCollection(TRI_document_collection_t* document,
                                     TRI_idx_iid_t iid, bool writeMarker) {
  if (iid == 0) {
    // invalid index id or primary index
    return true;
  }

  TRI_vocbase_t* vocbase = document->_vocbase;
  arangodb::Index* found = nullptr;
  {
    READ_LOCKER(readLocker, document->_vocbase->_inventoryLock);

    arangodb::aql::QueryCache::instance()->invalidate(
        vocbase, document->_info.namec_str());
    found = document->removeIndex(iid);
  }

  if (found != nullptr) {
    bool result = RemoveIndexFile(document, found->id());

    delete found;
    found = nullptr;

    if (writeMarker) {
      int res = TRI_ERROR_NO_ERROR;

      try {
        VPackBuilder markerBuilder;
        markerBuilder.openObject();
        markerBuilder.add("id", VPackValue(iid));
        markerBuilder.close();

        arangodb::wal::CollectionMarker marker(TRI_DF_MARKER_VPACK_DROP_INDEX, document->_vocbase->_id, document->_info.id(), markerBuilder.slice());
        
        arangodb::wal::SlotInfoCopy slotInfo =
            arangodb::wal::LogfileManager::instance()->allocateAndWrite(marker,
                                                                        false);

        if (slotInfo.errorCode != TRI_ERROR_NO_ERROR) {
          THROW_ARANGO_EXCEPTION(slotInfo.errorCode);
        }

        return true;
      } catch (arangodb::basics::Exception const& ex) {
        res = ex.code();
      } catch (...) {
        res = TRI_ERROR_INTERNAL;
      }

      LOG(WARN) << "could not save index drop marker in log: " << TRI_errno_string(res);
    }

    // TODO: what to do here?
    return result;
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief converts attribute names to lists of names
////////////////////////////////////////////////////////////////////////////////

static int NamesByAttributeNames(
    std::vector<std::string> const& attributes,
    std::vector<std::vector<arangodb::basics::AttributeName>>& names) {
  names.reserve(attributes.size());

  for (auto const& name : attributes) {
    std::vector<arangodb::basics::AttributeName> attrNameList;
    TRI_ParseAttributeString(name, attrNameList);
    TRI_ASSERT(!attrNameList.empty());
    std::vector<std::string> joinedNames;
    TRI_AttributeNamesJoinNested(attrNameList, joinedNames, true);
    names.emplace_back(attrNameList);
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a geo index to a collection
////////////////////////////////////////////////////////////////////////////////

static arangodb::Index* CreateGeoIndexDocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    std::string const& location, std::string const& latitude,
    std::string const& longitude, bool geoJson, TRI_idx_iid_t iid,
    bool& created) {

  arangodb::Index* idx = nullptr;
  created = false;
  std::unique_ptr<arangodb::GeoIndex2> geoIndex;
  if (!location.empty()) {
    // Use the version with one value
    std::vector<std::string> loc =
        arangodb::basics::StringUtils::split(location, ".");

    // check, if we know the index
    idx = TRI_LookupGeoIndex1DocumentCollection(document, loc, geoJson);

    if (idx != nullptr) {
      LOG(TRACE) << "geo-index already created for location '" << location << "'";
      return idx;
    }

    if (iid == 0) {
      iid = arangodb::Index::generateId();
    }

    geoIndex.reset(new arangodb::GeoIndex2(
        iid, document,
        std::vector<std::vector<arangodb::basics::AttributeName>>{
            {{location, false}}},
        loc, geoJson));

    LOG(TRACE) << "created geo-index for location '" << location << "'";
  } else if (!longitude.empty() && !latitude.empty()) {
    // Use the version with two values
    std::vector<std::string> lat =
        arangodb::basics::StringUtils::split(latitude, ".");

    std::vector<std::string> lon =
        arangodb::basics::StringUtils::split(longitude, ".");

    // check, if we know the index
    idx = TRI_LookupGeoIndex2DocumentCollection(document, lat, lon);

    if (idx != nullptr) {
      LOG(TRACE) << "geo-index already created for latitude '" << latitude
                 << "' and longitude '" << longitude << "'";
      return idx;
    }

    if (iid == 0) {
      iid = arangodb::Index::generateId();
    }

    geoIndex.reset(new arangodb::GeoIndex2(
        iid, document,
        std::vector<std::vector<arangodb::basics::AttributeName>>{
            {{latitude, false}}, {{longitude, false}}},
        std::vector<std::vector<std::string>>{lat, lon}));

    LOG(TRACE) << "created geo-index for latitude '" << latitude
               << "' and longitude '" << longitude << "'";
  } else {
    TRI_set_errno(TRI_ERROR_INTERNAL);
    LOG(TRACE) << "expecting either 'location' or 'latitude' and 'longitude'";
    return nullptr;
  }

  idx = static_cast<arangodb::GeoIndex2*>(geoIndex.get());

  if (idx == nullptr) {
    TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
    return nullptr;
  }

  // initializes the index with all existing documents
  int res = FillIndex(trx, document, idx);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_set_errno(res);

    return nullptr;
  }

  // and store index
  try {
    document->addIndex(idx);
    geoIndex.release();
  } catch (...) {
    TRI_set_errno(res);

    return nullptr;
  }

  created = true;

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int GeoIndexFromVelocyPack(arangodb::Transaction* trx,
                                  TRI_document_collection_t* document,
                                  VPackSlice const& definition,
                                  TRI_idx_iid_t iid, arangodb::Index** dst) {
  if (dst != nullptr) {
    *dst = nullptr;
  }

  VPackSlice const type = definition.get("type");

  if (!type.isString()) {
    return TRI_ERROR_INTERNAL;
  }

  std::string typeStr = type.copyString();

  // extract fields
  VPackSlice fld;
  try {
    fld = ExtractFields(definition, iid);
  } catch (arangodb::basics::Exception const& e) {
    return TRI_set_errno(e.code());
  }
  VPackValueLength fieldCount = fld.length();

  arangodb::Index* idx = nullptr;

  // list style
  if (typeStr == "geo1") {
    // extract geo json
    bool geoJson = arangodb::basics::VelocyPackHelper::getBooleanValue(
        definition, "geoJson", false);

    // need just one field
    if (fieldCount == 1) {
      VPackSlice loc = fld.at(0);
      bool created;

      idx = CreateGeoIndexDocumentCollection(trx, document, loc.copyString(),
                                             std::string(), std::string(),
                                             geoJson, iid, created);

      if (dst != nullptr) {
        *dst = idx;
      }

      return idx == nullptr ? TRI_errno() : TRI_ERROR_NO_ERROR;
    } else {
      LOG(ERR) << "ignoring " << typeStr << "-index " << iid << ", 'fields' must be a list with 1 entries";

      return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
    }
  }

  // attribute style
  else if (typeStr == "geo2") {
    if (fieldCount == 2) {
      VPackSlice lat = fld.at(0);
      VPackSlice lon = fld.at(1);

      bool created;

      idx = CreateGeoIndexDocumentCollection(trx, document, std::string(),
                                             lat.copyString(), lon.copyString(),
                                             false, iid, created);

      if (dst != nullptr) {
        *dst = idx;
      }

      return idx == nullptr ? TRI_errno() : TRI_ERROR_NO_ERROR;
    } else {
      LOG(ERR) << "ignoring " << typeStr << "-index " << iid << ", 'fields' must be a list with 2 entries";

      return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
    }
  } else {
    TRI_ASSERT(false);
  }

  return TRI_ERROR_NO_ERROR;  // shut the vc++ up
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a geo index, list style
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_LookupGeoIndex1DocumentCollection(
    TRI_document_collection_t* document, std::vector<std::string> const& location,
    bool geoJson) {

  for (auto const& idx : document->allIndexes()) {
    if (idx->type() == arangodb::Index::TRI_IDX_TYPE_GEO1_INDEX) {
      auto geoIndex = static_cast<arangodb::GeoIndex2*>(idx);

      if (geoIndex->isSame(location, geoJson)) {
        return idx;
      }
    }
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a geo index, attribute style
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_LookupGeoIndex2DocumentCollection(
    TRI_document_collection_t* document, std::vector<std::string> const& latitude,
    std::vector<std::string> const& longitude) {
  for (auto const& idx : document->allIndexes()) {
    if (idx->type() == arangodb::Index::TRI_IDX_TYPE_GEO2_INDEX) {
      auto geoIndex = static_cast<arangodb::GeoIndex2*>(idx);

      if (geoIndex->isSame(latitude, longitude)) {
        return idx;
      }
    }
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a geo index exists, list style
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_EnsureGeoIndex1DocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    TRI_idx_iid_t iid, std::string const& location, bool geoJson,
    bool& created) {
  READ_LOCKER(readLocker, document->_vocbase->_inventoryLock);

  auto idx =
      CreateGeoIndexDocumentCollection(trx, document, location, std::string(),
                                       std::string(), geoJson, iid, created);

  if (idx != nullptr) {
    if (created) {
      arangodb::aql::QueryCache::instance()->invalidate(
          document->_vocbase, document->_info.namec_str());
      int res = TRI_SaveIndex(document, idx, true);

      if (res != TRI_ERROR_NO_ERROR) {
        idx = nullptr;
      }
    }
  }

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a geo index exists, attribute style
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_EnsureGeoIndex2DocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    TRI_idx_iid_t iid, std::string const& latitude,
    std::string const& longitude, bool& created) {
  READ_LOCKER(readLocker, document->_vocbase->_inventoryLock);

  auto idx = CreateGeoIndexDocumentCollection(
      trx, document, std::string(), latitude, longitude, false, iid, created);

  if (idx != nullptr) {
    if (created) {
      arangodb::aql::QueryCache::instance()->invalidate(
          document->_vocbase, document->_info.namec_str());
      int res = TRI_SaveIndex(document, idx, true);

      if (res != TRI_ERROR_NO_ERROR) {
        idx = nullptr;
      }
    }
  }

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a hash index to the collection
////////////////////////////////////////////////////////////////////////////////

static arangodb::Index* CreateHashIndexDocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    std::vector<std::string> const& attributes, TRI_idx_iid_t iid, bool sparse,
    bool unique, bool& created) {
  created = false;
  std::vector<std::vector<arangodb::basics::AttributeName>> fields;

  int res = NamesByAttributeNames(attributes, fields);

  if (res != TRI_ERROR_NO_ERROR) {
    return nullptr;
  }

  // ...........................................................................
  // Attempt to find an existing index which matches the attributes above.
  // If a suitable index is found, return that one otherwise we need to create
  // a new one.
  // ...........................................................................

  int sparsity = sparse ? 1 : 0;
  auto idx = LookupPathIndexDocumentCollection(
      document, fields, arangodb::Index::TRI_IDX_TYPE_HASH_INDEX, sparsity,
      unique, false);

  if (idx != nullptr) {
    LOG(TRACE) << "hash-index already created";

    return idx;
  }

  if (iid == 0) {
    iid = arangodb::Index::generateId();
  }

  // create the hash index. we'll provide it with the current number of
  // documents
  // in the collection so the index can do a sensible memory preallocation
  auto hashIndex = std::make_unique<arangodb::HashIndex>(iid, document, fields,
                                                         unique, sparse);
  idx = static_cast<arangodb::Index*>(hashIndex.get());

  // initializes the index with all existing documents
  res = FillIndex(trx, document, idx);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_set_errno(res);

    return nullptr;
  }

  // store index and return
  try {
    document->addIndex(idx);
    hashIndex.release();
  } catch (...) {
    TRI_set_errno(res);

    return nullptr;
  }

  created = true;

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int HashIndexFromVelocyPack(arangodb::Transaction* trx,
                                   TRI_document_collection_t* document,
                                   VPackSlice const& definition,
                                   TRI_idx_iid_t iid, arangodb::Index** dst) {
  return PathBasedIndexFromVelocyPack(trx, document, definition, iid,
                                      CreateHashIndexDocumentCollection, dst);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a hash index (unique or non-unique)
/// the index lock must be held when calling this function
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_LookupHashIndexDocumentCollection(
    TRI_document_collection_t* document,
    std::vector<std::string> const& attributes, int sparsity, bool unique) {
  std::vector<std::vector<arangodb::basics::AttributeName>> fields;

  int res = NamesByAttributeNames(attributes, fields);

  if (res != TRI_ERROR_NO_ERROR) {
    return nullptr;
  }

  return LookupPathIndexDocumentCollection(
      document, fields, arangodb::Index::TRI_IDX_TYPE_HASH_INDEX, sparsity,
      unique, true);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a hash index exists
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_EnsureHashIndexDocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    TRI_idx_iid_t iid, std::vector<std::string> const& attributes, bool sparse,
    bool unique, bool& created) {
  READ_LOCKER(readLocker, document->_vocbase->_inventoryLock);

  auto idx = CreateHashIndexDocumentCollection(trx, document, attributes, iid,
                                               sparse, unique, created);

  if (idx != nullptr) {
    if (created) {
      arangodb::aql::QueryCache::instance()->invalidate(
          document->_vocbase, document->_info.namec_str());
      int res = TRI_SaveIndex(document, idx, true);

      if (res != TRI_ERROR_NO_ERROR) {
        idx = nullptr;
      }
    }
  }

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a skiplist index to the collection
////////////////////////////////////////////////////////////////////////////////

static arangodb::Index* CreateSkiplistIndexDocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    std::vector<std::string> const& attributes, TRI_idx_iid_t iid, bool sparse,
    bool unique, bool& created) {
  created = false;
  std::vector<std::vector<arangodb::basics::AttributeName>> fields;

  int res = NamesByAttributeNames(attributes, fields);

  if (res != TRI_ERROR_NO_ERROR) {
    return nullptr;
  }

  // ...........................................................................
  // Attempt to find an existing index which matches the attributes above.
  // If a suitable index is found, return that one otherwise we need to create
  // a new one.
  // ...........................................................................

  int sparsity = sparse ? 1 : 0;
  auto idx = LookupPathIndexDocumentCollection(
      document, fields, arangodb::Index::TRI_IDX_TYPE_SKIPLIST_INDEX, sparsity,
      unique, false);

  if (idx != nullptr) {
    LOG(TRACE) << "skiplist-index already created";

    return idx;
  }

  if (iid == 0) {
    iid = arangodb::Index::generateId();
  }

  // Create the skiplist index
  auto skiplistIndex = std::make_unique<arangodb::SkiplistIndex>(
      iid, document, fields, unique, sparse);
  idx = static_cast<arangodb::Index*>(skiplistIndex.get());

  // initializes the index with all existing documents
  res = FillIndex(trx, document, idx);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_set_errno(res);

    return nullptr;
  }

  // store index and return
  try {
    document->addIndex(idx);
    skiplistIndex.release();
  } catch (...) {
    TRI_set_errno(res);

    return nullptr;
  }

  created = true;

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int SkiplistIndexFromVelocyPack(arangodb::Transaction* trx,
                                       TRI_document_collection_t* document,
                                       VPackSlice const& definition,
                                       TRI_idx_iid_t iid,
                                       arangodb::Index** dst) {
  return PathBasedIndexFromVelocyPack(trx, document, definition, iid,
                                      CreateSkiplistIndexDocumentCollection,
                                      dst);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a skiplist index (unique or non-unique)
/// the index lock must be held when calling this function
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_LookupSkiplistIndexDocumentCollection(
    TRI_document_collection_t* document,
    std::vector<std::string> const& attributes, int sparsity, bool unique) {
  std::vector<std::vector<arangodb::basics::AttributeName>> fields;

  int res = NamesByAttributeNames(attributes, fields);

  if (res != TRI_ERROR_NO_ERROR) {
    return nullptr;
  }

  return LookupPathIndexDocumentCollection(
      document, fields, arangodb::Index::TRI_IDX_TYPE_SKIPLIST_INDEX, sparsity,
      unique, true);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a skiplist index exists
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_EnsureSkiplistIndexDocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    TRI_idx_iid_t iid, std::vector<std::string> const& attributes, bool sparse,
    bool unique, bool& created) {
  READ_LOCKER(readLocker, document->_vocbase->_inventoryLock);

  auto idx = CreateSkiplistIndexDocumentCollection(
      trx, document, attributes, iid, sparse, unique, created);

  if (idx != nullptr) {
    if (created) {
      arangodb::aql::QueryCache::instance()->invalidate(
          document->_vocbase, document->_info.namec_str());
      int res = TRI_SaveIndex(document, idx, true);

      if (res != TRI_ERROR_NO_ERROR) {
        idx = nullptr;
      }
    }
  }

  return idx;
}

static arangodb::Index* LookupFulltextIndexDocumentCollection(
    TRI_document_collection_t* document, std::string const& attribute,
    int minWordLength) {
  for (auto const& idx : document->allIndexes()) {
    if (idx->type() == arangodb::Index::TRI_IDX_TYPE_FULLTEXT_INDEX) {
      auto fulltextIndex = static_cast<arangodb::FulltextIndex*>(idx);

      if (fulltextIndex->isSame(attribute, minWordLength)) {
        return idx;
      }
    }
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a fulltext index to the collection
////////////////////////////////////////////////////////////////////////////////

static arangodb::Index* CreateFulltextIndexDocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    std::string const& attribute, int minWordLength, TRI_idx_iid_t iid,
    bool& created) {
  created = false;

  // ...........................................................................
  // Attempt to find an existing index with the same attribute
  // If a suitable index is found, return that one otherwise we need to create
  // a new one.
  // ...........................................................................

  auto idx =
      LookupFulltextIndexDocumentCollection(document, attribute, minWordLength);

  if (idx != nullptr) {
    LOG(TRACE) << "fulltext-index already created";

    return idx;
  }

  if (iid == 0) {
    iid = arangodb::Index::generateId();
  }

  // Create the fulltext index
  auto fulltextIndex = std::make_unique<arangodb::FulltextIndex>(
      iid, document, attribute, minWordLength);
  idx = static_cast<arangodb::Index*>(fulltextIndex.get());

  // initializes the index with all existing documents
  int res = FillIndex(trx, document, idx);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_set_errno(res);

    return nullptr;
  }

  // store index and return
  try {
    document->addIndex(idx);
    fulltextIndex.release();
  } catch (...) {
    TRI_set_errno(res);

    return nullptr;
  }

  created = true;

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores an index
////////////////////////////////////////////////////////////////////////////////

static int FulltextIndexFromVelocyPack(arangodb::Transaction* trx,
                                       TRI_document_collection_t* document,
                                       VPackSlice const& definition,
                                       TRI_idx_iid_t iid,
                                       arangodb::Index** dst) {
  if (dst != nullptr) {
    *dst = nullptr;
  }

  // extract fields
  VPackSlice fld;
  try {
    fld = ExtractFields(definition, iid);
  } catch (arangodb::basics::Exception const& e) {
    return TRI_set_errno(e.code());
  }
  VPackValueLength fieldCount = fld.length();

  // extract the list of fields
  if (fieldCount != 1) {
    LOG(ERR) << "ignoring index " << iid << ", has an invalid number of attributes";

    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }

  VPackSlice value = fld.at(0);

  if (!value.isString()) {
    return TRI_set_errno(TRI_ERROR_BAD_PARAMETER);
  }

  std::string const attribute = value.copyString();

  // 2013-01-17: deactivated substring indexing
  // indexSubstrings = TRI_LookupObjectJson(definition, "indexSubstrings");

  int minWordLengthValue =
      arangodb::basics::VelocyPackHelper::getNumericValue<int>(
          definition, "minLength", TRI_FULLTEXT_MIN_WORD_LENGTH_DEFAULT);

  // create the index
  auto idx = LookupFulltextIndexDocumentCollection(document, attribute,
                                                   minWordLengthValue);

  if (idx == nullptr) {
    bool created;
    idx = CreateFulltextIndexDocumentCollection(
        trx, document, attribute, minWordLengthValue, iid, created);
  }

  if (dst != nullptr) {
    *dst = idx;
  }

  if (idx == nullptr) {
    LOG(ERR) << "cannot create fulltext index " << iid;
    return TRI_errno();
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finds a fulltext index (unique or non-unique)
/// the index lock must be held when calling this function
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_LookupFulltextIndexDocumentCollection(
    TRI_document_collection_t* document, std::string const& attribute,
    int minWordLength) {
  return LookupFulltextIndexDocumentCollection(document, attribute,
                                               minWordLength);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures that a fulltext index exists
////////////////////////////////////////////////////////////////////////////////

arangodb::Index* TRI_EnsureFulltextIndexDocumentCollection(
    arangodb::Transaction* trx, TRI_document_collection_t* document,
    TRI_idx_iid_t iid, std::string const& attribute, int minWordLength,
    bool& created) {
  READ_LOCKER(readLocker, document->_vocbase->_inventoryLock);

  auto idx = CreateFulltextIndexDocumentCollection(trx, document, attribute,
                                                   minWordLength, iid, created);

  if (idx != nullptr) {
    if (created) {
      arangodb::aql::QueryCache::instance()->invalidate(
          document->_vocbase, document->_info.namec_str());
      int res = TRI_SaveIndex(document, idx, true);

      if (res != TRI_ERROR_NO_ERROR) {
        idx = nullptr;
      }
    }
  }

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief rotate the current journal of the collection
/// use this for testing only
////////////////////////////////////////////////////////////////////////////////

int TRI_RotateJournalDocumentCollection(TRI_document_collection_t* document) {
  int res = TRI_ERROR_ARANGO_NO_JOURNAL;

  TRI_LOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);

  if (document->_state == TRI_COL_STATE_WRITE) {
    size_t const n = document->_journals.size();

    if (n > 0) {
      TRI_ASSERT(document->_journals[0] != nullptr);
      TRI_CloseDatafileDocumentCollection(document, 0, false);

      res = TRI_ERROR_NO_ERROR;
    }
  }

  TRI_UNLOCK_JOURNAL_ENTRIES_DOC_COLLECTION(document);

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads an element from the document collection
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::read(Transaction* trx, std::string const& key,
                                    TRI_doc_mptr_t* mptr, bool lock) {
  TRI_ASSERT(mptr != nullptr);
  mptr->setDataPtr(nullptr);  // PROTECTED by trx in trxCollection

  VPackBuilder builder;
  builder.openObject();
  builder.add(TRI_VOC_ATTRIBUTE_KEY, VPackValue(key));
  builder.close();

  VPackSlice slice = builder.slice();

  {
    TRI_IF_FAILURE("ReadDocumentNoLock") {
      // test what happens if no lock can be acquired
      return TRI_ERROR_DEBUG;
    }

    TRI_IF_FAILURE("ReadDocumentNoLockExcept") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    CollectionReadLocker collectionLocker(this, lock);

    TRI_doc_mptr_t* header;
    int res = lookupDocument(trx, &slice, nullptr, header);

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }

    // we found a document, now copy it over
    *mptr = *header;
  }

  TRI_ASSERT(mptr->getDataPtr() != nullptr);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief inserts a document or edge into the collection
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::insert(Transaction* trx, VPackSlice const* slice,
                                      TRI_doc_mptr_t* mptr,
                                      OperationOptions& options,
                                      bool lock) {

  if (_info.type() == TRI_COL_TYPE_EDGE) {
    VPackSlice s = slice->get(TRI_VOC_ATTRIBUTE_FROM);
    if (!s.isString()) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
    s = slice->get(TRI_VOC_ATTRIBUTE_TO);
    if (!s.isString()) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
  }

  TRI_ASSERT(mptr != nullptr);
  mptr->setDataPtr(nullptr);

  VPackSlice const key(slice->get(TRI_VOC_ATTRIBUTE_KEY));
  uint64_t const hash = key.hash();

  std::unique_ptr<arangodb::wal::Marker> marker;
  if (options.recoveryMarker == nullptr) {
    marker.reset(createVPackInsertMarker(trx, slice));
  }

  TRI_voc_tick_t markerTick = 0;
  int res;
  // now insert into indexes
  {
    TRI_IF_FAILURE("InsertDocumentNoLock") {
      // test what happens if no lock can be acquired
      return TRI_ERROR_DEBUG;
    }

    arangodb::CollectionWriteLocker collectionLocker(this, lock);

    auto actualMarker = (options.recoveryMarker == nullptr ? marker.get() : options.recoveryMarker);
    bool const freeMarker = (options.recoveryMarker == nullptr);

    arangodb::wal::DocumentOperation operation(
        trx, actualMarker, freeMarker, this, TRI_VOC_DOCUMENT_OPERATION_INSERT);

    marker.release();

    // DocumentOperation has taken over the ownership for the marker
    TRI_ASSERT(operation.marker != nullptr);
    TRI_ASSERT(marker == nullptr);

    TRI_IF_FAILURE("InsertDocumentNoHeader") {
      // test what happens if no header can be acquired
      return TRI_ERROR_DEBUG;
    }

    TRI_IF_FAILURE("InsertDocumentNoHeaderExcept") {
      // test what happens if no header can be acquired
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    // create a new header
    TRI_doc_mptr_t* header = operation.header = _masterPointers.request();

    if (header == nullptr) {
      // out of memory. no harm done here. just return the error
      return TRI_ERROR_OUT_OF_MEMORY;
    }

    // update the header we got
    void* mem = operation.marker->mem();
    header->setHash(hash);
    header->setDataPtr(mem);  // PROTECTED by trx in trxCollection

    // insert into indexes
    res = insertDocument(trx, header, operation, mptr, options.waitForSync);

    if (res != TRI_ERROR_NO_ERROR) {
      operation.revert();
    } else {
      TRI_ASSERT(mptr->getDataPtr() !=
                 nullptr);  // PROTECTED by trx in trxCollection

      if (options.waitForSync) {
        markerTick = operation.tick;
      }
    }
  }

  if (markerTick > 0) {
    // need to wait for tick, outside the lock
    arangodb::wal::LogfileManager::instance()->slots()->waitForTick(markerTick);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates a document or edge in a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::update(Transaction* trx,
                                      VPackSlice const newSlice, 
                                      TRI_doc_mptr_t* mptr,
                                      OperationOptions& options,
                                      bool lock,
                                      TRI_voc_rid_t& prevRev) {
  // initialize the result
  TRI_ASSERT(mptr != nullptr);
  mptr->setDataPtr(nullptr);

  TRI_voc_rid_t revisionId = TRI_NewTickServer();
  
  TRI_voc_tick_t markerTick = 0;
  int res;
  {
    TRI_IF_FAILURE("UpdateDocumentNoLock") { return TRI_ERROR_DEBUG; }

    arangodb::CollectionWriteLocker collectionLocker(this, lock);
    
    // get the header pointer of the previous revision
    TRI_doc_mptr_t* oldHeader;
    VPackSlice key = newSlice.get(TRI_VOC_ATTRIBUTE_KEY);
    TRI_ASSERT(!key.isNone());
    res = lookupDocument(trx, key, oldHeader);
    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }

    TRI_IF_FAILURE("UpdateDocumentNoMarker") {
      // test what happens when no marker can be created
      return TRI_ERROR_DEBUG;
    }

    TRI_IF_FAILURE("UpdateDocumentNoMarkerExcept") {
      // test what happens when no marker can be created
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    prevRev = oldHeader->revisionId();

    // Check old revision:
    if (!options.ignoreRevs) {
      VPackSlice expectedRevSlice = newSlice.get(TRI_VOC_ATTRIBUTE_REV);
      int res = checkRevision(trx, expectedRevSlice, prevRev);
      if (res != TRI_ERROR_NO_ERROR) {
        return res;
      }
    }

    // merge old and new values 
    VPackBuilder builder = mergeObjectsForUpdate(
        trx, VPackSlice(oldHeader->vpack()), newSlice, 
        std::to_string(revisionId), options.mergeObjects, options.keepNull);
 
    // create marker
    std::unique_ptr<arangodb::wal::Marker> marker;
    if (options.recoveryMarker == nullptr) {
      marker.reset(createVPackInsertMarker(trx, builder.slice()));
    }
    
    auto actualMarker = (options.recoveryMarker == nullptr 
                        ? marker.get() 
                        : options.recoveryMarker);
    bool const freeMarker = (options.recoveryMarker == nullptr);

    arangodb::wal::DocumentOperation operation(
        trx, actualMarker, freeMarker, this, TRI_VOC_DOCUMENT_OPERATION_UPDATE);

    marker.release();

    // DocumentOperation has taken over the ownership for the marker
    TRI_ASSERT(operation.marker != nullptr);
    TRI_ASSERT(marker == nullptr);

    operation.header = oldHeader;
    operation.init();

    res = updateDocument(trx, revisionId, oldHeader, operation, mptr, options.waitForSync);

    if (res != TRI_ERROR_NO_ERROR) {
      operation.revert();
    } else if (options.waitForSync) {
      markerTick = operation.tick;
    }
  }
  
  if (res == TRI_ERROR_NO_ERROR) {
    TRI_ASSERT(mptr->getDataPtr() != nullptr); 
  }

  if (markerTick > 0) {
    // need to wait for tick, outside the lock
    arangodb::wal::LogfileManager::instance()->slots()->waitForTick(markerTick);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief replaces a document or edge in a collection
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::replace(Transaction* trx,
                                       VPackSlice const newSlice, 
                                       TRI_doc_mptr_t* mptr,
                                       OperationOptions& options,
                                       bool lock,
                                       TRI_voc_rid_t& prevRev) {

  if (_info.type() == TRI_COL_TYPE_EDGE) {
    VPackSlice s = newSlice.get(TRI_VOC_ATTRIBUTE_FROM);
    if (!s.isString()) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
    s = newSlice.get(TRI_VOC_ATTRIBUTE_TO);
    if (!s.isString()) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
  }

  // initialize the result
  TRI_ASSERT(mptr != nullptr);
  mptr->setDataPtr(nullptr);

  TRI_voc_rid_t revisionId = TRI_NewTickServer();
  
  TRI_voc_tick_t markerTick = 0;
  int res;
  {
    TRI_IF_FAILURE("ReplaceDocumentNoLock") { return TRI_ERROR_DEBUG; }

    arangodb::CollectionWriteLocker collectionLocker(this, lock);
    
    // get the header pointer of the previous revision
    TRI_doc_mptr_t* oldHeader;
    VPackSlice key = newSlice.get(TRI_VOC_ATTRIBUTE_KEY);
    TRI_ASSERT(!key.isNone());
    res = lookupDocument(trx, key, oldHeader);
    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }

    TRI_IF_FAILURE("ReplaceDocumentNoMarker") {
      // test what happens when no marker can be created
      return TRI_ERROR_DEBUG;
    }

    TRI_IF_FAILURE("ReplaceDocumentNoMarkerExcept") {
      // test what happens when no marker can be created
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    prevRev = oldHeader->revisionId();

    // Check old revision:
    if (!options.ignoreRevs) {
      VPackSlice expectedRevSlice = newSlice.get(TRI_VOC_ATTRIBUTE_REV);
      int res = checkRevision(trx, expectedRevSlice, prevRev);
      if (res != TRI_ERROR_NO_ERROR) {
        return res;
      }
    }

    // merge old and new values 
    VPackBuilder builder = newObjectForReplace(
        trx, VPackSlice(oldHeader->vpack()),
        newSlice, std::to_string(revisionId));

    // create marker
    std::unique_ptr<arangodb::wal::Marker> marker;
    if (options.recoveryMarker == nullptr) {
      marker.reset(createVPackInsertMarker(trx, builder.slice()));
    }
    
    auto actualMarker = (options.recoveryMarker == nullptr ? marker.get() : options.recoveryMarker);
    bool const freeMarker = (options.recoveryMarker == nullptr);

    arangodb::wal::DocumentOperation operation(
        trx, actualMarker, freeMarker, this, TRI_VOC_DOCUMENT_OPERATION_REPLACE);
    
    marker.release();
    
    // DocumentOperation has taken over the ownership for the marker
    TRI_ASSERT(operation.marker != nullptr);
    TRI_ASSERT(marker == nullptr);
    
    operation.header = oldHeader;
    operation.init();

    res = updateDocument(trx, revisionId, oldHeader, operation, mptr, options.waitForSync);

    if (res != TRI_ERROR_NO_ERROR) {
      operation.revert();
    } else if (options.waitForSync) {
      markerTick = operation.tick;
    }
  }
  
  if (res == TRI_ERROR_NO_ERROR) {
    TRI_ASSERT(mptr->getDataPtr() != nullptr); 
  }

  if (markerTick > 0) {
    // need to wait for tick, outside the lock
    arangodb::wal::LogfileManager::instance()->slots()->waitForTick(markerTick);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief removes a document or edge
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::remove(arangodb::Transaction* trx,
                                      VPackSlice const* slice,
                                      TRI_doc_update_policy_t const* policy,
                                      OperationOptions& options,
                                      bool lock) {
  TRI_IF_FAILURE("RemoveDocumentNoMarker") {
    // test what happens when no marker can be created
    return TRI_ERROR_DEBUG;
  }

  TRI_IF_FAILURE("RemoveDocumentNoMarkerExcept") {
    // test what happens if no marker can be created
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  
  std::unique_ptr<arangodb::wal::Marker> marker;
  if (options.recoveryMarker == nullptr) {
    marker.reset(createVPackRemoveMarker(trx, slice));
  }

  int res;
  TRI_voc_tick_t markerTick = 0;
  {
    TRI_IF_FAILURE("RemoveDocumentNoLock") {
      // test what happens if no lock can be acquired
      return TRI_ERROR_DEBUG;
    }

    arangodb::CollectionWriteLocker collectionLocker(this, lock);

    auto actualMarker = (options.recoveryMarker == nullptr ? marker.get() : options.recoveryMarker);
    bool const freeMarker = (options.recoveryMarker == nullptr);

    arangodb::wal::DocumentOperation operation(
        trx, actualMarker, freeMarker, this, TRI_VOC_DOCUMENT_OPERATION_REMOVE);

    marker.release();
    
    // DocumentOperation has taken over the ownership for the marker
    TRI_ASSERT(operation.marker != nullptr);
    TRI_ASSERT(marker == nullptr);

    TRI_doc_mptr_t* header;
    res = lookupDocument(trx, slice, policy, header);

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }

    // we found a document to remove
    TRI_ASSERT(header != nullptr);
    operation.header = header;
    operation.init();

    // delete from indexes
    res = deleteSecondaryIndexes(trx, header, false);

    if (res != TRI_ERROR_NO_ERROR) {
      insertSecondaryIndexes(trx, header, true);
      return res;
    }

    res = deletePrimaryIndex(trx, header);

    if (res != TRI_ERROR_NO_ERROR) {
      insertSecondaryIndexes(trx, header, true);
      return res;
    }

    operation.indexed();
    _numberDocuments--;

    TRI_IF_FAILURE("RemoveDocumentNoOperation") { return TRI_ERROR_DEBUG; }

    TRI_IF_FAILURE("RemoveDocumentNoOperationExcept") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }
  
    res = TRI_AddOperationTransaction(trx->getInternals(), operation, options.waitForSync);

    if (res != TRI_ERROR_NO_ERROR) {
      operation.revert();
    } else if (options.waitForSync) {
      markerTick = operation.tick;
    }
  }
  
  if (markerTick > 0) {
    // need to wait for tick, outside the lock
    arangodb::wal::LogfileManager::instance()->slots()->waitForTick(markerTick);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief rolls back a document operation
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::rollbackOperation(arangodb::Transaction* trx, 
                                                 TRI_voc_document_operation_e type, 
                                                 TRI_doc_mptr_t* header,
                                                 TRI_doc_mptr_t const* oldData) {
  if (type == TRI_VOC_DOCUMENT_OPERATION_INSERT) {
    // ignore any errors we're getting from this
    deletePrimaryIndex(trx, header);
    deleteSecondaryIndexes(trx, header, true);

    TRI_ASSERT(_numberDocuments > 0);
    _numberDocuments--;

    return TRI_ERROR_NO_ERROR;
  } else if (type == TRI_VOC_DOCUMENT_OPERATION_UPDATE ||
             type == TRI_VOC_DOCUMENT_OPERATION_REPLACE) {
    // copy the existing header's state
    TRI_doc_mptr_t copy = *header;

    // remove the current values from the indexes
    deleteSecondaryIndexes(trx, header, true);
    // revert to the old state
    header->copy(*oldData);
    // re-insert old state
    int res = insertSecondaryIndexes(trx, header, true);
    // revert again to the new state, because other parts of the new state
    // will be reverted at some other place
    header->copy(copy);

    return res;
  } else if (type == TRI_VOC_DOCUMENT_OPERATION_REMOVE) {
    int res = insertPrimaryIndex(trx, header);

    if (res == TRI_ERROR_NO_ERROR) {
      res = insertSecondaryIndexes(trx, header, true);
      _numberDocuments++;
    } else {
      LOG(ERR) << "error rolling back remove operation";
    }
    return res;
  }

  return TRI_ERROR_INTERNAL;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a vpack-based insert marker for documents / edges
////////////////////////////////////////////////////////////////////////////////

arangodb::wal::Marker* TRI_document_collection_t::createVPackInsertMarker(
    Transaction* trx, VPackSlice const* slice) {
  return new arangodb::wal::CrudMarker(TRI_DF_MARKER_VPACK_DOCUMENT, trx->getInternals()->_id, *slice);
}

arangodb::wal::Marker* TRI_document_collection_t::createVPackInsertMarker(
    Transaction* trx, VPackSlice const& slice) {
  return createVPackInsertMarker(trx, &slice);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a vpack-based remove marker for documents / edges
////////////////////////////////////////////////////////////////////////////////

arangodb::wal::Marker* TRI_document_collection_t::createVPackRemoveMarker(
    Transaction* trx, VPackSlice const* slice) {
  return new arangodb::wal::CrudMarker(TRI_DF_MARKER_VPACK_REMOVE, trx->getInternals()->_id, *slice);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up a document by key, low level worker
/// the caller must make sure the read lock on the collection is held
/// the slice contains _key and possibly _rev
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::lookupDocument(
    arangodb::Transaction* trx, VPackSlice const* slice,
    TRI_doc_update_policy_t const* policy, TRI_doc_mptr_t*& header) {
  
  VPackSlice key = slice->get(TRI_VOC_ATTRIBUTE_KEY);
  
  if (!key.isString()) {
    return TRI_ERROR_INTERNAL;
  }
  VPackBuilder searchValue;
  searchValue.openArray();
  searchValue.openObject();
  searchValue.add(TRI_SLICE_KEY_EQUAL, key);
  searchValue.close();
  searchValue.close();
    
  header = primaryIndex()->lookup(trx, searchValue.slice());

  if (header == nullptr) {
    return TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND;
  }

  if (policy != nullptr) {
    return policy->check(header->revisionId());
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up a document by key, low level worker
/// the caller must make sure the read lock on the collection is held
/// the key must be a string slice, no revision check is performed
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::lookupDocument(
    arangodb::Transaction* trx, VPackSlice const key,
    TRI_doc_mptr_t*& header) {
  
  if (!key.isString()) {
    return TRI_ERROR_INTERNAL;
  }

  VPackBuilder searchValue;
  searchValue.openArray();
  searchValue.openObject();
  searchValue.add(TRI_SLICE_KEY_EQUAL, key);
  searchValue.close();
  searchValue.close();
    
  header = primaryIndex()->lookup(trx, searchValue.slice());

  if (header == nullptr) {
    return TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks the revision of a document
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::checkRevision(Transaction* trx,
                                             VPackSlice const expected,
                                             TRI_voc_rid_t found) {
  TRI_voc_rid_t expectedRev = 0;
  if (expected.isString()) {
    expectedRev = arangodb::basics::VelocyPackHelper::stringUInt64(expected);
  } else if (expected.isNumber()) {
    expectedRev = expected.getNumber<TRI_voc_rid_t>();
  }
  if (expectedRev != 0 && found != expectedRev) {
    return TRI_ERROR_ARANGO_CONFLICT;
  }
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief updates an existing document, low level worker
/// the caller must make sure the write lock on the collection is held
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::updateDocument(arangodb::Transaction* trx,
                          TRI_voc_rid_t revisionId,
                          TRI_doc_mptr_t* oldHeader,
                          arangodb::wal::DocumentOperation& operation,
                          TRI_doc_mptr_t* mptr, bool& waitForSync) {

  // save the old data, remember
  TRI_doc_mptr_t oldData = *oldHeader;

  // remove old document from secondary indexes
  // (it will stay in the primary index as the key won't change)
  int res = deleteSecondaryIndexes(trx, oldHeader, false);

  if (res != TRI_ERROR_NO_ERROR) {
    // re-enter the document in case of failure, ignore errors during rollback
    insertSecondaryIndexes(trx, oldHeader, true);
    return res;
  }

  // update header
  TRI_doc_mptr_t* newHeader = oldHeader;

  // update the header. this will modify oldHeader, too !!!
  newHeader->setDataPtr(operation.marker->mem()); 

  // insert new document into secondary indexes
  res = insertSecondaryIndexes(trx, newHeader, false);

  if (res != TRI_ERROR_NO_ERROR) {
    // rollback
    deleteSecondaryIndexes(trx, newHeader, true);

    // copy back old header data
    oldHeader->copy(oldData);

    insertSecondaryIndexes(trx, oldHeader, true);

    return res;
  }

  operation.indexed();

  TRI_IF_FAILURE("UpdateDocumentNoOperation") { return TRI_ERROR_DEBUG; }

  TRI_IF_FAILURE("UpdateDocumentNoOperationExcept") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  res = TRI_AddOperationTransaction(trx->getInternals(), operation, waitForSync);

  if (res == TRI_ERROR_NO_ERROR) {
    // write new header into result
    *mptr = *newHeader;
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief insert a document, low level worker
/// the caller must make sure the write lock on the collection is held
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::insertDocument(
    arangodb::Transaction* trx, TRI_doc_mptr_t* header,
    arangodb::wal::DocumentOperation& operation, TRI_doc_mptr_t* mptr,
    bool& waitForSync) {
  TRI_ASSERT(header != nullptr);
  TRI_ASSERT(mptr != nullptr);

  // .............................................................................
  // insert into indexes
  // .............................................................................

  // insert into primary index first
  int res = insertPrimaryIndex(trx, header);

  if (res != TRI_ERROR_NO_ERROR) {
    // insert has failed
    return res;
  }

  // insert into secondary indexes
  res = insertSecondaryIndexes(trx, header, false);

  if (res != TRI_ERROR_NO_ERROR) {
    deleteSecondaryIndexes(trx, header, true);
    deletePrimaryIndex(trx, header);
    return res;
  }

  _numberDocuments++;

  operation.indexed();

  TRI_IF_FAILURE("InsertDocumentNoOperation") { return TRI_ERROR_DEBUG; }

  TRI_IF_FAILURE("InsertDocumentNoOperationExcept") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  res = TRI_AddOperationTransaction(trx->getInternals(), operation, waitForSync);

  if (res == TRI_ERROR_NO_ERROR) {
    *mptr = *header;
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new entry in the primary index
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::insertPrimaryIndex(arangodb::Transaction* trx,
                                                  TRI_doc_mptr_t* header) {
  TRI_IF_FAILURE("InsertPrimaryIndex") { return TRI_ERROR_DEBUG; }

  TRI_doc_mptr_t* found;

  TRI_ASSERT(header != nullptr);
  TRI_ASSERT(header->getDataPtr() != nullptr); 

  // insert into primary index
  int res = primaryIndex()->insertKey(trx, header, (void const**)&found);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  if (found == nullptr) {
    // success
    return TRI_ERROR_NO_ERROR;
  }

  return TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new entry in the secondary indexes
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::insertSecondaryIndexes(
    arangodb::Transaction* trx, TRI_doc_mptr_t const* header, bool isRollback) {
  TRI_IF_FAILURE("InsertSecondaryIndexes") { return TRI_ERROR_DEBUG; }

  if (!useSecondaryIndexes()) {
    return TRI_ERROR_NO_ERROR;
  }

  int result = TRI_ERROR_NO_ERROR;

  auto const& indexes = allIndexes();
  size_t const n = indexes.size();

  for (size_t i = 1; i < n; ++i) {
    auto idx = indexes[i];
    int res = idx->insert(trx, header, isRollback);

    // in case of no-memory, return immediately
    if (res == TRI_ERROR_OUT_OF_MEMORY) {
      return res;
    } else if (res != TRI_ERROR_NO_ERROR) {
      if (res == TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED ||
          result == TRI_ERROR_NO_ERROR) {
        // "prefer" unique constraint violated
        result = res;
      }
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes an entry from the primary index
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::deletePrimaryIndex(
    arangodb::Transaction* trx, TRI_doc_mptr_t const* header) {
  TRI_IF_FAILURE("DeletePrimaryIndex") { return TRI_ERROR_DEBUG; }

  auto found = primaryIndex()->removeKey(
      trx,
      VPackSlice(header->vpack()).get(TRI_VOC_ATTRIBUTE_KEY));  // ONLY IN INDEX, PROTECTED by RUNTIME

  if (found == nullptr) {
    return TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes an entry from the secondary indexes
////////////////////////////////////////////////////////////////////////////////

int TRI_document_collection_t::deleteSecondaryIndexes(
    arangodb::Transaction* trx, TRI_doc_mptr_t const* header, bool isRollback) {
  if (!useSecondaryIndexes()) {
    return TRI_ERROR_NO_ERROR;
  }

  TRI_IF_FAILURE("DeleteSecondaryIndexes") { return TRI_ERROR_DEBUG; }

  int result = TRI_ERROR_NO_ERROR;

  auto const& indexes = allIndexes();
  size_t const n = indexes.size();

  for (size_t i = 1; i < n; ++i) {
    auto idx = indexes[i];
    int res = idx->remove(trx, header, isRollback);

    if (res != TRI_ERROR_NO_ERROR) {
      // an error occurred
      result = res;
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief new object for replace, oldValue must have _key and _id correctly
/// set.
////////////////////////////////////////////////////////////////////////////////
    
VPackBuilder TRI_document_collection_t::newObjectForReplace(
    Transaction* trx,
    VPackSlice const oldValue,
    VPackSlice const newValue,
    std::string const& rev) {
  // replace
  VPackBuilder builder;
  { VPackObjectBuilder guard(&builder);

    VPackObjectIterator it(newValue);
    while (it.valid()) {
      std::string key(it.key().copyString());
      if (key[0] != '_' ||
          (key != TRI_VOC_ATTRIBUTE_ID &&
           key != TRI_VOC_ATTRIBUTE_KEY &&
           key != TRI_VOC_ATTRIBUTE_REV)) {
        builder.add(key, it.value());
      }
      it.next();
    }
    VPackSlice s = oldValue.get(TRI_VOC_ATTRIBUTE_ID);
    TRI_ASSERT(!s.isNone());
    builder.add(TRI_VOC_ATTRIBUTE_ID, s);
    s = oldValue.get(TRI_VOC_ATTRIBUTE_KEY);
    TRI_ASSERT(!s.isNone());
    builder.add(TRI_VOC_ATTRIBUTE_KEY, s);
    builder.add(TRI_VOC_ATTRIBUTE_REV, VPackValue(rev));
  }
  return builder;
} 

////////////////////////////////////////////////////////////////////////////////
/// @brief merge two objects for update, oldValue must have correctly set
/// _key and _id attributes
////////////////////////////////////////////////////////////////////////////////
    
VPackBuilder TRI_document_collection_t::mergeObjectsForUpdate(
      arangodb::Transaction* trx,
      VPackSlice const oldValue,
      VPackSlice const newValue,
      std::string const& rev,
      bool mergeObjects, bool keepNull) {

  VPackBuilder b;
  { VPackObjectBuilder guard(&b);

    // Find the attributes in the newValue object:
    std::unordered_map<std::string, VPackSlice> newValues;
    { VPackObjectIterator it(newValue);
      while (it.valid()) {
        std::string key = it.key().copyString();
        if (key != TRI_VOC_ATTRIBUTE_KEY &&
            key != TRI_VOC_ATTRIBUTE_ID &&
            key != TRI_VOC_ATTRIBUTE_REV) {
          newValues.emplace(it.key().copyString(), it.value());
          it.next();
        }
      }
    }

    { VPackObjectIterator it(oldValue);
      while (it.valid()) {
        auto key = it.key().copyString();
        if (key == TRI_VOC_ATTRIBUTE_REV) {
          it.next();
          continue;
        }
        auto found = newValues.find(key);

        if (found == newValues.end()) {
          // use old value
          b.add(key, it.value());
        } else if (mergeObjects && it.value().isObject() &&
                   (*found).second.isObject()) {
          // merge both values
          auto& value = (*found).second;
          if (keepNull || (!value.isNone() && !value.isNull())) {
            VPackBuilder sub = VPackCollection::merge(it.value(), value, 
                                                      true, !keepNull);
            b.add(key, sub.slice());
          }
          // clear the value in the map so its not added again
          (*found).second = VPackSlice();
        } else {
          // use new value
          auto& value = (*found).second;
          if (keepNull || (!value.isNone() && !value.isNull())) {
            b.add(key, value);
          }
          // clear the value in the map so its not added again
          (*found).second = VPackSlice();
        }
        it.next();
      }
    }

    // add remaining values that were only in new object
    for (auto& it : newValues) {
      auto& s = it.second;
      if (s.isNone()) {
        continue;
      }
      if (!keepNull && s.isNull()) {
        continue;
      }
      b.add(std::move(it.first), s);
    }

    // Finally, add the new revision:
    b.add(TRI_VOC_ATTRIBUTE_REV, VPackValue(rev));
  }
  return b;
}
