//////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 EMC Corporation
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
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"
#include "common.h"
#include "StorageEngineMock.h"
#include "ExpressionContextMock.h"

#include "analysis/token_attributes.hpp"
#include "search/scorers.hpp"
#include "utils/locale_utils.hpp"
#include "utils/log.hpp"
#include "utils/utf8_path.hpp"

#include "ApplicationFeatures/JemallocFeature.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/AstNode.h"
#include "Aql/Function.h"
#include "Aql/SortCondition.h"
#include "Basics/ArangoGlobalContext.h"
#include "Basics/files.h"

#if USE_ENTERPRISE
  #include "Enterprise/Ldap/LdapFeature.h"
#endif

#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/ApplicationServerHelper.h"
#include "IResearch/IResearchDocument.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchLinkMeta.h"
#include "IResearch/IResearchMMFilesLink.h"
#include "IResearch/IResearchView.h"
#include "IResearch/SystemDatabaseFeature.h"
#include "Logger/Logger.h"
#include "Logger/LogTopic.h"
#include "Random/RandomFeature.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/TraverserEngineRegistryFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/FlushFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/UserTransaction.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "velocypack/Iterator.h"
#include "velocypack/Parser.h"
#include "V8Server/V8DealerFeature.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ManagedDocumentResult.h"

NS_LOCAL

struct DocIdScorer: public irs::sort {
  DECLARE_SORT_TYPE() { static irs::sort::type_id type("test_doc_id"); return type; }
  static ptr make(const irs::string_ref&) { PTR_NAMED(DocIdScorer, ptr); return ptr; }
  DocIdScorer(): irs::sort(DocIdScorer::type()) { }
  virtual sort::prepared::ptr prepare() const override { PTR_NAMED(Prepared, ptr); return ptr; }

  struct Prepared: public irs::sort::prepared_base<uint64_t> {
    virtual void add(score_t& dst, const score_t& src) const override { dst = src; }
    virtual irs::flags const& features() const override { return irs::flags::empty_instance(); }
    virtual bool less(const score_t& lhs, const score_t& rhs) const override { return lhs < rhs; }
    virtual irs::sort::collector::ptr prepare_collector() const override { return nullptr; }
    virtual void prepare_score(score_t& score) const override { }
    virtual irs::sort::scorer::ptr prepare_scorer(
      irs::sub_reader const& segment,
      irs::term_reader const& field,
      irs::attribute_store const& query_attrs,
      irs::attribute_view const& doc_attrs
    ) const override {
      return irs::sort::scorer::make<Scorer>(doc_attrs.get<irs::document>());
    }
  };

  struct Scorer: public irs::sort::scorer {
    irs::attribute_view::ref<irs::document>::type const& _doc;
    Scorer(irs::attribute_view::ref<irs::document>::type const& doc): _doc(doc) { }
    virtual void score(irs::byte_type* score_buf) override {
      reinterpret_cast<uint64_t&>(*score_buf) = _doc.get()->value;
    }
  };
};

REGISTER_SCORER_TEXT(DocIdScorer, DocIdScorer::make);

typedef std::unique_ptr<arangodb::TransactionState> TrxStatePtr;

NS_END

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

struct IResearchViewSetup {
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;
  std::unique_ptr<TRI_vocbase_t> system;
  std::vector<std::pair<arangodb::application_features::ApplicationFeature*, bool>> features;
  std::string testFilesystemPath;

  IResearchViewSetup(): server(nullptr, nullptr) {
    arangodb::EngineSelectorFeature::ENGINE = &engine;

    arangodb::tests::init();

    // suppress INFO {authentication} Authentication is turned on (system only), authentication for unix sockets is turned on
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::WARN);

    // setup required application features
    features.emplace_back(new arangodb::V8DealerFeature(&server), false);
    features.emplace_back(new arangodb::ViewTypesFeature(&server), true);
    features.emplace_back(new arangodb::QueryRegistryFeature(&server), false);
    arangodb::application_features::ApplicationServer::server->addFeature(features.back().first);
    system = irs::memory::make_unique<TRI_vocbase_t>(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 0, TRI_VOC_SYSTEM_DATABASE);
    features.emplace_back(new arangodb::RandomFeature(&server), false); // required by AuthenticationFeature
    features.emplace_back(new arangodb::AuthenticationFeature(&server), true);
    features.emplace_back(new arangodb::DatabaseFeature(&server), false);
    features.emplace_back(new arangodb::DatabasePathFeature(&server), false);
    features.emplace_back(new arangodb::JemallocFeature(&server), false); // required for DatabasePathFeature
    features.emplace_back(new arangodb::TraverserEngineRegistryFeature(&server), false); // must be before AqlFeature
    features.emplace_back(new arangodb::AqlFeature(&server), true);
    features.emplace_back(new arangodb::aql::AqlFunctionFeature(&server), true); // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::iresearch::IResearchAnalyzerFeature(&server), true);
    features.emplace_back(new arangodb::iresearch::IResearchFeature(&server), true);
    features.emplace_back(new arangodb::iresearch::SystemDatabaseFeature(&server, system.get()), false); // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::FlushFeature(&server), false); // do not start the thread

    #if USE_ENTERPRISE
      features.emplace_back(new arangodb::LdapFeature(&server), false); // required for AuthenticationFeature with USE_ENTERPRISE
    #endif

    for (auto& f : features) {
      arangodb::application_features::ApplicationServer::server->addFeature(f.first);
    }

    for (auto& f : features) {
      f.first->prepare();
    }

    for (auto& f : features) {
      if (f.second) {
        f.first->start();
      }
    }

    TransactionStateMock::abortTransactionCount = 0;
    TransactionStateMock::beginTransactionCount = 0;
    TransactionStateMock::commitTransactionCount = 0;
    testFilesystemPath = (
      (irs::utf8_path()/=
      TRI_GetTempPath())/=
      (std::string("arangodb_tests.") + std::to_string(TRI_microtime()))
    ).utf8();
    auto* dbPathFeature = arangodb::application_features::ApplicationServer::getFeature<arangodb::DatabasePathFeature>("DatabasePath");
    const_cast<std::string&>(dbPathFeature->directory()) = testFilesystemPath;

    long systemError;
    std::string systemErrorStr;
    TRI_CreateDirectory(testFilesystemPath.c_str(), systemError, systemErrorStr);

    // suppress log messages since tests check error conditions
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::IResearchFeature::IRESEARCH.name(), arangodb::LogLevel::FATAL);
    irs::logger::output_le(iresearch::logger::IRL_FATAL, stderr);
  }

  ~IResearchViewSetup() {
    system.reset(); // destroy before reseting the 'ENGINE'
    TRI_RemoveDirectory(testFilesystemPath.c_str());
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::IResearchFeature::IRESEARCH.name(), arangodb::LogLevel::DEFAULT);
    arangodb::application_features::ApplicationServer::server = nullptr;
    arangodb::EngineSelectorFeature::ENGINE = nullptr;

    // destroy application features
    for (auto& f : features) {
      if (f.second) {
        f.first->stop();
      }
    }

    for (auto& f : features) {
      f.first->unprepare();
    }

    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::DEFAULT);
  }
};

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief setup
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("IResearchViewTest", "[iresearch][iresearch-view]") {
  IResearchViewSetup s;
  UNUSED(s);

SECTION("test_type") {
  CHECK((arangodb::LogicalDataSource::Type::emplace(arangodb::velocypack::StringRef("arangosearch")) == arangodb::iresearch::IResearchView::type()));
}

SECTION("test_defaults") {
  auto json = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\" }");

  // existing view definition with LogicalView (for persistence)
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false);
    CHECK(nullptr != view);

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, true);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(false == slice.get("deleted").getBool());
    CHECK(6 == slice.length());

    auto propSlice = slice.get("properties");
    CHECK(propSlice.isObject());
    CHECK((5U == propSlice.length()));
    CHECK((!propSlice.hasKey("links"))); // for persistence so no links
    CHECK((meta.init(propSlice, error) && expectedMeta == meta));
  }

  // existing view definition with LogicalView
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false);
    CHECK((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone()); // no system properties
    CHECK(4 == slice.length());

    auto propSlice = slice.get("properties");
    CHECK(propSlice.isObject());
    CHECK((6U == propSlice.length()));
    CHECK((propSlice.hasKey("links")));
    CHECK((meta.init(propSlice, error) && expectedMeta == meta));
  }

  // new view definition with LogicalView (for persistence)
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = arangodb::iresearch::IResearchView::make(vocbase, json->slice(), true);
    CHECK((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, false, true);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(false == slice.get("deleted").getBool());
    CHECK(5 == slice.length());

    auto propSlice = slice.get("properties");
    CHECK(propSlice.isNone());
  }

  // new view definition with LogicalView
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = arangodb::iresearch::IResearchView::make(vocbase, json->slice(), true);
    CHECK((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, false, false);
    builder.close();

    auto slice = builder.slice();
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone());
    CHECK(slice.get("properties").isNone());
    CHECK(3 == slice.length());
  }

  // new view definition with links (not supported for link creation)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\", \"id\": 100 }");
    auto viewJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 101, \"properties\": { \"links\": { \"testCollection\": {} } } }");

    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    CHECK((nullptr != logicalCollection));
    CHECK((true == !vocbase.lookupView("testView")));
    CHECK((true == logicalCollection->getIndexes().empty()));
    auto logicalView = vocbase.createView(viewJson->slice(), 0);
    REQUIRE((false == !logicalView));
    std::set<TRI_voc_cid_t> cids;
    logicalView->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
    CHECK((0 == cids.size()));
    CHECK((true == logicalCollection->getIndexes().empty()));
  }
}

SECTION("test_drop") {
  std::string dataPath = (((irs::utf8_path()/=s.testFilesystemPath)/=std::string("databases"))/=std::string("arangosearch-123")).utf8();
  auto json = arangodb::velocypack::Parser::fromJson("{ \
    \"id\": 123, \
    \"name\": \"testView\", \
    \"type\": \"arangosearch\" \
  }");

  CHECK((false == TRI_IsDirectory(dataPath.c_str())));

  Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
  auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
  CHECK((nullptr != logicalCollection));
  CHECK((true == !vocbase.lookupView("testView")));
  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((false == TRI_IsDirectory(dataPath.c_str()))); // createView(...) will call open()
  auto logicalView = vocbase.createView(json->slice(), 0);
  REQUIRE((false == !logicalView));
  auto view = &logicalView;
  REQUIRE((false == !view));

  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((false == !vocbase.lookupView("testView")));
  CHECK((true == TRI_IsDirectory(dataPath.c_str())));
  CHECK((TRI_ERROR_NO_ERROR == vocbase.dropView("testView")));
  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((true == !vocbase.lookupView("testView")));
  CHECK((false == TRI_IsDirectory(dataPath.c_str())));
}

SECTION("test_drop_with_link") {
  std::string dataPath = (((irs::utf8_path()/=s.testFilesystemPath)/=std::string("databases"))/=std::string("arangosearch-123")).utf8();
  auto json = arangodb::velocypack::Parser::fromJson("{ \
    \"id\": 123, \
    \"name\": \"testView\", \
    \"type\": \"arangosearch\" \
  }");

  CHECK((false == TRI_IsDirectory(dataPath.c_str())));

  Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
  auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
  CHECK((nullptr != logicalCollection));
  CHECK((true == !vocbase.lookupView("testView")));
  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((false == TRI_IsDirectory(dataPath.c_str()))); // createView(...) will call open()
  auto logicalView = vocbase.createView(json->slice(), 0);
  REQUIRE((false == !logicalView));
  auto view = &logicalView;
  REQUIRE((false == !view));

  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((false == !vocbase.lookupView("testView")));
  CHECK((true == TRI_IsDirectory(dataPath.c_str())));

  auto links = arangodb::velocypack::Parser::fromJson("{ \
    \"links\": { \"testCollection\": {} } \
  }");

  arangodb::Result res = logicalView->updateProperties(links->slice(), true, false);
  CHECK(true == res.ok());
  CHECK((false == logicalCollection->getIndexes().empty()));

  CHECK((TRI_ERROR_NO_ERROR == vocbase.dropView("testView")));
  CHECK((true == logicalCollection->getIndexes().empty()));
  CHECK((true == !vocbase.lookupView("testView")));
  CHECK((false == TRI_IsDirectory(dataPath.c_str())));
}

SECTION("test_drop_cid") {
  static std::vector<std::string> const EMPTY;

  // cid not in list of fully indexed (view definition not updated, not persisted)
  {
    auto json = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\" }");
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false)
    );
    CHECK((false == !view));

    // fill with test data
    {
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));
      view->insert(trx, 42, arangodb::LocalDocumentId(0), doc->slice(), meta);
      CHECK((trx.commit().ok()));
      view->sync();
    }

    // query
    {
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      CHECK(1 == snapshot->live_docs_count());
    }

    // drop cid 42
    {
      view->drop(42);
      view->sync();
    }

    // query
    {
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      CHECK(0 == snapshot->live_docs_count());
    }
  }

  // cid in list of fully indexed (view definition updated+persisted)
  {
    auto json = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"collections\": [ 42 ] }");
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false)
    );
    CHECK((false == !view));

    // fill with test data
    {
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));
      view->insert(trx, 42, arangodb::LocalDocumentId(0), doc->slice(), meta);
      CHECK((trx.commit().ok()));
      view->sync();
    }

    // query
    {
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      CHECK(1 == snapshot->live_docs_count());
    }

    // drop cid 42
    {
      view->drop(42);
      view->sync();
    }

    // query
    {
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      CHECK(0 == snapshot->live_docs_count());
    }
  }
}

SECTION("test_insert") {
  static std::vector<std::string> const EMPTY;
  auto json = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\":\"arangosearch\" }");
  arangodb::aql::AstNode noop(arangodb::aql::AstNodeType::NODE_TYPE_FILTER);
  arangodb::aql::AstNode noopChild(true, arangodb::aql::AstNodeValueType::VALUE_TYPE_BOOL); // all

  noop.addMember(&noopChild);

  // in recovery (removes cid+rid before insert)
  {
    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false)
    );
    CHECK((false == !view));
    view->open();

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta))); // 2nd time
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
      CHECK((view->sync()));
    }

    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    CHECK(2 == snapshot->live_docs_count());
  }

  // in recovery batch (removes cid+rid before insert)
  {
    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false)
    );
    CHECK((false == !view));
    view->open();

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );
      std::vector<std::pair<arangodb::LocalDocumentId, arangodb::velocypack::Slice>> batch = {
        { arangodb::LocalDocumentId(1), docJson->slice() },
        { arangodb::LocalDocumentId(2), docJson->slice() },
      };

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
      CHECK((view->sync()));
    }

    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    CHECK((2 == snapshot->docs_count()));
  }

  // not in recovery
  {
    StorageEngineMock::inRecoveryResult = false;
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false)
    );
    CHECK((false == !view));

    // validate cid count
    {
      std::set<TRI_voc_cid_t> cids;
      view->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
      std::unordered_set<TRI_voc_cid_t> actual;
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      arangodb::iresearch::appendKnownCollections(actual, *snapshot);
      CHECK((actual.empty()));
    }

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta))); // 2nd time
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
      CHECK((view->sync()));
    }

    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    CHECK((4 == snapshot->docs_count()));

    // validate cid count
    {
      std::set<TRI_voc_cid_t> cids;
      view->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
      std::unordered_set<TRI_voc_cid_t> expected = { 1 };
      std::unordered_set<TRI_voc_cid_t> actual;
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      arangodb::iresearch::appendKnownCollections(actual, *snapshot);

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }
  }

  // not in recovery (with waitForSync)
  {
    StorageEngineMock::inRecoveryResult = false;
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");

    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false)
    );
    CHECK((false == !view));
    CHECK(view->category() == arangodb::LogicalView::category());

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::Options options;
      options.waitForSync = true;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, options
      );

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(1), docJson->slice(), linkMeta))); // 2nd time
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, arangodb::LocalDocumentId(2), docJson->slice(), linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
    }

    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    CHECK((4 == snapshot->docs_count()));
  }

  // not in recovery batch
  {
    StorageEngineMock::inRecoveryResult = false;
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false)
    );
    CHECK((false == !view));

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );
      std::vector<std::pair<arangodb::LocalDocumentId, arangodb::velocypack::Slice>> batch = {
        { arangodb::LocalDocumentId(1), docJson->slice() },
        { arangodb::LocalDocumentId(2), docJson->slice() },
      };

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
      CHECK((view->sync()));
    }

    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    CHECK((4 == snapshot->docs_count()));
  }

  // not in recovery batch (waitForSync)
  {
    StorageEngineMock::inRecoveryResult = false;
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false)
    );
    CHECK((false == !view));

    {
      auto docJson = arangodb::velocypack::Parser::fromJson("{\"abc\": \"def\"}");
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      arangodb::transaction::Options options;
      options.waitForSync = true;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, options
      );
      std::vector<std::pair<arangodb::LocalDocumentId, arangodb::velocypack::Slice>> batch = {
        { arangodb::LocalDocumentId(1), docJson->slice() },
        { arangodb::LocalDocumentId(2), docJson->slice() },
      };

      linkMeta._includeAllFields = true;
      CHECK((trx.begin().ok()));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta)));
      CHECK((TRI_ERROR_NO_ERROR == view->insert(trx, 1, batch, linkMeta))); // 2nd time
      CHECK((trx.commit().ok()));
    }

    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    CHECK((4 == snapshot->docs_count()));
  }
}

SECTION("test_link") {
  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\", \"id\": 100 }");
  auto viewJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\" }");

  // drop invalid collection
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto viewImpl = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !viewImpl));

    {
      std::set<TRI_voc_cid_t> cids;
      viewImpl->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
    }

    {
      CHECK((true == viewImpl->link(100, arangodb::velocypack::Slice::nullSlice()).ok()));
      std::set<TRI_voc_cid_t> cids;
      viewImpl->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
    }
  }

  // drop non-exiting
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    CHECK((nullptr != logicalCollection));
    auto viewImpl = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !viewImpl));

    {
      std::set<TRI_voc_cid_t> cids;
      viewImpl->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
    }

    {
      CHECK((true == viewImpl->link(logicalCollection->id(), arangodb::velocypack::Slice::nullSlice()).ok()));
      std::set<TRI_voc_cid_t> cids;
      viewImpl->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
    }
  }

  // drop exiting
  {
    TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    CHECK((nullptr != logicalCollection));
    auto viewImpl = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !viewImpl));

    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": {} } \
    }");
    CHECK((true == viewImpl->updateProperties(links->slice(), true, false).ok()));

    {
      std::set<TRI_voc_cid_t> cids;
      viewImpl->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((1 == cids.size()));
      CHECK((1 == logicalCollection->getIndexes().size()));
    }

    {
      CHECK((true == viewImpl->link(logicalCollection->id(), arangodb::velocypack::Slice::nullSlice()).ok()));
      std::set<TRI_voc_cid_t> cids;
      viewImpl->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
      CHECK((true == logicalCollection->getIndexes().empty()));
    }
  }

  // drop invalid collection + recreate
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto logicalView = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !logicalView));

    {
      std::set<TRI_voc_cid_t> cids;
      logicalView->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
    }

    {
      CHECK((false == logicalView->link(100, arangodb::iresearch::emptyObjectSlice()).ok()));
      std::set<TRI_voc_cid_t> cids;
      logicalView->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
    }
  }

  // drop non-existing + recreate
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    CHECK((nullptr != logicalCollection));
    auto logicalView = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !logicalView));

    {
      std::set<TRI_voc_cid_t> cids;
      logicalView->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
      CHECK((true == logicalCollection->getIndexes().empty()));
    }

    {
      CHECK((true == logicalView->link(logicalCollection->id(), arangodb::iresearch::emptyObjectSlice()).ok()));
      std::set<TRI_voc_cid_t> cids;
      logicalView->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      std::unordered_set<TRI_voc_cid_t> expected = { 100 };

      for (auto& cid: expected) {
        CHECK((1 == cids.erase(cid)));
      }

      CHECK((0 == cids.size()));
      CHECK((1 == logicalCollection->getIndexes().size()));
    }
  }

  // drop existing + recreate
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    CHECK((nullptr != logicalCollection));
    auto logicalView = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !logicalView));

    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": { \"includeAllFields\": true } } \
    }");
    CHECK((true == logicalView->updateProperties(links->slice(), true, false).ok()));

    {
      std::set<TRI_voc_cid_t> cids;
      logicalView->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((1 == cids.size()));
      CHECK((1 == logicalCollection->getIndexes().size()));
      auto link = logicalCollection->getIndexes()[0]->toVelocyPack(true, false);
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      std::string error;
      CHECK((linkMeta.init(link->slice(), error) && true == linkMeta._includeAllFields));
    }

    {
      CHECK((true == logicalView->link(logicalCollection->id(), arangodb::iresearch::emptyObjectSlice()).ok()));
      std::set<TRI_voc_cid_t> cids;
      logicalView->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      std::unordered_set<TRI_voc_cid_t> expected = { 100 };

      for (auto& cid: expected) {
        CHECK((1 == cids.erase(cid)));
      }

      CHECK((0 == cids.size()));
      CHECK((1 == logicalCollection->getIndexes().size()));
      auto link = logicalCollection->getIndexes()[0]->toVelocyPack(true, false);
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      std::string error;
      CHECK((linkMeta.init(link->slice(), error) && false == linkMeta._includeAllFields));
    }
  }

  // drop existing + recreate invalid
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    CHECK((nullptr != logicalCollection));
    auto logicalView = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !logicalView));

    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": { \"includeAllFields\": true } } \
    }");
    CHECK((true == logicalView->updateProperties(links->slice(), true, false).ok()));

    {
      std::set<TRI_voc_cid_t> cids;
      logicalView->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((1 == cids.size()));
      CHECK((1 == logicalCollection->getIndexes().size()));
      auto link = logicalCollection->getIndexes()[0]->toVelocyPack(true, false);
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      std::string error;
      CHECK((linkMeta.init(link->slice(), error) && true == linkMeta._includeAllFields));
    }

    {
      arangodb::velocypack::Builder builder;
      builder.openObject();
      builder.add("includeAllFields", arangodb::velocypack::Value("abc"));
      builder.close();
      auto slice  = builder.slice();
      CHECK((false == logicalView->link(logicalCollection->id(), slice).ok()));
      std::set<TRI_voc_cid_t> cids;
      logicalView->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      std::unordered_set<TRI_voc_cid_t> expected = { 100 };

      for (auto& cid: expected) {
        CHECK((1 == cids.erase(cid)));
      }

      CHECK((0 == cids.size()));
      CHECK((1 == logicalCollection->getIndexes().size()));
      auto link = logicalCollection->getIndexes()[0]->toVelocyPack(true, false);
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      std::string error;
      CHECK((linkMeta.init(link->slice(), error) && true == linkMeta._includeAllFields));
    }
  }
}

SECTION("test_open") {
  // default data path
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    std::string dataPath = (((irs::utf8_path()/=s.testFilesystemPath)/=std::string("databases"))/=std::string("arangosearch-123")).utf8();
    auto namedJson = arangodb::velocypack::Parser::fromJson("{ \"id\": 123, \"name\": \"testView\", \"type\": \"testType\" }");

    CHECK((false == TRI_IsDirectory(dataPath.c_str())));
    auto view = arangodb::iresearch::IResearchView::make(vocbase, namedJson->slice(), false);
    CHECK((false == !view));
    CHECK((false == TRI_IsDirectory(dataPath.c_str())));
    view->open();
    CHECK((true == TRI_IsDirectory(dataPath.c_str())));
  }
}

SECTION("test_query") {
  auto createJson = arangodb::velocypack::Parser::fromJson("{ \
    \"name\": \"testView\", \
    \"type\": \"arangosearch\" \
  }");
  static std::vector<std::string> const EMPTY;
  arangodb::aql::AstNode noop(arangodb::aql::AstNodeType::NODE_TYPE_FILTER);
  arangodb::aql::AstNode noopChild(true, arangodb::aql::AstNodeValueType::VALUE_TYPE_BOOL); // all

  noop.addMember(&noopChild);

  // no filter/order provided, means "RETURN *"
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));

    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    CHECK(0 == snapshot->docs_count());
  }

  // ordered iterator
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    CHECK((false == !view));

    // fill with test data
    {
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));

      for (size_t i = 0; i < 12; ++i) {
        view->insert(trx, 1, arangodb::LocalDocumentId(i), doc->slice(), meta);
      }

      CHECK((trx.commit().ok()));
      view->sync();
    }

    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    CHECK(12 == snapshot->docs_count());
  }

  // snapshot isolation
  {
    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": { \"includeAllFields\" : true } } \
    }");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");

    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    std::vector<std::string> collections{ logicalCollection->name() };
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    CHECK((false == !view));
    arangodb::Result res = view->updateProperties(links->slice(), true, false);
    CHECK(true == res.ok());
    CHECK((false == logicalCollection->getIndexes().empty()));

    // fill with test data
    {
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, collections, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));

      arangodb::ManagedDocumentResult inserted;
      TRI_voc_tick_t tick;
      arangodb::OperationOptions options;
      for (size_t i = 1; i <= 12; ++i) {
        auto doc = arangodb::velocypack::Parser::fromJson(std::string("{ \"key\": ") + std::to_string(i) + " }");
        logicalCollection->insert(&trx, doc->slice(), inserted, options, tick, false);
      }

      CHECK((trx.commit().ok()));
      view->sync();
    }

    TrxStatePtr state0(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot0 = view->snapshot(*state0, true);
    CHECK(12 == snapshot0->docs_count());

    // add more data
    {
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, collections, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));

      arangodb::ManagedDocumentResult inserted;
      TRI_voc_tick_t tick;
      arangodb::OperationOptions options;
      for (size_t i = 13; i <= 24; ++i) {
        auto doc = arangodb::velocypack::Parser::fromJson(std::string("{ \"key\": ") + std::to_string(i) + " }");
        logicalCollection->insert(&trx, doc->slice(), inserted, options, tick, false);
      }

      CHECK(trx.commit().ok());
      CHECK(view->sync());
    }

    // old reader sees same data as before
    CHECK(12 == snapshot0->docs_count());
    // new reader sees new data
    TrxStatePtr state1(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot1 = view->snapshot(*state1, true);
    CHECK(24 == snapshot1->docs_count());
  }

  // query while running FlushThread
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto viewCreateJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
    auto viewUpdateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": { \"includeAllFields\": true } } }");
    auto* feature = arangodb::iresearch::getFeature<arangodb::FlushFeature>("Flush");
    REQUIRE(feature);
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewCreateJson->slice(), 0)
    );
    REQUIRE((false == !view));
    arangodb::Result res = view->updateProperties(viewUpdateJson->slice(), true, false);
    REQUIRE(true == res.ok());

    // start flush thread
    auto flush = std::make_shared<std::atomic<bool>>(true);
    std::thread flushThread([feature, flush]()->void{
      while (flush->load()) {
        feature->executeCallbacks();
      }
    });
    auto flushStop = irs::make_finally([flush, &flushThread]()->void{
      flush->store(false);
      flushThread.join();
    });

    static std::vector<std::string> const EMPTY;
    arangodb::transaction::Options options;

    options.waitForSync = true;

    arangodb::aql::Variable variable("testVariable", 0);

    // test insert + query
    for (size_t i = 1; i < 200; ++i) {
      // insert
      {
        auto doc = arangodb::velocypack::Parser::fromJson(std::string("{ \"seq\": ") + std::to_string(i) + " }");
        arangodb::transaction::UserTransaction trx(
          arangodb::transaction::StandaloneContext::Create(&vocbase),
          EMPTY, EMPTY, EMPTY, options
        );

        CHECK((trx.begin().ok()));
        CHECK((trx.insert(logicalCollection->name(), doc->slice(), arangodb::OperationOptions()).ok()));
        CHECK((trx.commit().ok()));
      }

      // query
      {
        TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
        auto* snapshot = view->snapshot(*state, true);
        CHECK(i == snapshot->docs_count());
      }
    }
  }
}

SECTION("test_register_link") {
  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\", \"id\": 100 }");
  auto viewJson0 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 101 }");
  auto viewJson1 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 101, \"properties\": { \"collections\": [ 100 ] } }");
  auto linkJson  = arangodb::velocypack::Parser::fromJson("{ \"view\": 101 }");

  // new link in recovery
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson0->slice(), 0)
    );

    REQUIRE((false == !view));

    {
      arangodb::velocypack::Builder builder;
      builder.openObject();
      view->toVelocyPack(builder, false, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("id").copyString() == "101");
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      CHECK(3 == slice.length());
    }

    {
      std::set<TRI_voc_cid_t> cids;
      view->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));
    }

    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    auto link = arangodb::iresearch::IResearchMMFilesLink::make(1, logicalCollection, linkJson->slice());
    CHECK((false == !link));
    std::set<TRI_voc_cid_t> cids;
    view->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
    CHECK((0 == cids.size())); // link addition does not modify view meta
  }

  // new link
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson0->slice(), 0)
    );
    REQUIRE((false == !view));

    {
      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, false, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("id").copyString() == "101");
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      CHECK(3 == slice.length());
    }

    {
      std::unordered_set<TRI_voc_cid_t> cids;
      view->sync();
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      arangodb::iresearch::appendKnownCollections(cids, *snapshot);
      CHECK((0 == cids.size()));
    }

    {
      std::set<TRI_voc_cid_t> actual;
      view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });
      CHECK((actual.empty()));
    }

    auto link = arangodb::iresearch::IResearchMMFilesLink::make(1, logicalCollection, linkJson->slice());
    CHECK((false == !link));
    std::unordered_set<TRI_voc_cid_t> cids;
    view->sync();
    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    arangodb::iresearch::appendKnownCollections(cids, *snapshot);
    CHECK((0 == cids.size())); // link addition does trigger collection load

    {
      std::set<TRI_voc_cid_t> actual;
      view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });
      CHECK((actual.empty())); // link addition does not modify view meta
    }
  }

  // known link
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson1->slice(), 0)
    );
    REQUIRE((false == !view));

    {
      std::unordered_set<TRI_voc_cid_t> cids;
      view->sync();
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      arangodb::iresearch::appendKnownCollections(cids, *snapshot);
      CHECK((0 == cids.size()));
    }

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100, 123 };
      std::set<TRI_voc_cid_t> actual = { 123 };
      view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }

    auto link1 = arangodb::iresearch::IResearchMMFilesLink::make(1, logicalCollection, linkJson->slice());
    CHECK((false == !link1)); // duplicate link creation is allowed
    std::unordered_set<TRI_voc_cid_t> cids;
    view->sync();
    TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
    auto* snapshot = view->snapshot(*state, true);
    arangodb::iresearch::appendKnownCollections(cids, *snapshot);
    CHECK((0 == cids.size())); // link addition does trigger collection load

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100, 123 };
      std::set<TRI_voc_cid_t> actual = { 123 };
      view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }
  }
}

SECTION("test_unregister_link") {
  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\", \"id\": 100 }");
  auto viewJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 101, \"properties\": { } }");

  // link removed before view (in recovery)
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !view));

    // add a document to the view
    {
      static std::vector<std::string> const EMPTY;
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));
      view->insert(trx, logicalCollection->id(), arangodb::LocalDocumentId(0), doc->slice(), meta);
      CHECK((trx.commit().ok()));
    }

    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": {} } \
    }");

    arangodb::Result res = view->updateProperties(links->slice(), true, false);
    CHECK(true == res.ok());
    CHECK((false == logicalCollection->getIndexes().empty()));

    {
      std::unordered_set<TRI_voc_cid_t> cids;
      view->sync();
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      arangodb::iresearch::appendKnownCollections(cids, *snapshot);
      CHECK((1 == cids.size()));
    }

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100 };
      std::set<TRI_voc_cid_t> actual = { };
      view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }

    CHECK((nullptr != vocbase.lookupCollection("testCollection")));

    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    CHECK((TRI_ERROR_NO_ERROR == vocbase.dropCollection(logicalCollection, true, -1)));
    CHECK((nullptr == vocbase.lookupCollection("testCollection")));

    {
      std::unordered_set<TRI_voc_cid_t> cids;
      view->sync();
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      arangodb::iresearch::appendKnownCollections(cids, *snapshot);
      CHECK((0 == cids.size()));
    }

    {
      std::set<TRI_voc_cid_t> actual;
      view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });
      CHECK((actual.empty())); // collection removal does modify view meta
    }

    CHECK((false == !vocbase.lookupView("testView")));
    CHECK((TRI_ERROR_NO_ERROR == vocbase.dropView("testView")));
    CHECK((true == !vocbase.lookupView("testView")));
  }

  // link removed before view
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !view));

    // add a document to the view
    {
      static std::vector<std::string> const EMPTY;
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));
      view->insert(trx, logicalCollection->id(), arangodb::LocalDocumentId(0), doc->slice(), meta);
      CHECK((trx.commit().ok()));
    }

    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": {} } \
    }");

    arangodb::Result res = view->updateProperties(links->slice(), true, false);
    CHECK(true == res.ok());
    CHECK((false == logicalCollection->getIndexes().empty()));

    {
      std::unordered_set<TRI_voc_cid_t> cids;
      view->sync();
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      arangodb::iresearch::appendKnownCollections(cids, *snapshot);
      CHECK((1 == cids.size()));
    }

    {
      std::unordered_set<TRI_voc_cid_t> expected = { 100 };
      std::set<TRI_voc_cid_t> actual;
      view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });

      for (auto& cid: expected) {
        CHECK((1 == actual.erase(cid)));
      }

      CHECK((actual.empty()));
    }

    CHECK((nullptr != vocbase.lookupCollection("testCollection")));
    CHECK((TRI_ERROR_NO_ERROR == vocbase.dropCollection(logicalCollection, true, -1)));
    CHECK((nullptr == vocbase.lookupCollection("testCollection")));

    {
      std::unordered_set<TRI_voc_cid_t> cids;
      view->sync();
      TrxStatePtr state(s.engine.createTransactionState(nullptr, arangodb::transaction::Options()));
      auto* snapshot = view->snapshot(*state, true);
      arangodb::iresearch::appendKnownCollections(cids, *snapshot);
      CHECK((0 == cids.size()));
    }

    {
      std::set<TRI_voc_cid_t> actual;
      view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });
      CHECK((actual.empty())); // collection removal does modify view meta
    }

    CHECK((false == !vocbase.lookupView("testView")));
    CHECK((TRI_ERROR_NO_ERROR == vocbase.dropView("testView")));
    CHECK((true == !vocbase.lookupView("testView")));
  }

  // view removed before link
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(viewJson->slice(), 0)
    );
    REQUIRE((false == !view));

    auto links = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \"testCollection\": {} } \
    }");

    arangodb::Result res = view->updateProperties(links->slice(), true, false);
    CHECK(true == res.ok());
    CHECK((false == logicalCollection->getIndexes().empty()));

    std::set<TRI_voc_cid_t> cids;
    view->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
    CHECK((1 == cids.size()));
    CHECK((false == !vocbase.lookupView("testView")));
    CHECK((TRI_ERROR_NO_ERROR == vocbase.dropView("testView")));
    CHECK((true == !vocbase.lookupView("testView")));
    CHECK((nullptr != vocbase.lookupCollection("testCollection")));
    CHECK((TRI_ERROR_NO_ERROR == vocbase.dropCollection(logicalCollection, true, -1)));
    CHECK((nullptr == vocbase.lookupCollection("testCollection")));
  }

  // view deallocated before link removed
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());

    {
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": {} } }");
      auto viewImpl = vocbase.createView(viewJson->slice(), 0);
      REQUIRE((nullptr != viewImpl));
      CHECK((viewImpl->updateProperties(updateJson->slice(), true, false).ok()));
      CHECK((false == logicalCollection->getIndexes().empty()));
      std::set<TRI_voc_cid_t> cids;
      viewImpl->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((1 == cids.size()));

      logicalCollection->getIndexes()[0]->unload(); // release view reference to prevent deadlock due to ~IResearchView() waiting for IResearchLink::unload()
      CHECK((false == logicalCollection->getIndexes().empty()));
    }

    // create a new view with same ID to validate links
    {
      auto json = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\":\"arangosearch\"}");
      auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
        arangodb::iresearch::IResearchView::make(vocbase, json->slice(), true)
      );
      REQUIRE((false == !view));
      std::set<TRI_voc_cid_t> cids;
      view->visitCollections([&cids](TRI_voc_cid_t cid)->bool { cids.emplace(cid); return true; });
      CHECK((0 == cids.size()));

      for (auto& index: logicalCollection->getIndexes()) {
        auto* link = dynamic_cast<arangodb::iresearch::IResearchLink*>(index.get());
        REQUIRE((*link != *view)); // check that link is unregistred from view
      }
    }
  }
}

SECTION("test_self_token") {
  // test empty token
  {
    arangodb::iresearch::IResearchView::AsyncSelf empty(nullptr);
    CHECK((nullptr == empty.get()));
  }

  arangodb::iresearch::IResearchView::AsyncSelf::ptr self;

  {
    auto json = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\" }");
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, json->slice(), false)
    );
    CHECK((false == !view));
    self = view->self();
    CHECK((false == !self));
    CHECK((view.get() == self->get()));
  }

  CHECK((false == !self));
  CHECK((nullptr == self->get()));
}

SECTION("test_tracked_cids") {
  auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\", \"id\": 100 }");
  auto viewJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 101, \"properties\": { } }");

  // test empty before open (TRI_vocbase_t::createView(...) will call open())
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = arangodb::iresearch::IResearchView::make(vocbase, viewJson->slice(), true);
    CHECK((nullptr != view));

    std::set<TRI_voc_cid_t> actual;
    view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });
    CHECK((actual.empty()));
  }

  // test add via link before open (TRI_vocbase_t::createView(...) will call open())
  {
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": { } } }");
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = arangodb::iresearch::IResearchView::make(vocbase, viewJson->slice(), true);
    REQUIRE((nullptr != view));
    StorageEngineMock().registerView(&vocbase, std::shared_ptr<arangodb::LogicalView>(view.get(), [](arangodb::LogicalView*)->void{})); // ensure link can find view

    CHECK((view->updateProperties(updateJson->slice(), false, false).ok()));

    std::set<TRI_voc_cid_t> actual;
    std::set<TRI_voc_cid_t> expected = { 100 };
    view->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });

    for (auto& cid: actual) {
      CHECK((1 == expected.erase(cid)));
    }

    CHECK((expected.empty()));
    logicalCollection->getIndexes()[0]->unload(); // release view reference to prevent deadlock due to ~IResearchView() waiting for IResearchLink::unload()
  }

  // test drop via link before open (TRI_vocbase_t::createView(...) will call open())
  {
    auto updateJson0 = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": { } } }");
    auto updateJson1 = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": null } }");
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto viewImpl = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      arangodb::iresearch::IResearchView::make(vocbase, viewJson->slice(), true)
    );
    REQUIRE((nullptr != viewImpl));
    StorageEngineMock().registerView(&vocbase, std::shared_ptr<arangodb::LogicalView>(viewImpl.get(), [](arangodb::LogicalView*)->void{})); // ensure link can find view

    // create link
    {
      CHECK((viewImpl->updateProperties(updateJson0->slice(), false, false).ok()));

      std::set<TRI_voc_cid_t> actual;
      std::set<TRI_voc_cid_t> expected = { 100 };
      viewImpl->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });

      for (auto& cid: actual) {
        CHECK((1 == expected.erase(cid)));
      }

      CHECK((expected.empty()));
    }

    // drop link
    {
      CHECK((viewImpl->updateProperties(updateJson1->slice(), false, false).ok()));

      std::set<TRI_voc_cid_t> actual;
      viewImpl->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });
      CHECK((actual.empty()));
    }
  }

  // test load persisted CIDs on open (TRI_vocbase_t::createView(...) will call open())
  // use separate view ID for this test since doing open from persisted store
  {
    // initial populate persisted view
    {
      auto createJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 102, \"properties\": { } }");
      auto* feature = arangodb::iresearch::getFeature<arangodb::FlushFeature>("Flush");
      REQUIRE(feature);
      Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
      auto viewImpl = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
        vocbase.createView(createJson->slice(), 0)
      );
      REQUIRE((nullptr != viewImpl));

      static std::vector<std::string> const EMPTY;
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
      arangodb::iresearch::IResearchLinkMeta meta;
      meta._includeAllFields = true;
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );
      CHECK((trx.begin().ok()));
      viewImpl->insert(trx, 42, arangodb::LocalDocumentId(0), doc->slice(), meta);
      CHECK((trx.commit().ok()));
      feature->executeCallbacks(); // commit to persisted store
    }

    // test persisted CIDs on open
    {
      auto createJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\", \"id\": 102, \"properties\": { } }");
      Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
      auto viewImpl = vocbase.createView(createJson->slice(), 0);
      REQUIRE((nullptr != viewImpl));

      std::set<TRI_voc_cid_t> actual;
      viewImpl->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });
      CHECK((actual.empty())); // persisted cids do not modify view meta
    }
  }

  // test add via link after open (TRI_vocbase_t::createView(...) will call open())
  {
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": { } } }");
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto viewImpl = vocbase.createView(viewJson->slice(), 0);
    REQUIRE((nullptr != viewImpl));

    CHECK((viewImpl->updateProperties(updateJson->slice(), false, false).ok()));

    std::set<TRI_voc_cid_t> actual;
    std::set<TRI_voc_cid_t> expected = { 100 };
    viewImpl->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });

    for (auto& cid: actual) {
      CHECK((1 == expected.erase(cid)));
    }

    CHECK((expected.empty()));
  }

  // test drop via link after open (TRI_vocbase_t::createView(...) will call open())
  {
    auto updateJson0 = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": { } } }");
    auto updateJson1 = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection\": null } }");
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto viewImpl = vocbase.createView(viewJson->slice(), 0);
    REQUIRE((nullptr != viewImpl));

    // create link
    {
      CHECK((viewImpl->updateProperties(updateJson0->slice(), false, false).ok()));

      std::set<TRI_voc_cid_t> actual;
      std::set<TRI_voc_cid_t> expected = { 100 };
      viewImpl->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });

      for (auto& cid: actual) {
        CHECK((1 == expected.erase(cid)));
      }

      CHECK((expected.empty()));
    }

    // drop link
    {
      CHECK((viewImpl->updateProperties(updateJson1->slice(), false, false).ok()));

      std::set<TRI_voc_cid_t> actual;
      viewImpl->visitCollections([&actual](TRI_voc_cid_t cid)->bool { actual.emplace(cid); return true; });
      CHECK((actual.empty()));
    }
  }
}

SECTION("test_transaction_registration") {
  auto collectionJson0 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection0\" }");
  auto collectionJson1 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection1\" }");
  auto viewJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\" }");
  Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  auto* logicalCollection0 = vocbase.createCollection(collectionJson0->slice());
  REQUIRE((nullptr != logicalCollection0));
  auto* logicalCollection1 = vocbase.createCollection(collectionJson1->slice());
  REQUIRE((nullptr != logicalCollection1));
  auto logicalView = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
    vocbase.createView(viewJson->slice(), 0)
  );
  REQUIRE((nullptr != logicalView));

  // link collection to view
  {
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection0\": {}, \"testCollection1\": {} } }");
    CHECK((logicalView->updateProperties(updateJson->slice(), false, false).ok()));
  }

  // read transaction (by id)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->id(),
      arangodb::AccessMode::Type::READ
    );
    CHECK((trx.begin().ok()));
    CHECK((2 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection1->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0", "testCollection1" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // read transaction (by name)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->name(),
      arangodb::AccessMode::Type::READ
    );
    CHECK((trx.begin().ok()));
    CHECK((2 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection1->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0", "testCollection1" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // write transaction (by id)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->id(),
      arangodb::AccessMode::Type::WRITE
    );
    CHECK((trx.begin().ok()));
    CHECK((2 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection1->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0", "testCollection1" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // write transaction (by name)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->name(),
      arangodb::AccessMode::Type::WRITE
    );
    CHECK((trx.begin().ok()));
    CHECK((2 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection1->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0", "testCollection1" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // exclusive transaction (by id)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->id(),
      arangodb::AccessMode::Type::READ
    );
    CHECK((trx.begin().ok()));
    CHECK((2 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection1->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0", "testCollection1" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // exclusive transaction (by name)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->name(),
      arangodb::AccessMode::Type::READ
    );
    CHECK((trx.begin().ok()));
    CHECK((2 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection1->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0", "testCollection1" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // drop collection from vocbase
  CHECK((TRI_ERROR_NO_ERROR == vocbase.dropCollection(logicalCollection1, true, 0)));

  // read transaction (by id) (one collection dropped)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->id(),
      arangodb::AccessMode::Type::READ
    );
    CHECK((trx.begin().ok()));
    CHECK((1 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // read transaction (by name) (one collection dropped)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->name(),
      arangodb::AccessMode::Type::READ
    );
    CHECK((trx.begin().ok()));
    CHECK((1 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // write transaction (by id) (one collection dropped)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->id(),
      arangodb::AccessMode::Type::WRITE
    );
    CHECK((trx.begin().ok()));
    CHECK((1 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // write transaction (by name) (one collection dropped)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->name(),
      arangodb::AccessMode::Type::WRITE
    );
    CHECK((trx.begin().ok()));
    CHECK((1 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // exclusive transaction (by id) (one collection dropped)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->id(),
      arangodb::AccessMode::Type::READ
    );
    CHECK((trx.begin().ok()));
    CHECK((1 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }

  // exclusive transaction (by name) (one collection dropped)
  {
    arangodb::SingleCollectionTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      logicalView->name(),
      arangodb::AccessMode::Type::READ
    );
    CHECK((trx.begin().ok()));
    CHECK((1 == trx.state()->numCollections()));
    CHECK((nullptr != trx.state()->findCollection(logicalCollection0->id())));
    std::unordered_set<std::string> expectedNames = { "testCollection0" };
    auto actualNames = trx.state()->collectionNames();

    for(auto& entry: actualNames) {
      CHECK((1 == expectedNames.erase(entry)));
    }

    CHECK((expectedNames.empty()));
    CHECK((trx.commit().ok()));
  }
}

SECTION("test_transaction_snapshot") {
  static std::vector<std::string> const EMPTY;
  auto viewJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testView\", \"type\": \"arangosearch\", \"commit\": { \"commitIntervalMsec\": 0 } }");
  Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  auto viewImpl = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
    vocbase.createView(viewJson->slice(), 0)
  );
  REQUIRE((nullptr != viewImpl));

  // add a single document to view (do not sync)
  {
    auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 1 }");
    arangodb::iresearch::IResearchLinkMeta meta;
    meta._includeAllFields = true;
    arangodb::transaction::UserTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
    );
    CHECK((trx.begin().ok()));
    viewImpl->insert(trx, 42, arangodb::LocalDocumentId(0), doc->slice(), meta);
    CHECK((trx.commit().ok()));
  }

  // no snapshot in TransactionState (force == false, waitForSync = false)
  {
    std::unique_ptr<arangodb::TransactionState> state(
      s.engine.createTransactionState(&vocbase, arangodb::transaction::Options())
    );
    auto* snapshot = viewImpl->snapshot(*state);
    CHECK((nullptr == snapshot));
  }

  // no snapshot in TransactionState (force == true, waitForSync = false)
  {
    std::unique_ptr<arangodb::TransactionState> state(
      s.engine.createTransactionState(&vocbase, arangodb::transaction::Options())
    );
    auto* snapshot = viewImpl->snapshot(*state, true);
    CHECK((nullptr != snapshot));
    CHECK((0 == snapshot->live_docs_count()));
  }

  // no snapshot in TransactionState (force == false, waitForSync = true)
  {
    std::unique_ptr<arangodb::TransactionState> state(
      s.engine.createTransactionState(&vocbase, arangodb::transaction::Options())
    );
    state->waitForSync(true);
    auto* snapshot = viewImpl->snapshot(*state);
    CHECK((nullptr == snapshot));
  }

  // no snapshot in TransactionState (force == true, waitForSync = true)
  {
    arangodb::transaction::Options options;
    std::unique_ptr<arangodb::TransactionState> state(
      s.engine.createTransactionState(&vocbase, arangodb::transaction::Options())
    );
    state->waitForSync(true);
    auto* snapshot = viewImpl->snapshot(*state, true);
    CHECK((nullptr != snapshot));
    CHECK((1 == snapshot->live_docs_count()));
  }

  // add another single document to view (do not sync)
  {
    auto doc = arangodb::velocypack::Parser::fromJson("{ \"key\": 2 }");
    arangodb::iresearch::IResearchLinkMeta meta;
    meta._includeAllFields = true;
    arangodb::transaction::UserTransaction trx(
      arangodb::transaction::StandaloneContext::Create(&vocbase),
      EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
    );
    CHECK((trx.begin().ok()));
    viewImpl->insert(trx, 42, arangodb::LocalDocumentId(1), doc->slice(), meta);
    CHECK((trx.commit().ok()));
  }

  // old snapshot in TransactionState (force == false, waitForSync = false)
  {
    std::unique_ptr<arangodb::TransactionState> state(
      s.engine.createTransactionState(&vocbase, arangodb::transaction::Options())
    );
    viewImpl->apply(*state);
    state->updateStatus(arangodb::transaction::Status::RUNNING);
    auto* snapshot = viewImpl->snapshot(*state);
    CHECK((nullptr != snapshot));
    CHECK((1 == snapshot->live_docs_count()));
    state->updateStatus(arangodb::transaction::Status::ABORTED); // prevent assertion ind destructor
  }

  // old snapshot in TransactionState (force == true, waitForSync = false)
  {
    std::unique_ptr<arangodb::TransactionState> state(
      s.engine.createTransactionState(&vocbase, arangodb::transaction::Options())
    );
    viewImpl->apply(*state);
    state->updateStatus(arangodb::transaction::Status::RUNNING);
    auto* snapshot = viewImpl->snapshot(*state, true);
    CHECK((nullptr != snapshot));
    CHECK((1 == snapshot->live_docs_count()));
    state->updateStatus(arangodb::transaction::Status::ABORTED); // prevent assertion ind destructor
  }

  // old snapshot in TransactionState (force == true, waitForSync = false during updateStatus(), true during snapshot())
  {
    std::unique_ptr<arangodb::TransactionState> state(
      s.engine.createTransactionState(&vocbase, arangodb::transaction::Options())
    );
    viewImpl->apply(*state);
    state->updateStatus(arangodb::transaction::Status::RUNNING);
    state->waitForSync(true);
    auto* snapshot = viewImpl->snapshot(*state, true);
    CHECK((nullptr != snapshot));
    CHECK((1 == snapshot->live_docs_count()));
    state->updateStatus(arangodb::transaction::Status::ABORTED); // prevent assertion ind destructor
  }

  // old snapshot in TransactionState (force == true, waitForSync = true during updateStatus(), false during snapshot())
  {
    std::unique_ptr<arangodb::TransactionState> state(
      s.engine.createTransactionState(&vocbase, arangodb::transaction::Options())
    );
    state->waitForSync(true);
    viewImpl->apply(*state);
    state->updateStatus(arangodb::transaction::Status::RUNNING);
    state->waitForSync(false);
    auto* snapshot = viewImpl->snapshot(*state, true);
    CHECK((nullptr != snapshot));
    CHECK((2 == snapshot->live_docs_count()));
    state->updateStatus(arangodb::transaction::Status::ABORTED); // prevent assertion ind destructor
  }
}

SECTION("test_update_overwrite") {
  auto createJson = arangodb::velocypack::Parser::fromJson("{ \
    \"name\": \"testView\", \
    \"type\": \"arangosearch\" \
  }");

  // modify meta params
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));

    // initial update (overwrite)
    {
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
        \"locale\": \"en\", \
        \"threadsMaxIdle\": 10, \
        \"threadsMaxTotal\": 20 \
      }");

      expectedMeta._locale = irs::locale_utils::locale("en", true);
      expectedMeta._threadsMaxIdle = 10;
      expectedMeta._threadsMaxTotal = 20;
      CHECK((view->updateProperties(updateJson->slice(), false, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      CHECK(4 == slice.length());
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      auto propSlice = slice.get("properties");
      CHECK(propSlice.isObject());
      CHECK((6U == propSlice.length()));
      CHECK((meta.init(propSlice, error) && expectedMeta == meta));

      auto tmpSlice = propSlice.get("links");
      CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
    }

    // subsequent update (overwrite)
    {
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
        \"locale\": \"ru\" \
      }");

      expectedMeta._locale = irs::locale_utils::locale("ru", true);
      CHECK((view->updateProperties(updateJson->slice(), false, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      auto propSlice = slice.get("properties");
      CHECK(propSlice.isObject());
      CHECK((6U == propSlice.length()));
      CHECK((meta.init(propSlice, error) && expectedMeta == meta));

      auto tmpSlice = propSlice.get("links");
      CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
    }
  }

  // overwrite links
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson0 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection0\" }");
    auto collectionJson1 = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection1\" }");
    auto* logicalCollection0 = vocbase.createCollection(collectionJson0->slice());
    REQUIRE((nullptr != logicalCollection0));
    auto* logicalCollection1 = vocbase.createCollection(collectionJson1->slice());
    REQUIRE((nullptr != logicalCollection1));
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));
    REQUIRE(view->category() == arangodb::LogicalView::category());
    CHECK((true == logicalCollection0->getIndexes().empty()));
    CHECK((true == logicalCollection1->getIndexes().empty()));

    // initial creation
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection0\": {} } }");
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      std::unordered_map<std::string, arangodb::iresearch::IResearchLinkMeta> expectedLinkMeta;

      expectedMeta._collections.insert(logicalCollection0->id());
      expectedLinkMeta["testCollection0"]; // use defaults
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      auto propSlice = slice.get("properties");
      CHECK(propSlice.isObject());
      CHECK((6U == propSlice.length()));
      CHECK((meta.init(propSlice, error) && expectedMeta == meta));

      auto tmpSlice = propSlice.get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

      for (arangodb::velocypack::ObjectIterator itr(tmpSlice); itr.valid(); ++itr) {
        arangodb::iresearch::IResearchLinkMeta linkMeta;
        auto key = itr.key();
        auto value = itr.value();
        CHECK((true == key.isString()));

        auto expectedItr = expectedLinkMeta.find(key.copyString());
        CHECK((
          true == value.isObject()
          && expectedItr != expectedLinkMeta.end()
          && linkMeta.init(value, error)
          && expectedItr->second == linkMeta
        ));
        expectedLinkMeta.erase(expectedItr);
      }

      CHECK((true == expectedLinkMeta.empty()));
      CHECK((false == logicalCollection0->getIndexes().empty()));
      CHECK((true == logicalCollection1->getIndexes().empty()));
    }

    // update overwrite links
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \"links\": { \"testCollection1\": {} } }");
      arangodb::iresearch::IResearchViewMeta expectedMeta;
      std::unordered_map<std::string, arangodb::iresearch::IResearchLinkMeta> expectedLinkMeta;

      expectedMeta._collections.insert(logicalCollection1->id());
      expectedLinkMeta["testCollection1"]; // use defaults
      CHECK((view->updateProperties(updateJson->slice(), false, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      auto propSlice = slice.get("properties");
      CHECK(propSlice.isObject());
      CHECK((6U == propSlice.length()));
      CHECK((meta.init(propSlice, error) && expectedMeta == meta));

      auto tmpSlice = propSlice.get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

      for (arangodb::velocypack::ObjectIterator itr(tmpSlice); itr.valid(); ++itr) {
        arangodb::iresearch::IResearchLinkMeta linkMeta;
        auto key = itr.key();
        auto value = itr.value();
        CHECK((true == key.isString()));

        auto expectedItr = expectedLinkMeta.find(key.copyString());
        CHECK((
          true == value.isObject()
          && expectedItr != expectedLinkMeta.end()
          && linkMeta.init(value, error)
          && expectedItr->second == linkMeta
        ));
        expectedLinkMeta.erase(expectedItr);
      }

      CHECK((true == expectedLinkMeta.empty()));
      CHECK((true == logicalCollection0->getIndexes().empty()));
      CHECK((false == logicalCollection1->getIndexes().empty()));
    }
  }

  // update existing link (full update)
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));
    REQUIRE(view->category() == arangodb::LogicalView::category());

    // initial add of link
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": { \"includeAllFields\": true } } }"
      );
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties

      auto tmpSlice = slice.get("properties").get("collections");
      CHECK((true == tmpSlice.isArray() && 1 == tmpSlice.length()));
      tmpSlice = slice.get("properties").get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
      tmpSlice = tmpSlice.get("testCollection");
      CHECK((true == tmpSlice.isObject()));
      tmpSlice = tmpSlice.get("includeAllFields");
      CHECK((true == tmpSlice.isBoolean() && true == tmpSlice.getBoolean()));
    }

    // update link
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": { } } }"
      );
      CHECK((view->updateProperties(updateJson->slice(), false, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      auto tmpSlice = slice.get("properties").get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
      tmpSlice = tmpSlice.get("testCollection");
      CHECK((true == tmpSlice.isObject()));
      tmpSlice = tmpSlice.get("includeAllFields");
      CHECK((true == tmpSlice.isBoolean() && false == tmpSlice.getBoolean()));
    }
  }
}

SECTION("test_update_partial") {
  auto createJson = arangodb::velocypack::Parser::fromJson("{ \
    \"name\": \"testView\", \
    \"type\": \"arangosearch\" \
  }");

  // modify meta params
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));
    REQUIRE(view->category() == arangodb::LogicalView::category());

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"locale\": \"en\", \
      \"threadsMaxIdle\": 10, \
      \"threadsMaxTotal\": 20 \
    }");

    expectedMeta._locale = irs::locale_utils::locale("en", true);
    expectedMeta._threadsMaxIdle = 10;
    expectedMeta._threadsMaxTotal = 20;
    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, false);
    builder.close();

    auto slice = builder.slice();
    CHECK(slice.isObject());
    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone()); // no system properties
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    auto propSlice = slice.get("properties");
    CHECK(propSlice.isObject());
    CHECK((6U == propSlice.length()));
    CHECK((meta.init(propSlice, error) && expectedMeta == meta));

    auto tmpSlice = propSlice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // test rollback on meta modification failure (as an example invalid value for 'locale')
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));
    REQUIRE(view->category() == arangodb::LogicalView::category());

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson(std::string() + "{ \
      \"locale\": 123, \
      \"threadsMaxIdle\": 10, \
      \"threadsMaxTotal\": 20 \
    }");

    CHECK((TRI_ERROR_BAD_PARAMETER == view->updateProperties(updateJson->slice(), true, false).errorNumber()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, false);
    builder.close();

    auto slice = builder.slice();
    CHECK(slice.isObject());
    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone()); // no system properties
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    auto propSlice = slice.get("properties");
    CHECK(propSlice.isObject());
    CHECK((6U == propSlice.length()));
    CHECK((meta.init(propSlice, error) && expectedMeta == meta));

    auto tmpSlice = propSlice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // add a new link (in recovery)
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));
    REQUIRE(view->category() == arangodb::LogicalView::category());

    auto updateJson = arangodb::velocypack::Parser::fromJson(
      "{ \"links\": { \"testCollection\": {} } }"
    );

    auto before = StorageEngineMock::inRecoveryResult;
    StorageEngineMock::inRecoveryResult = true;
    auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, false);
    builder.close();

    auto slice = builder.slice();
    CHECK(slice.isObject());
    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone()); // no system properties

    auto propSlice = slice.get("properties");
    CHECK(propSlice.isObject());
    CHECK((
      true == propSlice.hasKey("links")
      && propSlice.get("links").isObject()
      && 1 == propSlice.get("links").length()
    ));
  }

  // add a new link
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));
    REQUIRE(view->category() == arangodb::LogicalView::category());

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    std::unordered_map<std::string, arangodb::iresearch::IResearchLinkMeta> expectedLinkMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": {} \
      }}");

    expectedMeta._collections.insert(logicalCollection->id());
    expectedLinkMeta["testCollection"]; // use defaults
    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, false);
    builder.close();

    auto slice = builder.slice();
    CHECK(slice.isObject());
    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone()); // no system properties
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    auto propSlice = slice.get("properties");
    CHECK(propSlice.isObject());
    CHECK((6U == propSlice.length()));
    CHECK((meta.init(propSlice, error) && expectedMeta == meta));

    auto tmpSlice = propSlice.get("links");
    CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

    for (arangodb::velocypack::ObjectIterator itr(tmpSlice); itr.valid(); ++itr) {
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      auto key = itr.key();
      auto value = itr.value();
      CHECK((true == key.isString()));

      auto expectedItr = expectedLinkMeta.find(key.copyString());
      CHECK((
        true == value.isObject()
        && expectedItr != expectedLinkMeta.end()
        && linkMeta.init(value, error)
        && expectedItr->second == linkMeta
      ));
      expectedLinkMeta.erase(expectedItr);
    }

    CHECK((true == expectedLinkMeta.empty()));
  }

  // add a new link to a collection with documents
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));
    REQUIRE(view->category() == arangodb::LogicalView::category());

    {
      static std::vector<std::string> const EMPTY;
      auto doc = arangodb::velocypack::Parser::fromJson("{ \"abc\": \"def\" }");
      arangodb::transaction::UserTransaction trx(
        arangodb::transaction::StandaloneContext::Create(&vocbase),
        EMPTY, EMPTY, EMPTY, arangodb::transaction::Options()
      );

      CHECK((trx.begin().ok()));
      CHECK((trx.insert(logicalCollection->name(), doc->slice(), arangodb::OperationOptions()).ok()));
      CHECK((trx.commit().ok()));
    }

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    std::unordered_map<std::string, arangodb::iresearch::IResearchLinkMeta> expectedLinkMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": {} \
      }}");

    expectedMeta._collections.insert(logicalCollection->id());
    expectedLinkMeta["testCollection"]; // use defaults
    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, false);
    builder.close();

    auto slice = builder.slice();
    CHECK(slice.isObject());
    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone()); // no system properties
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    auto propSlice = slice.get("properties");
    CHECK((6U == propSlice.length()));
    CHECK((meta.init(propSlice, error) && expectedMeta == meta));

    auto tmpSlice = propSlice.get("links");
    CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

    for (arangodb::velocypack::ObjectIterator itr(tmpSlice); itr.valid(); ++itr) {
      arangodb::iresearch::IResearchLinkMeta linkMeta;
      auto key = itr.key();
      auto value = itr.value();
      CHECK((true == key.isString()));

      auto expectedItr = expectedLinkMeta.find(key.copyString());
      CHECK((
        true == value.isObject()
        && expectedItr != expectedLinkMeta.end()
        && linkMeta.init(value, error)
        && expectedItr->second == linkMeta
      ));
      expectedLinkMeta.erase(expectedItr);
    }

    CHECK((true == expectedLinkMeta.empty()));
  }

  // add new link to non-existant collection
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));
    REQUIRE(view->category() == arangodb::LogicalView::category());

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": {} \
      }}");

    CHECK((TRI_ERROR_BAD_PARAMETER == view->updateProperties(updateJson->slice(), true, false).errorNumber()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, false);
    builder.close();

    auto slice = builder.slice();
    CHECK(slice.isObject());
    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone()); // no system properties
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    auto propSlice = slice.get("properties");
    CHECK((6U == propSlice.length()));
    CHECK((meta.init(propSlice, error) && expectedMeta == meta));

    auto tmpSlice = propSlice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // remove link (in recovery)
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice(), 0)
    );
    REQUIRE((false == !view));
    REQUIRE(view->category() == arangodb::LogicalView::category());

    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": {} } }"
      );
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties

      auto propSlice = slice.get("properties");
      CHECK((
        true == propSlice.hasKey("links")
        && propSlice.get("links").isObject()
        && 1 == propSlice.get("links").length()
      ));
    }

    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": null } }"
      );

      auto before = StorageEngineMock::inRecoveryResult;
      StorageEngineMock::inRecoveryResult = true;
      auto restore = irs::make_finally([&before]()->void { StorageEngineMock::inRecoveryResult = before; });
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties

      auto propSlice = slice.get("properties");
      CHECK((
        true == propSlice.hasKey("links")
        && propSlice.get("links").isObject()
        && 0 == propSlice.get("links").length()
      ));
    }
  }

  // remove link
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;

    expectedMeta._collections.insert(logicalCollection->id());

    {
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
        \"links\": { \
          \"testCollection\": {} \
      }}");

      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      auto propSlice = slice.get("properties");
      CHECK(propSlice.isObject());
      CHECK((6U == propSlice.length()));
      CHECK((meta.init(propSlice, error) && expectedMeta == meta));

      auto tmpSlice = propSlice.get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
    }

    {
      auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
        \"links\": { \
          \"testCollection\": null \
      }}");

      expectedMeta._collections.clear();
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      arangodb::iresearch::IResearchViewMeta meta;
      std::string error;

      auto propSlice = slice.get("properties");
      CHECK(propSlice.isObject());
      CHECK((6U == propSlice.length()));
      CHECK((meta.init(propSlice, error) && expectedMeta == meta));

      auto tmpSlice = propSlice.get("links");
      CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
    }
  }

  // remove link from non-existant collection
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto view = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": null \
      }}");

    CHECK((TRI_ERROR_BAD_PARAMETER == view->updateProperties(updateJson->slice(), true, false).errorNumber()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, false);
    builder.close();

    auto slice = builder.slice();
    CHECK(slice.isObject());
    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone()); // no system properties
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    auto propSlice = slice.get("properties");
    CHECK(propSlice.isObject());
    CHECK((6U == propSlice.length()));
    CHECK((meta.init(propSlice, error) && expectedMeta == meta));

    auto tmpSlice = propSlice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // remove non-existant link
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !view));

    arangodb::iresearch::IResearchViewMeta expectedMeta;
    auto updateJson = arangodb::velocypack::Parser::fromJson("{ \
      \"links\": { \
        \"testCollection\": null \
    }}");

    CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->toVelocyPack(builder, true, false);
    builder.close();

    auto slice = builder.slice();
    CHECK(slice.isObject());
    CHECK(slice.get("name").copyString() == "testView");
    CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
    CHECK(slice.get("deleted").isNone()); // no system properties
    arangodb::iresearch::IResearchViewMeta meta;
    std::string error;

    auto propSlice = slice.get("properties");
    CHECK(propSlice.isObject());
    CHECK((6U == propSlice.length()));
    CHECK((meta.init(propSlice, error) && expectedMeta == meta));

    auto tmpSlice = propSlice.get("links");
    CHECK((true == tmpSlice.isObject() && 0 == tmpSlice.length()));
  }

  // remove + add link to same collection (reindex)
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !view));

    // initial add of link
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": {} } }"
      );
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      auto tmpSlice = slice.get("properties").get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
    }

    // add + remove
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": null, \"testCollection\": {} } }"
      );
      std::unordered_set<TRI_idx_iid_t> initial;

      for (auto& idx: logicalCollection->getIndexes()) {
        initial.emplace(idx->id());
      }

      CHECK((!initial.empty()));
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));
      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      auto tmpSlice = slice.get("properties").get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));

      std::unordered_set<TRI_idx_iid_t> actual;

      for (auto& index: logicalCollection->getIndexes()) {
        actual.emplace(index->id());
      }

      CHECK((initial != actual)); // a reindexing took place (link recreated)
    }
  }

  // update existing link (partial update)
  {
    Vocbase vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
    auto collectionJson = arangodb::velocypack::Parser::fromJson("{ \"name\": \"testCollection\" }");
    auto* logicalCollection = vocbase.createCollection(collectionJson->slice());
    REQUIRE((nullptr != logicalCollection));
    auto view = vocbase.createView(createJson->slice(), 0);
    REQUIRE((false == !view));

    // initial add of link
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": { \"includeAllFields\": true } } }"
      );
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      auto tmpSlice = slice.get("properties").get("collections");
      CHECK((true == tmpSlice.isArray() && 1 == tmpSlice.length()));
      tmpSlice = slice.get("properties").get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
      tmpSlice = tmpSlice.get("testCollection");
      CHECK((true == tmpSlice.isObject()));
      tmpSlice = tmpSlice.get("includeAllFields");
      CHECK((true == tmpSlice.isBoolean() && true == tmpSlice.getBoolean()));
    }

    // update link
    {
      auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\": { \"testCollection\": { } } }"
      );
      CHECK((view->updateProperties(updateJson->slice(), true, false).ok()));

      arangodb::velocypack::Builder builder;

      builder.openObject();
      view->toVelocyPack(builder, true, false);
      builder.close();

      auto slice = builder.slice();
      CHECK(slice.isObject());
      CHECK(slice.get("name").copyString() == "testView");
      CHECK(slice.get("type").copyString() == arangodb::iresearch::IResearchView::type().name());
      CHECK(slice.get("deleted").isNone()); // no system properties
      auto tmpSlice = slice.get("properties").get("links");
      CHECK((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
      tmpSlice = tmpSlice.get("testCollection");
      CHECK((true == tmpSlice.isObject()));
      tmpSlice = tmpSlice.get("includeAllFields");
      CHECK((true == tmpSlice.isBoolean() && false == tmpSlice.getBoolean()));
    }
  }
}

}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
