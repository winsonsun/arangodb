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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "SingleServerEdgeCursor.h"

#include "Graph/BaseOptions.h"
#include "StorageEngine/DocumentIdentifierToken.h"
#include "Transaction/Methods.h"
#include "Utils/OperationCursor.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ManagedDocumentResult.h"
#include "VocBase/TraverserCache.h"

using namespace arangodb;
using namespace arangodb::graph;

////////////////////////////////////////////////////////////////////////////////
/// @brief Get a document by it's ID. Also lazy locks the collection.
///        If DOCUMENT_NOT_FOUND this function will return normally
///        with a OperationResult.failed() == true.
///        On all other cases this function throws.
////////////////////////////////////////////////////////////////////////////////

SingleServerEdgeCursor::SingleServerEdgeCursor(
    ManagedDocumentResult* mmdr, BaseOptions* opts, size_t nrCursors,
    std::vector<size_t> const* mapping)
    : _opts(opts),
      _trx(opts->trx()),
      _mmdr(mmdr),
      _cursors(),
      _currentCursor(0),
      _currentSubCursor(0),
      _cachePos(0),
      _internalCursorMapping(mapping) {
  TRI_ASSERT(_mmdr != nullptr);
  _cursors.reserve(nrCursors);
  _cache.reserve(1000);
  if (_opts->cache() == nullptr) {
    throw;
  }
};

SingleServerEdgeCursor::~SingleServerEdgeCursor() {
  for (auto& it : _cursors) {
    for (auto& it2 : it) {
      delete it2;
    }
  }
}

bool SingleServerEdgeCursor::next(
    std::function<void(StringRef const&, VPackSlice, size_t)> callback) {
  if (_currentCursor == _cursors.size()) {
    return false;
  }
  if (_cachePos < _cache.size()) {
    LogicalCollection* collection =
        _cursors[_currentCursor][_currentSubCursor]->collection();
    if (collection->readDocument(_trx, _cache[_cachePos++], *_mmdr)) {
      VPackSlice edgeDocument(_mmdr->vpack());
      std::string eid = _trx->extractIdString(edgeDocument);
      StringRef persId = _opts->cache()->persistString(StringRef(eid));
      _opts->cache()->insertDocument(persId, edgeDocument);
      if (_internalCursorMapping != nullptr) {
        TRI_ASSERT(_currentCursor < _internalCursorMapping->size());
        callback(persId, edgeDocument,
                 _internalCursorMapping->at(_currentCursor));
      } else {
        callback(persId, edgeDocument, _currentCursor);
      }
    }

    return true;
  }
  // We need to refill the cache.
  _cachePos = 0;
  auto cursorSet = _cursors[_currentCursor];
  while (cursorSet.empty()) {
    // Fast Forward to the next non-empty cursor set
    _currentCursor++;
    _currentSubCursor = 0;
    if (_currentCursor == _cursors.size()) {
      return false;
    }
    cursorSet = _cursors[_currentCursor];
  }
  auto cursor = cursorSet[_currentSubCursor];
  // NOTE: We cannot clear the cache,
  // because the cursor expect's it to be filled.
  do {
    if (!cursor->hasMore()) {
      // This one is exhausted, next
      ++_currentSubCursor;
      while (_currentSubCursor == cursorSet.size()) {
        ++_currentCursor;
        _currentSubCursor = 0;
        if (_currentCursor == _cursors.size()) {
          // We are done, all cursors exhausted.
          return false;
        }
        cursorSet = _cursors[_currentCursor];
      }
      cursor = cursorSet[_currentSubCursor];
      // If we switch the cursor. We have to clear the cache.
      _cache.clear();
    } else {
      _cache.clear();
      auto cb = [&](DocumentIdentifierToken const& token) {
        _cache.emplace_back(token);
      };
      bool tmp = cursor->getMore(cb, 1000);
      TRI_ASSERT(tmp == cursor->hasMore());
    }
  } while (_cache.empty());

  TRI_ASSERT(_cachePos < _cache.size());
  LogicalCollection* collection = cursor->collection();
  if (collection->readDocument(_trx, _cache[_cachePos++], *_mmdr)) {
    VPackSlice edgeDocument(_mmdr->vpack());
    std::string eid = _trx->extractIdString(edgeDocument);
    StringRef persId = _opts->cache()->persistString(StringRef(eid));
    _opts->cache()->insertDocument(persId, edgeDocument);
    if (_internalCursorMapping != nullptr) {
      TRI_ASSERT(_currentCursor < _internalCursorMapping->size());
      callback(persId, edgeDocument,
               _internalCursorMapping->at(_currentCursor));
    } else {
      callback(persId, edgeDocument, _currentCursor);
    }
  }
  return true;
}

void SingleServerEdgeCursor::readAll(
    std::function<void(StringRef const&, arangodb::velocypack::Slice, size_t&)>
        callback) {
  size_t cursorId = 0;
  for (_currentCursor = 0; _currentCursor < _cursors.size(); ++_currentCursor) {
    if (_internalCursorMapping != nullptr) {
      TRI_ASSERT(_currentCursor < _internalCursorMapping->size());
      cursorId = _internalCursorMapping->at(_currentCursor);
    } else {
      cursorId = _currentCursor;
    }
    auto& cursorSet = _cursors[_currentCursor];
    for (auto& cursor : cursorSet) {
      LogicalCollection* collection = cursor->collection();
      auto cb = [&](DocumentIdentifierToken const& token) {
        if (collection->readDocument(_trx, token, *_mmdr)) {
          VPackSlice doc(_mmdr->vpack());
          std::string tmpId = _trx->extractIdString(doc);
          StringRef edgeId = _opts->cache()->persistString(StringRef(tmpId));
          _opts->cache()->insertDocument(edgeId, doc);
          callback(edgeId, doc, cursorId);
        }
      };
      while (cursor->getMore(cb, 1000)) {
      }
    }
  }
}
