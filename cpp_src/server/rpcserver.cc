#include "rpcserver.h"
#include <sys/stat.h>
#include <sstream>
#include "core/cjson/jsonbuilder.h"
#include "core/iclientsstats.h"
#include "core/transactionimpl.h"
#include "net/cproto/cproto.h"
#include "net/cproto/serverconnection.h"
#include "net/listener.h"
#include "reindexer_version.h"
#include "vendor/msgpack/msgpack.h"

namespace reindexer_server {
const reindexer::SemVersion kMinUnknownReplSupportRxVersion("2.6.0");
const size_t kMaxTxCount = 1024;

RPCServer::RPCServer(DBManager &dbMgr, LoggerWrapper &logger, IClientsStats *clientsStats, bool allocDebug, IStatsWatcher *statsCollector)
	: dbMgr_(dbMgr),
	  logger_(logger),
	  allocDebug_(allocDebug),
	  statsWatcher_(statsCollector),
	  clientsStats_(clientsStats),
	  startTs_(std::chrono::system_clock::now()) {}

RPCServer::~RPCServer() {}

Error RPCServer::Ping(cproto::Context &) {
	//
	return 0;
}

static std::atomic<int> connCounter;

Error RPCServer::Login(cproto::Context &ctx, p_string login, p_string password, p_string db, cproto::optional<bool> createDBIfMissing,
					   cproto::optional<bool> checkClusterID, cproto::optional<int> expectedClusterID,
					   cproto::optional<p_string> clientRxVersion, cproto::optional<p_string> appName) {
	if (ctx.GetClientData()) {
		return Error(errParams, "Already logged in");
	}

	std::unique_ptr<RPCClientData> clientData(new RPCClientData);

	clientData->connID = connCounter.fetch_add(1, std::memory_order_relaxed);
	clientData->pusher.SetWriter(ctx.writer);
	clientData->subscribed = false;
	clientData->auth = AuthContext(login.toString(), password.toString());
	clientData->txStats = std::make_shared<reindexer::TxStats>();

	auto dbName = db.toString();
	if (checkClusterID.hasValue() && checkClusterID.value()) {
		assert(expectedClusterID.hasValue());
		clientData->auth.SetExpectedClusterID(expectedClusterID.value());
	}
	auto status = dbMgr_.Login(dbName, clientData->auth);
	if (!status.ok()) {
		return status;
	}

	if (clientRxVersion.hasValue()) {
		clientData->rxVersion = SemVersion(string_view(clientRxVersion.value()));
	} else {
		clientData->rxVersion = SemVersion();
	}
	if (clientData->rxVersion < kMinUnknownReplSupportRxVersion) {
		clientData->pusher.SetFilter([](WALRecord &rec) {
			if (rec.type == WalCommitTransaction || rec.type == WalInitTransaction || rec.type == WalSetSchema) {
				return true;
			}
			rec.inTransaction = false;
			return false;
		});
	}

	if (clientsStats_) {
		reindexer::ClientConnectionStat conn;
		conn.connectionStat = ctx.writer->GetConnectionStat();
		conn.ip = string(ctx.clientAddr);
		conn.userName = clientData->auth.Login();
		conn.dbName = clientData->auth.DBName();
		conn.userRights = string(UserRoleName(clientData->auth.UserRights()));
		conn.clientVersion = clientData->rxVersion.StrippedString();
		conn.appName = appName.hasValue() ? appName.value().toString() : string();
		conn.txStats = clientData->txStats;
		conn.updatesPusher = &clientData->pusher;
		clientsStats_->AddConnection(clientData->connID, std::move(conn));
	}

	ctx.SetClientData(std::move(clientData));
	if (statsWatcher_) {
		statsWatcher_->OnClientConnected(dbName, statsSourceName());
	}
	int64_t startTs = std::chrono::duration_cast<std::chrono::seconds>(startTs_.time_since_epoch()).count();
	static string_view version = REINDEX_VERSION;

	status = db.length() ? OpenDatabase(ctx, db, createDBIfMissing) : errOK;
	if (status.ok()) {
		ctx.Return({cproto::Arg(p_string(&version)), cproto::Arg(startTs)}, status);
	}

	return status;
}

static RPCClientData *getClientDataUnsafe(cproto::Context &ctx) { return dynamic_cast<RPCClientData *>(ctx.GetClientData()); }

static RPCClientData *getClientDataSafe(cproto::Context &ctx) {
	auto ret = dynamic_cast<RPCClientData *>(ctx.GetClientData());
	if (!ret) std::abort();	 // It should be set by middleware
	return ret;
}

Error RPCServer::OpenDatabase(cproto::Context &ctx, p_string db, cproto::optional<bool> createDBIfMissing) {
	auto *clientData = getClientDataSafe(ctx);
	if (clientData->auth.HaveDB()) {
		return Error(errParams, "Database already opened");
	}
	auto status = dbMgr_.OpenDatabase(db.toString(), clientData->auth, createDBIfMissing.hasValue() && createDBIfMissing.value());
	if (!status.ok()) {
		clientData->auth.ResetDB();
	}
	return status;
}

Error RPCServer::CloseDatabase(cproto::Context &ctx) {
	auto clientData = getClientDataSafe(ctx);
	clientData->auth.ResetDB();
	return errOK;
}
Error RPCServer::DropDatabase(cproto::Context &ctx) {
	auto clientData = getClientDataSafe(ctx);
	return dbMgr_.DropDatabase(clientData->auth);
}

Error RPCServer::CheckAuth(cproto::Context &ctx) {
	cproto::ClientData *ptr = ctx.GetClientData();
	auto clientData = dynamic_cast<RPCClientData *>(ptr);

	if (ctx.call->cmd == cproto::kCmdLogin || ctx.call->cmd == cproto::kCmdPing) {
		return errOK;
	}

	if (!clientData) {
		return Error(errForbidden, "You should login");
	}

	return errOK;
}

void RPCServer::OnClose(cproto::Context &ctx, const Error &err) {
	(void)ctx;
	(void)err;

	if (statsWatcher_) {
		auto clientData = getClientDataUnsafe(ctx);
		if (clientData) {
			statsWatcher_->OnClientDisconnected(clientData->auth.DBName(), statsSourceName());
		}
	}
	if (clientsStats_) {
		auto clientData = dynamic_cast<RPCClientData *>(ctx.GetClientData());
		if (clientData) clientsStats_->DeleteConnection(clientData->connID);
	}
	logger_.info("RPC: Client disconnected");
}

void RPCServer::OnResponse(cproto::Context &ctx) {
	if (statsWatcher_) {
		auto clientData = getClientDataUnsafe(ctx);
		auto dbName = (clientData != nullptr) ? clientData->auth.DBName() : "<unknown>";
		statsWatcher_->OnOutputTraffic(dbName, statsSourceName(), ctx.stat.sizeStat.respSizeBytes);
		if (ctx.stat.sizeStat.respSizeBytes) {
			// Don't update stats on responses like "updates push"
			statsWatcher_->OnInputTraffic(dbName, statsSourceName(), ctx.stat.sizeStat.reqSizeBytes);
		}
	}
}

void RPCServer::Logger(cproto::Context &ctx, const Error &err, const cproto::Args &ret) {
	auto clientData = getClientDataUnsafe(ctx);
	WrSerializer ser;

	if (clientData) {
		ser << "c='"_sv << clientData->connID << "' db='"_sv << clientData->auth.Login() << "@"_sv << clientData->auth.DBName() << "' "_sv;
	} else {
		ser << "- - "_sv;
	}

	if (ctx.call) {
		ser << cproto::CmdName(ctx.call->cmd) << " "_sv;
		ctx.call->args.Dump(ser);
	} else {
		ser << '-';
	}

	ser << " -> "_sv << (err.ok() ? "OK"_sv : err.what());
	if (ret.size()) {
		ser << ' ';
		ret.Dump(ser);
	}

	HandlerStat statDiff = HandlerStat() - ctx.stat.allocStat;
	ser << ' ' << statDiff.GetTimeElapsed() << "us"_sv;

	if (allocDebug_) {
		ser << " |  allocs: "_sv << statDiff.GetAllocsCnt() << ", allocated: " << statDiff.GetAllocsBytes() << " byte(s)";
	}

	logger_.info("{}", ser.Slice());
}

Error RPCServer::OpenNamespace(cproto::Context &ctx, p_string nsDefJson) {
	NamespaceDef nsDef;

	nsDef.FromJSON(giftStr(nsDefJson));
	if (!nsDef.indexes.empty()) {
		return getDB(ctx, kRoleDataRead).AddNamespace(nsDef);
	}
	return getDB(ctx, kRoleDataRead).OpenNamespace(nsDef.name, nsDef.storage);
}

Error RPCServer::DropNamespace(cproto::Context &ctx, p_string ns) {
	//
	return getDB(ctx, kRoleDBAdmin).DropNamespace(ns);
}

Error RPCServer::TruncateNamespace(cproto::Context &ctx, p_string ns) { return getDB(ctx, kRoleDBAdmin).TruncateNamespace(ns); }

Error RPCServer::RenameNamespace(cproto::Context &ctx, p_string srcNsName, p_string dstNsName) {
	return getDB(ctx, kRoleDBAdmin).RenameNamespace(srcNsName, dstNsName.toString());
}

Error RPCServer::CloseNamespace(cproto::Context &ctx, p_string ns) {
	// Do not close.
	// TODO: add reference counters
	// return getDB(ctx, kRoleDataRead)->CloseNamespace(ns);
	return getDB(ctx, kRoleDataRead).Commit(ns);
}

Error RPCServer::EnumNamespaces(cproto::Context &ctx, cproto::optional<int> opts, cproto::optional<p_string> filter) {
	vector<NamespaceDef> nsDefs;
	EnumNamespacesOpts eopts;
	if (opts.hasValue()) eopts.options_ = opts.value();
	if (filter.hasValue()) eopts.filter_ = filter.value();

	auto err = getDB(ctx, kRoleDataRead).EnumNamespaces(nsDefs, eopts);
	if (!err.ok()) {
		return err;
	}
	WrSerializer ser;
	ser << "{\"items\":[";
	for (unsigned i = 0; i < nsDefs.size(); i++) {
		if (i != 0) ser << ',';
		nsDefs[i].GetJSON(ser);
	}
	ser << "]}";
	auto resSlice = ser.Slice();

	ctx.Return({cproto::Arg(p_string(&resSlice))});
	return errOK;
}

Error RPCServer::EnumDatabases(cproto::Context &ctx) {
	auto dbList = dbMgr_.EnumDatabases();

	WrSerializer ser;
	JsonBuilder jb(ser);
	span<string> array(&dbList[0], dbList.size());
	jb.Array("databases"_sv, array);
	jb.End();

	auto resSlice = ser.Slice();
	ctx.Return({cproto::Arg(p_string(&resSlice))});
	return errOK;
}

Error RPCServer::AddIndex(cproto::Context &ctx, p_string ns, p_string indexDef) {
	IndexDef iDef;
	auto err = iDef.FromJSON(giftStr(indexDef));
	if (!err.ok()) {
		return err;
	}
	return getDB(ctx, kRoleDBAdmin).AddIndex(ns, iDef);
}

Error RPCServer::UpdateIndex(cproto::Context &ctx, p_string ns, p_string indexDef) {
	IndexDef iDef;
	auto err = iDef.FromJSON(giftStr(indexDef));
	if (!err.ok()) {
		return err;
	}
	return getDB(ctx, kRoleDBAdmin).UpdateIndex(ns, iDef);
}

Error RPCServer::DropIndex(cproto::Context &ctx, p_string ns, p_string index) {
	IndexDef idef(index.toString());
	return getDB(ctx, kRoleDBAdmin).DropIndex(ns, idef);
}

Error RPCServer::SetSchema(cproto::Context &ctx, p_string ns, p_string schema) {
	return getDB(ctx, kRoleDBAdmin).SetSchema(ns, string_view(schema));
}

Error RPCServer::StartTransaction(cproto::Context &ctx, p_string nsName) {
	int64_t id = -1;
	try {
		id = addTx(ctx, nsName);
	} catch (reindexer::Error &e) {
		return e;
	}
	ctx.Return({cproto::Arg(id)});
	return errOK;
}

Error RPCServer::AddTxItem(cproto::Context &ctx, int format, p_string itemData, int mode, p_string perceptsPack, int stateToken,
						   int64_t txID) {
	Transaction &tr = getTx(ctx, txID);

	auto item = tr.NewItem();
	Error err;
	if (!item.Status().ok()) {
		return item.Status();
	}

	err = processTxItem(static_cast<DataFormat>(format), itemData, item, static_cast<ItemModifyMode>(mode), stateToken);
	if (err.code() == errTagsMissmatch) {
		item = getDB(ctx, kRoleDataWrite).NewItem(tr.GetName());
		if (item.Status().ok()) {
			err = processTxItem(static_cast<DataFormat>(format), itemData, item, static_cast<ItemModifyMode>(mode), stateToken);
		} else {
			return item.Status();
		}
	}
	if (!err.ok()) {
		return err;
	}

	if (perceptsPack.length()) {
		Serializer ser(perceptsPack);
		uint64_t preceptsCount = ser.GetVarUint();
		vector<string> precepts;
		precepts.reserve(preceptsCount);
		for (unsigned prIndex = 0; prIndex < preceptsCount; prIndex++) {
			precepts.emplace_back(ser.GetVString());
		}
		item.SetPrecepts(precepts);
	}
	tr.Modify(std::move(item), ItemModifyMode(mode));

	return err;
}

Error RPCServer::DeleteQueryTx(cproto::Context &ctx, p_string queryBin, int64_t txID) {
	auto db = getDB(ctx, kRoleDataWrite);

	Transaction &tr = getTx(ctx, txID);
	Query query;
	Serializer ser(queryBin.data(), queryBin.size());
	query.Deserialize(ser);
	query.type_ = QueryDelete;
	tr.Modify(std::move(query));
	return errOK;
}

Error RPCServer::UpdateQueryTx(cproto::Context &ctx, p_string queryBin, int64_t txID) {
	auto db = getDB(ctx, kRoleDataWrite);

	Transaction &tr = getTx(ctx, txID);
	Query query;
	Serializer ser(queryBin.data(), queryBin.size());
	query.Deserialize(ser);
	query.type_ = QueryUpdate;
	tr.Modify(std::move(query));
	return errOK;
}

Error RPCServer::CommitTx(cproto::Context &ctx, int64_t txId, cproto::optional<int> flagsOpts) {
	auto db = getDB(ctx, kRoleDataWrite);

	Transaction &tr = getTx(ctx, txId);
	QueryResults qres;
	auto err = db.CommitTransaction(tr, qres);
	if (err.ok()) {
		int32_t ptVers = -1;
		ResultFetchOpts opts;
		int flags;
		if (flagsOpts.hasValue()) {
			flags = flagsOpts.value();
		} else {
			flags = kResultsWithItemID;
			if (tr.IsTagsUpdated()) flags |= kResultsWithPayloadTypes;
		}
		if (tr.IsTagsUpdated()) {
			opts = ResultFetchOpts{flags, span<int32_t>(&ptVers, 1), 0, INT_MAX};
		} else {
			opts = ResultFetchOpts{flags, {}, 0, INT_MAX};
		}
		err = sendResults(ctx, qres, -1, opts);
	}
	clearTx(ctx, txId);
	return err;
}

Error RPCServer::RollbackTx(cproto::Context &ctx, int64_t txId) {
	auto db = getDB(ctx, kRoleDataWrite);

	Transaction &tr = getTx(ctx, txId);
	auto err = db.RollBackTransaction(tr);
	clearTx(ctx, txId);
	return err;
}

Error RPCServer::ModifyItem(cproto::Context &ctx, p_string ns, int format, p_string itemData, int mode, p_string perceptsPack,
							int stateToken, int /*txID*/) {
	using std::chrono::steady_clock;
	using std::chrono::milliseconds;
	using std::chrono::duration_cast;

	auto db = getDB(ctx, kRoleDataWrite);
	auto execTimeout = ctx.call->execTimeout_;
	auto beginT = steady_clock::now();
	auto item = Item(db.NewItem(ns));
	if (execTimeout.count() > 0) {
		execTimeout -= duration_cast<milliseconds>(beginT - steady_clock::now());
		if (execTimeout.count() <= 0) {
			return errCanceled;
		}
	}
	bool tmUpdated = false, sendItemBack = false;
	Error err;
	if (!item.Status().ok()) {
		return item.Status();
	}

	switch (format) {
		case FormatJson:
			err = item.Unsafe().FromJSON(itemData, nullptr, mode == ModeDelete);
			break;
		case FormatCJson:
			if (item.GetStateToken() != stateToken) {
				err = Error(errStateInvalidated, "stateToken mismatch:  %08X, need %08X. Can't process item", stateToken,
							item.GetStateToken());
			} else {
				err = item.Unsafe().FromCJSON(itemData, mode == ModeDelete);
			}
			break;
		case FormatMsgPack: {
			size_t offset = 0;
			err = item.FromMsgPack(itemData, offset);
			break;
		}
		default:
			err = Error(-1, "Invalid source item format %d", format);
	}
	if (!err.ok()) {
		return err;
	}
	tmUpdated = item.IsTagsUpdated();

	if (perceptsPack.length()) {
		Serializer ser(perceptsPack);
		unsigned preceptsCount = ser.GetVarUint();
		vector<string> precepts;
		for (unsigned prIndex = 0; prIndex < preceptsCount; prIndex++) {
			string precept(ser.GetVString());
			precepts.push_back(precept);
		}
		item.SetPrecepts(precepts);
		if (preceptsCount) sendItemBack = true;
	}
	QueryResults qres;
	if (sendItemBack) {
		err = db.WithTimeout(execTimeout).RegisterQueryResults(ns, qres);
		if (!err.ok()) {
			return err;
		}
	}
	switch (mode) {
		case ModeUpsert:
			err = db.WithTimeout(execTimeout).Upsert(ns, item);
			break;
		case ModeInsert:
			err = db.WithTimeout(execTimeout).Insert(ns, item);
			break;
		case ModeUpdate:
			err = db.WithTimeout(execTimeout).Update(ns, item);
			break;
		case ModeDelete:
			err = db.WithTimeout(execTimeout).Delete(ns, item);
			break;
	}
	if (!err.ok()) {
		return err;
	}
	qres.AddItem(item, sendItemBack);
	int32_t ptVers = -1;
	ResultFetchOpts opts;
	if (tmUpdated) {
		opts = ResultFetchOpts{kResultsWithItemID | kResultsWithPayloadTypes, span<int32_t>(&ptVers, 1), 0, INT_MAX};
	} else {
		opts = ResultFetchOpts{kResultsWithItemID, {}, 0, INT_MAX};
	}
	if (sendItemBack) {
		if (format == FormatMsgPack) {
			opts.flags |= kResultsMsgPack;
		} else {
			opts.flags |= kResultsCJson;
		}
	}

	return sendResults(ctx, qres, -1, opts);
}

Error RPCServer::DeleteQuery(cproto::Context &ctx, p_string queryBin, cproto::optional<int> flagsOpts) {
	Query query;
	Serializer ser(queryBin.data(), queryBin.size());
	query.Deserialize(ser);
	query.type_ = QueryDelete;

	QueryResults qres;
	auto err = getDB(ctx, kRoleDataWrite).Delete(query, qres);
	if (!err.ok()) {
		return err;
	}
	int flags = kResultsWithItemID;
	if (flagsOpts.hasValue()) {
		flags = flagsOpts.value();
	}
	ResultFetchOpts opts{flags, {}, 0, INT_MAX};
	return sendResults(ctx, qres, -1, opts);
}

Error RPCServer::UpdateQuery(cproto::Context &ctx, p_string queryBin, cproto::optional<int> flagsOpts) {
	Query query;
	Serializer ser(queryBin.data(), queryBin.size());
	query.Deserialize(ser);
	query.type_ = QueryUpdate;

	QueryResults qres;
	auto err = getDB(ctx, kRoleDataWrite).Update(query, qres);
	if (!err.ok()) {
		return err;
	}

	int32_t ptVersion = -1;
	int flags = kResultsWithItemID | kResultsWithPayloadTypes | kResultsCJson;
	if (flagsOpts.hasValue()) flags = flagsOpts.value();
	ResultFetchOpts opts{flags, {&ptVersion, 1}, 0, INT_MAX};
	return sendResults(ctx, qres, -1, opts);
}

Reindexer RPCServer::getDB(cproto::Context &ctx, UserRole role) {
	auto rawClientData = ctx.GetClientData();
	if (rawClientData) {
		auto clientData = dynamic_cast<RPCClientData *>(rawClientData);
		if (clientData) {
			Reindexer *db = nullptr;
			auto status = clientData->auth.GetDB(role, &db);
			if (!status.ok()) {
				throw status;
			}
			if (db != nullptr) {
				return db->NeedTraceActivity() ? db->WithTimeout(ctx.call->execTimeout_)
													 .WithActivityTracer(ctx.clientAddr, clientData->auth.Login(), clientData->connID)
											   : db->WithTimeout(ctx.call->execTimeout_);
			}
		}
	}
	throw Error(errParams, "Database is not opened, you should open it first");
}

Error RPCServer::sendResults(cproto::Context &ctx, QueryResults &qres, int reqId, const ResultFetchOpts &opts) {
	WrResultSerializer rser(opts);
	bool doClose = rser.PutResults(&qres);
	if (doClose && reqId >= 0) {
		freeQueryResults(ctx, reqId);
		reqId = -1;
	}
	string_view resSlice = rser.Slice();
	ctx.Return({cproto::Arg(p_string(&resSlice)), cproto::Arg(int(reqId))});
	return errOK;
}

Error RPCServer::processTxItem(DataFormat format, string_view itemData, Item &item, ItemModifyMode mode, int stateToken) const noexcept {
	switch (format) {
		case FormatJson:
			return item.FromJSON(itemData, nullptr, mode == ModeDelete);
		case FormatCJson:
			if (item.GetStateToken() != stateToken) {
				return Error(errStateInvalidated, "stateToken mismatch:  %08X, need %08X. Can't process item", stateToken,
							 item.GetStateToken());
			} else {
				return item.FromCJSON(itemData, mode == ModeDelete);
			}
		case FormatMsgPack: {
			size_t offset = 0;
			return item.FromMsgPack(itemData, offset);
		}
		default:
			return Error(-1, "Invalid source item format %d", format);
	}
}

QueryResults &RPCServer::getQueryResults(cproto::Context &ctx, int &id) {
	auto data = getClientDataSafe(ctx);

	if (id < 0) {
		for (id = 0; id < int(data->results.size()); id++) {
			if (!data->results[id].second) {
				data->results[id] = {QueryResults(), true};
				return data->results[id].first;
			}
		}

		if (data->results.size() > cproto::kMaxConcurentQueries) throw Error(errLogic, "Too many paralell queries");
		id = data->results.size();
		data->results.push_back({QueryResults(), true});
	}

	if (id >= int(data->results.size())) {
		throw Error(errLogic, "Invalid query id");
	}
	return data->results[id].first;
}

Transaction &RPCServer::getTx(cproto::Context &ctx, int64_t id) {
	auto data = getClientDataSafe(ctx);

	if (size_t(id) >= data->txs.size() || data->txs[id].IsFree()) {
		throw Error(errLogic, "Invalid tx id");
	}
	return data->txs[id];
}

int64_t RPCServer::addTx(cproto::Context &ctx, string_view nsName) {
	auto db = getDB(ctx, kRoleDataWrite);
	int64_t id = -1;
	auto data = getClientDataSafe(ctx);
	for (size_t i = 0; i < data->txs.size(); ++i) {
		if (data->txs[i].IsFree()) {
			id = i;
			break;
		}
	}
	if (data->txs.size() >= kMaxTxCount && id < 0) {
		throw Error(errForbidden, "Too many active transactions");
	}

	auto tr = db.NewTransaction(nsName);
	if (!tr.Status().ok()) {
		throw tr.Status();
	}
	assert(data->txStats);
	data->txStats->txCount += 1;
	if (id >= 0) {
		data->txs[id] = std::move(tr);
		return id;
	}
	data->txs.emplace_back(std::move(tr));
	return int64_t(data->txs.size() - 1);
}
void RPCServer::clearTx(cproto::Context &ctx, uint64_t txId) {
	auto data = getClientDataSafe(ctx);
	if (txId >= data->txs.size()) {
		throw Error(errLogic, "Invalid tx id %d", txId);
	}
	assert(data->txStats);
	data->txStats->txCount -= 1;
	data->txs[txId] = Transaction();
}

void RPCServer::freeQueryResults(cproto::Context &ctx, int id) {
	auto data = getClientDataSafe(ctx);
	if (id >= int(data->results.size()) || id < 0) {
		throw Error(errLogic, "Invalid query id");
	}
	data->results[id] = {QueryResults(), false};
}

static h_vector<int32_t, 4> pack2vec(p_string pack) {
	// Get array of payload Type Versions
	Serializer ser(pack.data(), pack.size());
	h_vector<int32_t, 4> vec;
	int cnt = ser.GetVarUint();
	for (int i = 0; i < cnt; i++) vec.push_back(ser.GetVarUint());
	return vec;
}

Error RPCServer::Select(cproto::Context &ctx, p_string queryBin, int flags, int limit, p_string ptVersionsPck) {
	Query query;
	Serializer ser(queryBin);
	query.Deserialize(ser);

	if (query.IsWALQuery()) {
		auto data = getClientDataSafe(ctx);
		query.Where(string("#slave_version"_sv), CondEq, data->rxVersion.StrippedString());
	}

	int id = -1;
	QueryResults &qres = getQueryResults(ctx, id);

	auto ret = getDB(ctx, kRoleDataRead).Select(query, qres);
	if (!ret.ok()) {
		freeQueryResults(ctx, id);
		return ret;
	}
	auto ptVersions = pack2vec(ptVersionsPck);
	ResultFetchOpts opts{flags, ptVersions, 0, unsigned(limit)};

	return fetchResults(ctx, id, opts);
}

Error RPCServer::SelectSQL(cproto::Context &ctx, p_string querySql, int flags, int limit, p_string ptVersionsPck) {
	int id = -1;
	QueryResults &qres = getQueryResults(ctx, id);
	auto ret = getDB(ctx, kRoleDataRead).Select(querySql, qres);
	if (!ret.ok()) {
		freeQueryResults(ctx, id);
		return ret;
	}
	auto ptVersions = pack2vec(ptVersionsPck);
	ResultFetchOpts opts{flags, ptVersions, 0, unsigned(limit)};

	return fetchResults(ctx, id, opts);
}

Error RPCServer::FetchResults(cproto::Context &ctx, int reqId, int flags, int offset, int limit) {
	flags &= ~kResultsWithPayloadTypes;

	ResultFetchOpts opts = {flags, {}, unsigned(offset), unsigned(limit)};
	return fetchResults(ctx, reqId, opts);
}

Error RPCServer::CloseResults(cproto::Context &ctx, int reqId) {
	freeQueryResults(ctx, reqId);
	return errOK;
}

Error RPCServer::fetchResults(cproto::Context &ctx, int reqId, const ResultFetchOpts &opts) {
	QueryResults &qres = getQueryResults(ctx, reqId);

	return sendResults(ctx, qres, reqId, opts);
}

Error RPCServer::GetSQLSuggestions(cproto::Context &ctx, p_string query, int pos) {
	vector<string> suggests;
	Error err = getDB(ctx, kRoleDataRead).GetSqlSuggestions(query, pos, suggests);

	if (err.ok()) {
		cproto::Args ret;
		ret.reserve(suggests.size());
		for (auto &suggest : suggests) ret.push_back(cproto::Arg(suggest));
		ctx.Return(ret);
	}
	return err;
}

Error RPCServer::Commit(cproto::Context &ctx, p_string ns) { return getDB(ctx, kRoleDataWrite).Commit(ns); }

Error RPCServer::GetMeta(cproto::Context &ctx, p_string ns, p_string key) {
	string data;
	auto err = getDB(ctx, kRoleDataRead).GetMeta(ns, key.toString(), data);
	if (!err.ok()) {
		return err;
	}

	ctx.Return({cproto::Arg(data)});
	return errOK;
}

Error RPCServer::PutMeta(cproto::Context &ctx, p_string ns, p_string key, p_string data) {
	return getDB(ctx, kRoleDataWrite).PutMeta(ns, key.toString(), data);
}

Error RPCServer::EnumMeta(cproto::Context &ctx, p_string ns) {
	vector<string> keys;
	auto err = getDB(ctx, kRoleDataWrite).EnumMeta(ns, keys);
	if (!err.ok()) {
		return err;
	}
	cproto::Args ret;
	for (auto &key : keys) {
		ret.push_back(cproto::Arg(key));
	}
	ctx.Return(ret);
	return errOK;
}

Error RPCServer::SubscribeUpdates(cproto::Context &ctx, int flag, cproto::optional<p_string> filterJson, cproto::optional<int> options) {
	UpdatesFilters filters;
	Error ret;
	if (filterJson.hasValue()) {
		filters.FromJSON(giftStr(filterJson.value()));
		if (!ret.ok()) {
			return ret;
		}
	}
	SubscriptionOpts opts;
	if (options.hasValue()) {
		opts.options = options.value();
	}

	auto db = getDB(ctx, kRoleDataRead);
	auto clientData = getClientDataSafe(ctx);
	if (flag) {
		ret = db.SubscribeUpdates(&clientData->pusher, filters, opts);
	} else {
		ret = db.UnsubscribeUpdates(&clientData->pusher);
	}
	if (ret.ok()) clientData->subscribed = bool(flag);
	return ret;
}

bool RPCServer::Start(const string &addr, ev::dynamic_loop &loop, bool enableStat, size_t maxUpdatesSize) {
	dispatcher_.Register(cproto::kCmdPing, this, &RPCServer::Ping);
	dispatcher_.Register(cproto::kCmdLogin, this, &RPCServer::Login, true);
	dispatcher_.Register(cproto::kCmdOpenDatabase, this, &RPCServer::OpenDatabase, true);
	dispatcher_.Register(cproto::kCmdCloseDatabase, this, &RPCServer::CloseDatabase);
	dispatcher_.Register(cproto::kCmdDropDatabase, this, &RPCServer::DropDatabase);
	dispatcher_.Register(cproto::kCmdOpenNamespace, this, &RPCServer::OpenNamespace);
	dispatcher_.Register(cproto::kCmdDropNamespace, this, &RPCServer::DropNamespace);
	dispatcher_.Register(cproto::kCmdTruncateNamespace, this, &RPCServer::TruncateNamespace);
	dispatcher_.Register(cproto::kCmdRenameNamespace, this, &RPCServer::RenameNamespace);
	dispatcher_.Register(cproto::kCmdCloseNamespace, this, &RPCServer::CloseNamespace);
	dispatcher_.Register(cproto::kCmdEnumNamespaces, this, &RPCServer::EnumNamespaces, true);
	dispatcher_.Register(cproto::kCmdEnumDatabases, this, &RPCServer::EnumDatabases);

	dispatcher_.Register(cproto::kCmdAddIndex, this, &RPCServer::AddIndex);
	dispatcher_.Register(cproto::kCmdUpdateIndex, this, &RPCServer::UpdateIndex);
	dispatcher_.Register(cproto::kCmdDropIndex, this, &RPCServer::DropIndex);
	dispatcher_.Register(cproto::kCmdSetSchema, this, &RPCServer::SetSchema);
	dispatcher_.Register(cproto::kCmdCommit, this, &RPCServer::Commit);

	dispatcher_.Register(cproto::kCmdStartTransaction, this, &RPCServer::StartTransaction);
	dispatcher_.Register(cproto::kCmdAddTxItem, this, &RPCServer::AddTxItem);
	dispatcher_.Register(cproto::kCmdDeleteQueryTx, this, &RPCServer::DeleteQueryTx);
	dispatcher_.Register(cproto::kCmdUpdateQueryTx, this, &RPCServer::UpdateQueryTx);
	dispatcher_.Register(cproto::kCmdCommitTx, this, &RPCServer::CommitTx, true);
	dispatcher_.Register(cproto::kCmdRollbackTx, this, &RPCServer::RollbackTx);

	dispatcher_.Register(cproto::kCmdModifyItem, this, &RPCServer::ModifyItem);
	dispatcher_.Register(cproto::kCmdDeleteQuery, this, &RPCServer::DeleteQuery, true);
	dispatcher_.Register(cproto::kCmdUpdateQuery, this, &RPCServer::UpdateQuery, true);

	dispatcher_.Register(cproto::kCmdSelect, this, &RPCServer::Select);
	dispatcher_.Register(cproto::kCmdSelectSQL, this, &RPCServer::SelectSQL);
	dispatcher_.Register(cproto::kCmdFetchResults, this, &RPCServer::FetchResults);
	dispatcher_.Register(cproto::kCmdCloseResults, this, &RPCServer::CloseResults);

	dispatcher_.Register(cproto::kCmdGetSQLSuggestions, this, &RPCServer::GetSQLSuggestions);

	dispatcher_.Register(cproto::kCmdGetMeta, this, &RPCServer::GetMeta);
	dispatcher_.Register(cproto::kCmdPutMeta, this, &RPCServer::PutMeta);
	dispatcher_.Register(cproto::kCmdEnumMeta, this, &RPCServer::EnumMeta);
	dispatcher_.Register(cproto::kCmdSubscribeUpdates, this, &RPCServer::SubscribeUpdates, true);
	dispatcher_.Middleware(this, &RPCServer::CheckAuth);
	dispatcher_.OnClose(this, &RPCServer::OnClose);
	dispatcher_.OnResponse(this, &RPCServer::OnResponse);

	if (logger_) {
		dispatcher_.Logger(this, &RPCServer::Logger);
	}

	listener_.reset(new Listener(loop, cproto::ServerConnection::NewFactory(dispatcher_, enableStat, maxUpdatesSize)));
	return listener_->Bind(addr);
}

RPCClientData::~RPCClientData() {
	Reindexer *db = nullptr;
	auth.GetDB(kRoleNone, &db);
	if (subscribed && db) {
		db->UnsubscribeUpdates(&pusher);
	}
}

}  // namespace reindexer_server
