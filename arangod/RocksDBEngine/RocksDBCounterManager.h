////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_ROCKSDB_ENGINE_ROCKSDB_COUNTMANAGER_H
#define ARANGOD_ROCKSDB_ENGINE_ROCKSDB_COUNTMANAGER_H 1

#include <rocksdb/types.h>
#include "Basics/Common.h"
#include "Basics/ReadWriteLock.h"
#include "Basics/Result.h"
#include "VocBase/voc-types.h"
#include "RocksDBEngine/RocksDBTypes.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>
#include <atomic>

namespace rocksdb {
class DB;
class Transaction;
}

namespace arangodb {
  
class RocksDBCounterManager {
  friend class RocksDBEngine;
  
  /// Constructor needs to be called synchronously,
  /// will load counts from the db and scan the WAL
  explicit RocksDBCounterManager(rocksdb::DB* db);

 public:
    
  struct CounterAdjustment {
    rocksdb::SequenceNumber _sequenceNum = 0;
    uint64_t _added = 0;
    uint64_t _removed = 0;
    TRI_voc_rid_t _revisionId = 0; // used for revision id
    
    CounterAdjustment() {}
    CounterAdjustment(rocksdb::SequenceNumber seq, uint64_t added,
                  uint64_t removed, TRI_voc_rid_t revisionId)
    : _sequenceNum(seq), _added(added), _removed(removed), _revisionId(revisionId) {}
    
    rocksdb::SequenceNumber sequenceNumber() const {return _sequenceNum;};
    uint64_t added() const { return _added; }
    uint64_t removed() const { return _removed; }
    TRI_voc_rid_t revisionId() const { return _revisionId; }
  };
  

  /// Thread-Safe load a counter
  CounterAdjustment loadCounter(uint64_t objectId) const;

  /// collections / views / indexes can call this method to update
  /// their total counts. Thread-Safe needs the snapshot so we know
  /// the sequence number used
  void updateCounter(uint64_t objectId,
                     CounterAdjustment const&);

  /// Thread-Safe remove a counter
  void removeCounter(uint64_t objectId);

  /// Thread-Safe force sync
  arangodb::Result sync(bool force);

 protected:
  
  struct CMValue {
    /// ArangoDB transaction ID
    rocksdb::SequenceNumber  _sequenceNum;
    /// used for number of documents
    uint64_t _count;
    /// used for revision id
    TRI_voc_rid_t _revisionId;
    
    CMValue(rocksdb::SequenceNumber sq, uint64_t cc, TRI_voc_rid_t rid)
    : _sequenceNum(sq), _count(cc), _revisionId(rid) {}
    explicit CMValue(arangodb::velocypack::Slice const&);
    void serialize(arangodb::velocypack::Builder&) const;
  };

  
  void readSettings();
  void writeSettings();

  void readCounterValues();
  bool parseRocksWAL();
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief counter values
  //////////////////////////////////////////////////////////////////////////////
  std::unordered_map<uint64_t, CMValue> _counters;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief synced sequence numbers
  //////////////////////////////////////////////////////////////////////////////
  std::unordered_map<uint64_t, rocksdb::SequenceNumber> _syncedSeqNums;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief currently syncing
  //////////////////////////////////////////////////////////////////////////////
  std::atomic<bool> _syncing;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief rocsdb instance
  //////////////////////////////////////////////////////////////////////////////
  rocksdb::DB* _db;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief protect _syncing and _counters
  //////////////////////////////////////////////////////////////////////////////
  mutable basics::ReadWriteLock _rwLock;
};
}

#endif
