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

#include "EngineInfoContainer.h"

#include "Aql/ClusterBlocks.h"
#include "Aql/ClusterNodes.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"
#include "Aql/IndexNode.h"
#include "Aql/ModificationNodes.h"
#include "Aql/Query.h"
#include "Aql/QueryRegistry.h"
#include "Cluster/ClusterComm.h"
#include "Cluster/ServerState.h"
#include "Logger/Logger.h"
#include "VocBase/ticks.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

// -----------------------------------------------------------------------------
// --SECTION--                                             Coordinator Container
// -----------------------------------------------------------------------------

EngineInfoContainerCoordinator::EngineInfo::EngineInfo(
    size_t id, std::vector<ExecutionNode*>& nodes, size_t idOfRemoteNode)
    : _id(id), _nodes(nodes), _idOfRemoteNode(idOfRemoteNode) {
  TRI_ASSERT(!_nodes.empty());
  TRI_ASSERT(!nodes.empty());
}

EngineInfoContainerCoordinator::EngineInfo::~EngineInfo() {
  // This container is not responsible for nodes, they are managed by the AST
  // somewhere else
}

EngineInfoContainerCoordinator::EngineInfoContainerCoordinator() {}

EngineInfoContainerCoordinator::~EngineInfoContainerCoordinator() {}

EngineInfoContainerCoordinator::EngineInfo::EngineInfo(EngineInfo const&& other)
    : _id(other._id),
      _nodes(std::move(other._nodes)),
      _idOfRemoteNode(other._idOfRemoteNode) {
  TRI_ASSERT(!_nodes.empty());
}

ExecutionEngine* EngineInfoContainerCoordinator::EngineInfo::buildEngine(
    Query* query, QueryRegistry* queryRegistry,
    std::unordered_map<std::string, std::string>& queryIds) const {
  auto engine = std::make_unique<ExecutionEngine>(query);
  query->engine(engine.get());

  auto clusterInfo = arangodb::ClusterInfo::instance();

  std::unordered_map<ExecutionNode*, ExecutionBlock*> cache;
  RemoteNode* remoteNode = nullptr;

  for (auto const& en : _nodes) {
    auto const nodeType = en->getType();

    if (nodeType == ExecutionNode::REMOTE) {
      remoteNode = static_cast<RemoteNode*>(en);
      continue;
    }

    // for all node types but REMOTEs, we create blocks
    ExecutionBlock* eb =
        // ExecutionEngine::CreateBlock(engine.get(), en, cache,
        // _includedShards);
        ExecutionEngine::CreateBlock(engine.get(), en, cache, {});

    if (eb == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "illegal node type");
    }

    try {
      engine.get()->addBlock(eb);
    } catch (...) {
      delete eb;
      throw;
    }

    for (auto const& dep : en->getDependencies()) {
      auto d = cache.find(dep);

      if (d != cache.end()) {
        // add regular dependencies
        TRI_ASSERT((*d).second != nullptr);
        eb->addDependency((*d).second);
      }
    }

    if (nodeType == ExecutionNode::GATHER) {
      // we found a gather node
      if (remoteNode == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "expecting a remoteNode");
      }

      // now we'll create a remote node for each shard and add it to the
      // gather node
      auto gatherNode = static_cast<GatherNode const*>(en);
      Collection const* collection = gatherNode->collection();

      // auto shardIds = collection->shardIds(_includedShards);
      auto shardIds = collection->shardIds();
      for (auto const& shardId : *shardIds) {
        std::string theId =
            arangodb::basics::StringUtils::itoa(remoteNode->id()) + ":" +
            shardId;

        auto it = queryIds.find(theId);
        if (it == queryIds.end()) {
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                         "could not find query id in list");
        }
        std::string idThere = it->second;
        if (idThere.back() == '*') {
          idThere.pop_back();
        }

        auto serverList = clusterInfo->getResponsibleServer(shardId);
        if (serverList->empty()) {
          THROW_ARANGO_EXCEPTION_MESSAGE(
              TRI_ERROR_CLUSTER_BACKEND_UNAVAILABLE,
              "Could not find responsible server for shard " + shardId);
        }

        // use "server:" instead of "shard:" to send query fragments to
        // the correct servers, even after failover or when a follower drops
        // the problem with using the previous shard-based approach was that
        // responsibilities for shards may change at runtime.
        // however, an AQL query must send all requests for the query to the
        // initially used servers.
        // if there is a failover while the query is executing, we must still
        // send all following requests to the same servers, and not the newly
        // responsible servers.
        // otherwise we potentially would try to get data from a query from
        // server B while the query was only instanciated on server A.
        TRI_ASSERT(!serverList->empty());
        auto& leader = (*serverList)[0];
        ExecutionBlock* r = new RemoteBlock(engine.get(), remoteNode,
                                            "server:" + leader,  // server
                                            "",                  // ownName
                                            idThere);            // queryId

        try {
          engine.get()->addBlock(r);
        } catch (...) {
          delete r;
          throw;
        }

        TRI_ASSERT(r != nullptr);
        eb->addDependency(r);
      }
    }

    // the last block is always the root
    engine->root(eb);

    // put it into our cache:
    cache.emplace(en, eb);
  }

  TRI_ASSERT(engine->root() != nullptr);

  LOG_TOPIC(ERR, arangodb::Logger::AQL) << "Storing Coordinator engine: "
                                        << _id;
  try {
    queryRegistry->insert(_id, engine->getQuery(), 600.0);
  } catch (...) {
    // TODO Is this correct or does it cause failures?
    // TODO Add failure tests
    delete engine->getQuery();
    // This deletes the new query as well as the engine
    throw;
  }
  try {
    std::string queryId = arangodb::basics::StringUtils::itoa(_id);
    std::string theID = arangodb::basics::StringUtils::itoa(_idOfRemoteNode) +
                        "/" + engine->getQuery()->vocbase()->name();
    queryIds.emplace(theID, queryId);
  } catch (...) {
    queryRegistry->destroy(engine->getQuery()->vocbase(), _id,
                           TRI_ERROR_INTERNAL);
    // This deletes query, engine and entry in QueryRegistry
    throw;
  }
  
  return engine.release();
}

QueryId EngineInfoContainerCoordinator::addQuerySnippet(
    std::vector<ExecutionNode*>& nodes, size_t idOfRemoteNode) {
  // TODO: Check if the following is true:
  // idOfRemote === 0 => id === 0
  QueryId id = TRI_NewTickServer();
  _engines.emplace_back(id, nodes, idOfRemoteNode);
  return id;
}

ExecutionEngine* EngineInfoContainerCoordinator::buildEngines(
    Query* query, QueryRegistry* registry,
    std::unordered_map<std::string, std::string>& queryIds) const {
  bool first = true;
  std::unique_ptr<ExecutionEngine> result;
  Query* localQuery = query;

  for (auto const& info : _engines) {
    if (!first) {
      // need a new query instance on the coordinator
      localQuery = query->clone(PART_DEPENDENT, false);
      if (localQuery == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "cannot clone query");
      }
    }
    try {
      info.buildEngine(localQuery, registry, queryIds);
    } catch (...) {
      localQuery->engine(nullptr);  // engine is already destroyed internally
      if (!first) {
        delete localQuery;
      }
      throw;
    }
    first = false;
  }

  return result.release();
}

// -----------------------------------------------------------------------------
// --SECTION--                                                DBServer Container
// -----------------------------------------------------------------------------

EngineInfoContainerDBServer::EngineInfo::EngineInfo(
    std::vector<ExecutionNode*>& nodes, size_t idOfRemoteNode)
    : _nodes(nodes), _idOfRemoteNode(idOfRemoteNode), _otherId(0) {
  TRI_ASSERT(!_nodes.empty());
  LOG_TOPIC(DEBUG, Logger::AQL) << "Create DBServer Engine";
}

EngineInfoContainerDBServer::EngineInfo::~EngineInfo() {
  TRI_ASSERT(!_nodes.empty());
  LOG_TOPIC(DEBUG, Logger::AQL) << "Destroying DBServer Engine";
  // This container is not responsible for nodes
  // they are managed by the AST somewhere else
}

EngineInfoContainerDBServer::EngineInfo::EngineInfo(EngineInfo const&& other)
    : _nodes(std::move(other._nodes)),
      _idOfRemoteNode(other._idOfRemoteNode),
      _otherId(other._otherId) {
  TRI_ASSERT(!_nodes.empty());
}

void EngineInfoContainerDBServer::EngineInfo::connectQueryId(QueryId id) {
  _otherId = id;
}

void EngineInfoContainerDBServer::EngineInfo::serializeSnippet(
    ShardID id, VPackBuilder& infoBuilder) const {
  // The Key is required to build up the queryId mapping later
  infoBuilder.add(VPackValue(_idOfRemoteNode + ":" + id));
  TRI_ASSERT(!_nodes.empty());
  // TODO: Well do we need to clone?!
  auto last = _nodes.back();
  // Only the LAST node can be a REMOTE node.
  // Inject the Shard. And Start Velocypack from there
  if (last->getType() == ExecutionNode::REMOTE) {
    auto rem = static_cast<RemoteNode*>(last);
    rem->server("server:" + arangodb::ServerState::instance()->getId());
    rem->ownName(id);
    rem->queryId(_otherId);
    // Do we need this still?
    rem->isResponsibleForInitializeCursor(false);
  }
  // Always Verbose
  last->toVelocyPack(infoBuilder, true);
}

EngineInfoContainerDBServer::EngineInfoContainerDBServer()
    : _lastEngine(nullptr) {}

EngineInfoContainerDBServer::~EngineInfoContainerDBServer() {}

void EngineInfoContainerDBServer::connectLastSnippet(QueryId id) {
  if (_lastEngine == nullptr) {
    // If we do not have engines we cannot append the snippet.
    // This is the case for the initial coordinator snippet.
    return;
  }
  _lastEngine->connectQueryId(id);
}

void EngineInfoContainerDBServer::addQuerySnippet(
    std::vector<ExecutionNode*>& nodes, size_t idOfRemoteNode) {
  if (nodes.empty()) {
    // How can this happen?
    return;
  }
  Collection const* collection = nullptr;
  auto handleCollection = [&](Collection const* col, bool isWrite) -> void {
    auto it = _collections.find(col);
    if (it == _collections.end()) {
      _collections.emplace(
          col, (isWrite ? AccessMode::Type::WRITE : AccessMode::Type::READ));
    } else {
      if (isWrite && it->second == AccessMode::Type::READ) {
        // We need to upgrade the lock
        it->second = AccessMode::Type::WRITE;
      }
    }
    if (collection != nullptr && collection->isSatellite()) {
      _satellites.emplace(collection);
    }
    collection = col;
  };

  // Analyse the collections used in this Query.
  for (auto en : nodes) {
    switch (en->getType()) {
      case ExecutionNode::ENUMERATE_COLLECTION:
        handleCollection(
            static_cast<EnumerateCollectionNode*>(en)->collection(), false);
        break;
      case ExecutionNode::INDEX:
        handleCollection(static_cast<IndexNode*>(en)->collection(), false);
        break;
      case ExecutionNode::INSERT:
      case ExecutionNode::UPDATE:
      case ExecutionNode::REMOVE:
      case ExecutionNode::REPLACE:
      case ExecutionNode::UPSERT:
        handleCollection(static_cast<ModificationNode*>(en)->collection(),
                         true);
        break;
      default:
        // Do nothing
        break;
    };
  }

  _engines[collection].emplace_back(nodes, idOfRemoteNode);
  _lastEngine = &_engines[collection].back();  // The new engine
}

EngineInfoContainerDBServer::DBServerInfo::DBServerInfo() {}
EngineInfoContainerDBServer::DBServerInfo::~DBServerInfo() {}

void EngineInfoContainerDBServer::DBServerInfo::addShardLock(
    AccessMode::Type const& lock, ShardID const& id) {
  _shardLocking[lock].emplace_back(id);
}

void EngineInfoContainerDBServer::DBServerInfo::addEngine(
    EngineInfoContainerDBServer::EngineInfo const* info, ShardID const& id) {
  _engineInfos[info].emplace_back(id);
}

void EngineInfoContainerDBServer::DBServerInfo::buildMessage(
    Query* query, VPackBuilder& infoBuilder) const {
  TRI_ASSERT(infoBuilder.isEmpty());

  infoBuilder.openObject();
  infoBuilder.add(VPackValue("lockInfo"));
  infoBuilder.openObject();
  for (auto const& shardLocks : _shardLocking) {
    switch (shardLocks.first) {
      case AccessMode::Type::READ:
        infoBuilder.add(VPackValue("READ"));
        break;
      case AccessMode::Type::WRITE:
        infoBuilder.add(VPackValue("WRITE"));
        break;
      default:
        // We only have Read and Write Locks in Cluster.
        // NONE or EXCLUSIVE is impossible
        TRI_ASSERT(false);
        continue;
    }

    infoBuilder.openArray();
    for (auto const& s : shardLocks.second) {
      infoBuilder.add(VPackValue(s));
    }
    infoBuilder.close();  // The array
  }
  infoBuilder.close();  // lockInfo
  infoBuilder.add(VPackValue("options"));
  injectQueryOptions(query, infoBuilder);
  infoBuilder.add(VPackValue("variables"));
  // This will open and close an Object.
  query->ast()->variables()->toVelocyPack(infoBuilder);
  infoBuilder.add(VPackValue("snippets"));
  infoBuilder.openObject();

  for (auto const& it : _engineInfos) {
    for (auto const& s : it.second) {
      it.first->serializeSnippet(s, infoBuilder);
    }
  }
  infoBuilder.close();  // snippets
  infoBuilder.close();  // Object
}

void EngineInfoContainerDBServer::DBServerInfo::injectQueryOptions(
    Query* query, VPackBuilder& infoBuilder) const {
  // the toVelocyPack will open & close the "options" object
  query->queryOptions().toVelocyPack(infoBuilder, true);
}

void EngineInfoContainerDBServer::buildEngines(Query* query, std::unordered_map<std::string, std::string>& queryIds) const {
  LOG_TOPIC(DEBUG, arangodb::Logger::AQL) << "We have " << _engines.size()
                                          << " DBServer engines";
  std::map<ServerID, DBServerInfo> dbServerMapping;

  auto ci = ClusterInfo::instance();

  for (auto const& it : _collections) {
    // it.first => Collection const*
    // it.second => Lock Type
    std::vector<EngineInfo> const* engines = nullptr;
    if (_engines.find(it.first) != _engines.end()) {
      engines = &_engines.find(it.first)->second;
    }
    auto shardIds = it.first->shardIds();
    for (auto const& s : *(shardIds.get())) {
      auto const servers = ci->getResponsibleServer(s);
      if (servers == nullptr || servers->empty()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_CLUSTER_BACKEND_UNAVAILABLE,
            "Could not find responsible server for shard " + s);
      }
      auto responsible = servers->at(0);
      auto& mapping = dbServerMapping[responsible];
      mapping.addShardLock(it.second, s);
      if (engines != nullptr) {
        for (auto& e : *engines) {
          mapping.addEngine(&e, s);
        }
      }
    }
  }

  auto cc = ClusterComm::instance();

  if (cc == nullptr) {
    // nullptr only happens on controlled shutdown
    return;
  }

  // TODO FIXME
  std::string const url("/_db/" + arangodb::basics::StringUtils::urlEncode(
                                      query->vocbase()->name()) +
                        "/_internal/traverser");

  std::unordered_map<std::string, std::string> headers;
  // Build Lookup Infos
  VPackBuilder infoBuilder;
  for (auto const& it : dbServerMapping) {
    LOG_TOPIC(DEBUG, arangodb::Logger::AQL) << "Building Engine Info for "
                                            << it.first;
    infoBuilder.clear();
    it.second.buildMessage(query, infoBuilder);
    LOG_TOPIC(DEBUG, arangodb::Logger::AQL) << infoBuilder.toJson();


    // Now we send to DBServers. We expect a body with {id => engineId} plus 0 => trxEngine
    CoordTransactionID coordTransactionID = TRI_NewTickServer();
    auto res = cc->syncRequest("", coordTransactionID, "server:" + it.first,
                               RequestType::POST, url, infoBuilder.toJson(),
                               headers, 90.0);

    if (res->getErrorCode() != TRI_ERROR_NO_ERROR) {
      // TODO could not register all engines. Need to cleanup.
      THROW_ARANGO_EXCEPTION_MESSAGE(res->getErrorCode(), res->stringifyErrorMessage());
    }

    std::shared_ptr<VPackBuilder> builder = res->result->getBodyVelocyPack();
    VPackSlice response = builder->slice();
    if (!response.isObject()) {
      // TODO could not register all engines. Need to cleanup.
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_AQL_COMMUNICATION, "Unable to deploy query on all required servers. This can happen during Failover. Please check: " + it.first);
    }

    for (auto const& resEntry: VPackObjectIterator(response)) {
      if (!resEntry.value.isString()) {
        // TODO could not register all engines. Need to cleanup.
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_AQL_COMMUNICATION, "Unable to deploy query on all required servers. This can happen during Failover. Please check: " + it.first);
      }
      queryIds.emplace(resEntry.key.copyString(), resEntry.value.copyString());
    }

  }

}