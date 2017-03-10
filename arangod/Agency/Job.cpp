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
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "Job.h"

static std::string const DBServer = "DBServer";

using namespace arangodb::consensus;

bool arangodb::consensus::compareServerLists(Slice plan, Slice current) {
  if (!plan.isArray() || !current.isArray()) {
    return false;
  }
  std::vector<std::string> planv, currv;
  for (auto const& srv : VPackArrayIterator(plan)) {
    if (srv.isString()) {
      planv.push_back(srv.copyString());
    }
  }
  for (auto const& srv : VPackArrayIterator(current)) {
    if (srv.isString()) {
      currv.push_back(srv.copyString());
    }
  }
  bool equalLeader = !planv.empty() && !currv.empty() &&
                     planv.front() == currv.front();
  std::sort(planv.begin(), planv.end());
  std::sort(currv.begin(), currv.end());
  return equalLeader && currv == planv;
}

Job::Job(JOB_STATUS status, Node const& snapshot, AgentInterface* agent,
         std::string const& jobId, std::string const& creator)
  : _status(status),
    _snapshot(snapshot),
    _agent(agent),
    _jobId(jobId),
    _creator(creator),
    _jb(nullptr) {
}

Job::~Job() {}

// this will be initialized in the AgencyFeature
std::string Job::agencyPrefix = "/arango";

JOB_STATUS Job::exists() const {

  Node const& target = _snapshot("/Target");
  
  if (target.exists(std::string("/ToDo/") + _jobId).size() == 2) {
    return TODO;
  } else if (target.exists(std::string("/Pending/") + _jobId).size() == 2) {
    return PENDING;
  } else if (target.exists(std::string("/Finished/") + _jobId).size() == 2) {
    return FINISHED;
  } else if (target.exists(std::string("/Failed/") + _jobId).size() == 2) {
    return FAILED;
  }
  
  return NOTFOUND;
}

bool Job::finish(std::string const& server, std::string const& shard,
                 bool success, std::string const& reason) {
  
  Builder pending, finished;
  
  // Get todo entry
  bool started = false;
  { VPackArrayBuilder guard(&pending);
    if (_snapshot.exists(pendingPrefix + _jobId).size() == 3) {
      _snapshot(pendingPrefix + _jobId).toBuilder(pending);
      started = true;
    } else if (_snapshot.exists(toDoPrefix + _jobId).size() == 3) {
      _snapshot(toDoPrefix + _jobId).toBuilder(pending);
    } else {
      LOG_TOPIC(DEBUG, Logger::AGENCY)
        << "Nothing in pending to finish up for job " << _jobId;
      return false;
    }
  }

  std::string jobType;
  try {
    jobType = pending.slice()[0].get("type").copyString();
  } catch (std::exception const&) {
    LOG_TOPIC(WARN, Logger::AGENCY)
      << "Failed to obtain type of job " << _jobId;
  }
  
  // Prepare pending entry, block toserver
  { VPackArrayBuilder guard(&finished);
    VPackObjectBuilder guard2(&finished);

    addPutJobIntoSomewhere(finished, success ? "Finished" : "Failed",
                           pending.slice()[0], reason);

    addRemoveJobFromSomewhere(finished, "Todo", _jobId);
    addRemoveJobFromSomewhere(finished, "Pending", _jobId);

    // --- Remove blocks if specified:
    if (started && !server.empty()) {
      addReleaseServer(finished, server);
    }
    if (started && !shard.empty()) {
      addReleaseShard(finished, shard);
    }
  }  // close object and array

  write_ret_t res = transact(_agent, finished);
  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    LOG_TOPIC(DEBUG, Logger::AGENCY)
      << "Successfully finished job " << jobType << "(" << _jobId << ")";
    _status = (success ? FINISHED : FAILED);
    return true;
  }

  return false;
}


std::vector<std::string> Job::availableServers(Node const& snapshot) {

  std::vector<std::string> ret;

  // Get servers from plan
  Node::Children const& dbservers = snapshot(plannedServers).children();
  for (auto const& srv : dbservers) {
    ret.push_back(srv.first);
  }
  
  // Remove cleaned servers from ist
  try {
    for (auto const& srv :
           VPackArrayIterator(snapshot(cleanedPrefix).slice())) {
      ret.erase(
        std::remove(ret.begin(), ret.end(), srv.copyString()),
        ret.end());
    }
  } catch (...) {}

  // Remove failed servers from list
  try {
    for (auto const& srv :
           VPackObjectIterator(snapshot(failedServersPrefix).slice())) {
      ret.erase(
        std::remove(ret.begin(), ret.end(), srv.key.copyString()),
        ret.end());
    }
  } catch (...) {}
  
  return ret;
  
}

std::vector<Job::shard_t> Job::clones(
  Node const& snapshot, std::string const& database,
  std::string const& collection, std::string const& shard) {

  std::vector<shard_t> ret;
  ret.emplace_back(collection, shard);  // add (collection, shard) as first item

  try {
    std::string databasePath = planColPrefix + database,
      planPath = databasePath + "/" + collection + "/shards";

    auto myshards = snapshot(planPath).children();
    auto steps = std::distance(myshards.begin(), myshards.find(shard));

    for (const auto& colptr : snapshot(databasePath).children()) { // collections

      auto const col = *colptr.second;
      auto const otherCollection = colptr.first;

      try {
        std::string const& prototype =
          col("distributeShardsLike").slice().copyString();
        if (otherCollection != collection && prototype == collection) {
          auto othershards = col("shards").children();
          auto opos = othershards.begin();
          std::advance(opos, steps);
          auto const& otherShard = opos->first;
          ret.emplace_back(otherCollection, otherShard);
        }
      } catch(...) {}
      
    }
  } catch (...) {
    ret.clear();
    ret.emplace_back(collection, shard);
  }
  
  return ret;
}


std::string Job::findCommonInSyncFollower(
  Node const& snap, std::string const& db, std::string const& col,
  std::string const& shrd) {

  auto cs = clones(snap, db, col, shrd);
  auto nclones = cs.size();

  std::map<std::string,size_t> currentServers;
  for (const auto& clone : cs) {
    auto shardPath =
      curColPrefix + db + "/" + clone.collection + "/"
      + clone.shard + "/servers";
    size_t i = 0;
    for (const auto& server : VPackArrayIterator(snap(shardPath).getArray())) {
      if (i++ > 0) { // Skip leader
        if (++currentServers[server.copyString()] == nclones) {
          return server.copyString();
        }
      }
    }
  }

  return std::string();
  
}

std::string Job::uuidLookup (std::string const& shortID) {
  for (auto const& uuid : _snapshot(mapUniqueToShortID).children()) {
    if ((*uuid.second)("ShortName").getString() == shortID) {
      return uuid.first;
    }
  }
  return std::string();
}


std::string Job::id(std::string const& idOrShortName) {
  std::string id = uuidLookup(idOrShortName);
  if (!id.empty()) {
    return id;
  }
  return idOrShortName;
}

bool Job::abortable(Node const& snapshot, std::string const& jobId) {

  auto const& job = snapshot(blockedServersPrefix + jobId);
  auto const& type = job("type").getString();

  if (type == "failedServer" || type == "failedLeader") {
    return false;
  } else if (type == "addFollower" || type == "moveShard" ||
             type == "cleanOutServer") {
    return true;
  }

  // We should never get here
  TRI_ASSERT(false);
  
}

void Job::doForAllShards(Node const& snapshot,
	std::string& database,
	std::vector<shard_t>& shards,
  std::function<void(Slice plan, Slice current, std::string& planPath)> worker) {
	for (auto const& collShard : shards) {
		std::string shard = collShard.shard;
		std::string collection = collShard.collection;

    std::string planPath =
      planColPrefix + database + "/" + collection + "/shards/" + shard;
    std::string curPath = curColPrefix + database + "/" + collection
                          + "/" + shard + "/servers";

		Slice plan = snapshot(planPath).slice();
		Slice current = snapshot(curPath).slice();

    worker(plan, current, planPath);
  }
}

void Job::addIncreasePlanVersion(Builder& trx) {
  trx.add(VPackValue(planVersion));
  { VPackObjectBuilder guard(&trx);
    trx.add("op", VPackValue("increment"));
  }
}

void Job::addRemoveJobFromSomewhere(Builder& trx, std::string where,
  std::string jobId) {
  trx.add(VPackValue("/Target/" + where + "/" + jobId));
  { VPackObjectBuilder guard(&trx);
    trx.add("op", VPackValue("delete"));
  }
}

void Job::addPutJobIntoSomewhere(Builder& trx, std::string where, Slice job,
    std::string reason) {
  Slice jobIdSlice = job.get("jobId");
  TRI_ASSERT(jobIdSlice.isString());
  std::string jobId = jobIdSlice.copyString();
  trx.add(VPackValue("/Target/" + where + "/" + jobId));
  { VPackObjectBuilder guard(&trx);
    if (where == "Pending") {
      trx.add("timeStarted",
        VPackValue(timepointToString(std::chrono::system_clock::now())));
    } else {
      trx.add("timeFinished",
        VPackValue(timepointToString(std::chrono::system_clock::now())));
    }
    for (auto const& obj : VPackObjectIterator(job)) {
      trx.add(obj.key.copyString(), obj.value);
    }
    if (!reason.empty()) {
      trx.add("reason", VPackValue(reason));
    }
  }
}

void Job::addPreconditionCollectionStillThere(Builder& pre,
    std::string database, std::string collection) {
  std::string planPath
      = planColPrefix + database + "/" + collection;
  pre.add(VPackValue(planPath));
  { VPackObjectBuilder guard(&pre);
    pre.add("oldEmpty", VPackValue(false));
  }
}

void Job::addPreconditionServerNotBlocked(Builder& pre, std::string server) {
	pre.add(VPackValue(blockedServersPrefix + server));
	{ VPackObjectBuilder serverLockEmpty(&pre);
		pre.add("oldEmpty", VPackValue(true));
	}
}

void Job::addPreconditionServerGood(Builder& pre, std::string server) {
	pre.add(VPackValue(healthPrefix + server + "/Status"));
	{ VPackObjectBuilder serverGood(&pre);
		pre.add("old", VPackValue("GOOD"));
	}
}

void Job::addPreconditionShardNotBlocked(Builder& pre, std::string shard) {
	pre.add(VPackValue(blockedShardsPrefix + shard));
	{ VPackObjectBuilder shardLockEmpty(&pre);
		pre.add("oldEmpty", VPackValue(true));
	}
}

void Job::addPreconditionUnchanged(Builder& pre,
    std::string key, Slice value) {
  pre.add(VPackValue(key));
  { VPackObjectBuilder guard(&pre);
    pre.add("old", value);
  }
}

void Job::addBlockServer(Builder& trx, std::string server, std::string jobId) {
  trx.add(blockedServersPrefix + server, VPackValue(jobId));
}

void Job::addBlockShard(Builder& trx, std::string shard, std::string jobId) {
  trx.add(blockedShardsPrefix + shard, VPackValue(jobId));
}

void Job::addReleaseServer(Builder& trx, std::string server) {
  trx.add(VPackValue(blockedServersPrefix + server));
  { VPackObjectBuilder guard(&trx);
    trx.add("op", VPackValue("delete"));
  }
}

void Job::addReleaseShard(Builder& trx, std::string shard) {
  trx.add(VPackValue(blockedShardsPrefix + shard));
  { VPackObjectBuilder guard(&trx);
    trx.add("op", VPackValue("delete"));
  }
}

std::string Job::checkServerGood(Node const& snapshot,
                                 std::string const& server) {
  if (!snapshot.has(healthPrefix + server + "/Status")) {
    return "UNCLEAR";
  }
  if (snapshot(healthPrefix + server + "/Status").getString() != "GOOD") {
    return "UNHEALTHY";
  }
  return "GOOD";
}

