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

#include "v8-vocindex.h"
#include "Basics/ReadLocker.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/conversions.h"
#include "Basics/tri-strings.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterMethods.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Indexes/Index.h"
#include "Indexes/IndexFactory.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
#include "Transaction/Hints.h"
#include "Transaction/V8Context.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "V8/v8-conv.h"
#include "V8/v8-globals.h"
#include "V8/v8-utils.h"
#include "V8/v8-vpack.h"
#include "V8Server/v8-collection.h"
#include "V8Server/v8-externals.h"
#include "V8Server/v8-vocbase.h"
#include "V8Server/v8-vocbaseprivate.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Indexes.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures an index
////////////////////////////////////////////////////////////////////////////////

static void EnsureIndex(v8::FunctionCallbackInfo<v8::Value> const& args,
                        bool create, char const* functionName) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  arangodb::LogicalCollection* collection =
      TRI_UnwrapClass<arangodb::LogicalCollection>(args.Holder(),
                                                   WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() != 1 || !args[0]->IsObject()) {
    std::string name(functionName);
    name.append("(<description>)");
    TRI_V8_THROW_EXCEPTION_USAGE(name.c_str());
  }

  VPackBuilder builder;
  TRI_V8ToVPackSimple(isolate, builder, args[0]);

  VPackBuilder output;
  Result res = methods::Indexes::ensureIndex(collection, builder.slice(),
                                             create, output);
  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }
  v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, output.slice());
  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionEnsureIndex
////////////////////////////////////////////////////////////////////////////////

static void JS_EnsureIndexVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  PREVENT_EMBEDDED_TRANSACTION();

  EnsureIndex(args, true, "ensureIndex");
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up an index
////////////////////////////////////////////////////////////////////////////////

static void JS_LookupIndexVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  EnsureIndex(args, false, "lookupIndex");
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock col_dropIndex
////////////////////////////////////////////////////////////////////////////////

static void JS_DropIndexVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  PREVENT_EMBEDDED_TRANSACTION();

  arangodb::LogicalCollection* collection =
      TRI_UnwrapClass<arangodb::LogicalCollection>(args.Holder(),
                                                   WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("dropIndex(<index-handle>)");
  }

  VPackBuilder builder;
  TRI_V8ToVPackSimple(isolate, builder, args[0]);

  Result res = methods::Indexes::drop(collection, builder.slice());
  if (res.ok()) {
    TRI_V8_RETURN_TRUE();
  }
  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionGetIndexes
////////////////////////////////////////////////////////////////////////////////

static void JS_GetIndexesVocbaseCol(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  arangodb::LogicalCollection* collection =
      TRI_UnwrapClass<arangodb::LogicalCollection>(args.Holder(),
                                                   WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  bool withFigures = false;
  if (args.Length() > 0) {
    withFigures = TRI_ObjectToBoolean(args[0]);
  }

  VPackBuilder output;
  Result res = methods::Indexes::getAll(collection, withFigures, output);
  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, output.slice());
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a collection
////////////////////////////////////////////////////////////////////////////////

static void CreateVocBase(v8::FunctionCallbackInfo<v8::Value> const& args,
                          TRI_col_type_e collectionType) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_vocbase_t* vocbase = GetContextVocBase(isolate);
  if (vocbase == nullptr || vocbase->isDangling()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  } else if (args.Length() < 1 || args.Length() > 4) {
    TRI_V8_THROW_EXCEPTION_USAGE("_create(<name>, <properties>, <type>, <options>)");
  }

  if (ExecContext::CURRENT != nullptr &&
      !ExecContext::CURRENT->canUseDatabase(vocbase->name(), auth::Level::RW)) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_FORBIDDEN);
  }

  // optional, third parameter can override collection type
  if (args.Length() >= 3 && args[2]->IsString()) {
    std::string typeString = TRI_ObjectToString(args[2]);
    if (typeString == "edge") {
      collectionType = TRI_COL_TYPE_EDGE;
    } else if (typeString == "document") {
      collectionType = TRI_COL_TYPE_DOCUMENT;
    }
  }

  PREVENT_EMBEDDED_TRANSACTION();

  // extract the name
  std::string const name = TRI_ObjectToString(args[0]);

  VPackBuilder properties;
  VPackSlice propSlice = VPackSlice::emptyObjectSlice();
  if (args.Length() >= 2) {
    if (!args[1]->IsObject()) {
      TRI_V8_THROW_TYPE_ERROR("<properties> must be an object");
    }
    int res = TRI_V8ToVPack(isolate, properties, args[1]->ToObject(), false);
    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }
    propSlice = properties.slice();
  }

  // waitForSync can be 3. or 4. parameter
  auto cluster = application_features::ApplicationServer::getFeature<ClusterFeature>("Cluster");
  bool createWaitsForSyncReplication = cluster->createWaitsForSyncReplication();
  bool enforceReplicationFactor = true;

  if (args.Length() >= 3 && args[args.Length() - 1]->IsObject()) {
    v8::Handle<v8::Object> obj = args[args.Length() - 1]->ToObject();
    createWaitsForSyncReplication = TRI_GetOptionalBooleanProperty(isolate,
      obj, "waitForSyncReplication", createWaitsForSyncReplication);

    enforceReplicationFactor = TRI_GetOptionalBooleanProperty(isolate,
      obj, "enforceReplicationFactor", enforceReplicationFactor);
  }

  v8::Handle<v8::Value> result;
  Result res = methods::Collections::create(vocbase, name, collectionType,
                                            propSlice,
                                            createWaitsForSyncReplication,
                                            enforceReplicationFactor,
                                            [&isolate, &result](LogicalCollection* coll) {
                                              if (ServerState::instance()->isCoordinator()) {
                                                std::unique_ptr<LogicalCollection> cc = coll->clone();
                                                result = WrapCollection(isolate, cc.get());
                                                cc.release();
                                              } else {
                                                result = WrapCollection(isolate, coll);
                                              }
                                            });
  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionDatabaseCreate
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateVocbase(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  CreateVocBase(args, TRI_COL_TYPE_DOCUMENT);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionCreateDocumentCollection
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateDocumentCollectionVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  CreateVocBase(args, TRI_COL_TYPE_DOCUMENT);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionCreateEdgeCollection
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateEdgeCollectionVocbase(
    v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  CreateVocBase(args, TRI_COL_TYPE_EDGE);
  TRI_V8_TRY_CATCH_END
}

void TRI_InitV8IndexArangoDB(v8::Isolate* isolate,
                             v8::Handle<v8::ObjectTemplate> rt) {
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "_create"),
                       JS_CreateVocbase, true);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING(isolate, "_createEdgeCollection"),
                       JS_CreateEdgeCollectionVocbase);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING(isolate, "_createDocumentCollection"),
                       JS_CreateDocumentCollectionVocbase);
}

void TRI_InitV8IndexCollection(v8::Isolate* isolate,
                               v8::Handle<v8::ObjectTemplate> rt) {
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "dropIndex"),
                       JS_DropIndexVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "ensureIndex"),
                       JS_EnsureIndexVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "lookupIndex"),
                       JS_LookupIndexVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING(isolate, "getIndexes"),
                       JS_GetIndexesVocbaseCol);
}
