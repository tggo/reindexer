#pragma once

#include "aggregationresult.h"
#include "core/item.h"
#include "core/payload/payloadvalue.h"
#include "core/rdxcontext.h"
#include "itemref.h"
#include "tools/serializer.h"

namespace reindexer {

class Schema;
class TagsMatcher;
class PayloadType;
class WrSerializer;
struct NsContext;
struct ResultFetchOpts;
class SelectFunctionsHolder;
class NamespaceImpl;

namespace joins {
class NamespaceResults;
class ItemIterator;
}  // namespace joins

/// QueryResults is an interface for iterating over documents, returned by Query from Reindexer.<br>
/// *Lifetime*: QueryResults uses Copy-On-Write semantics, so it has independent lifetime and state - e.g., acquired from Reindexer.
/// QueryResults cannot be externaly changed or deleted even in case of changing origin data in DB.<br>
/// *Thread safety*: QueryResults is thread safe.

class QueryResults {
public:
	QueryResults(int flags = 0);
	QueryResults(const ItemRefVector::const_iterator &b, const ItemRefVector::const_iterator &e);
	QueryResults(std::initializer_list<ItemRef> l);
	QueryResults(const QueryResults &) = delete;
	QueryResults(QueryResults &&);
	~QueryResults();
	QueryResults &operator=(const QueryResults &) = delete;
	QueryResults &operator=(QueryResults &&obj) noexcept;
	void Add(const ItemRef &);
	void AddItem(Item &item, bool withData = false);
	void Dump() const;
	void Erase(ItemRefVector::iterator begin, ItemRefVector::iterator end);
	size_t Count() const { return items_.size(); }
	size_t TotalCount() const { return totalCount; }
	const string &GetExplainResults() const { return explainResults; }
	const vector<AggregationResult> &GetAggregationResults() const { return aggregationResults; }
	void Clear();
	h_vector<string_view, 1> GetNamespaces() const;
	bool IsCacheEnabled() const { return !nonCacheableData; }

	class Iterator {
	public:
		Error GetJSON(WrSerializer &wrser, bool withHdrLen = true);
		Error GetCJSON(WrSerializer &wrser, bool withHdrLen = true);
		Error GetMsgPack(WrSerializer &wrser, bool withHdrLen = true);
		Error GetProtobuf(WrSerializer &wrser, bool withHdrLen = true);
		Item GetItem();
		joins::ItemIterator GetJoined();
		const ItemRef &GetItemRef() const { return qr_->items_[idx_]; }
		int64_t GetLSN() const { return qr_->items_[idx_].Value().GetLSN(); }
		bool IsRaw() const;
		string_view GetRaw() const;
		Iterator &operator++();
		Iterator &operator+(int delta);
		Error Status() { return err_; }
		bool operator!=(const Iterator &) const;
		bool operator==(const Iterator &) const;
		Iterator &operator*() { return *this; }

		const QueryResults *qr_;
		int idx_;
		Error err_;
	};

	Iterator begin() const { return Iterator{this, 0, errOK}; }
	Iterator end() const { return Iterator{this, int(items_.size()), errOK}; }
	Iterator operator[](int idx) const { return Iterator{this, idx, errOK}; }

	std::vector<joins::NamespaceResults> joined_;
	vector<AggregationResult> aggregationResults;
	int totalCount = 0;
	bool haveRank = false;
	bool nonCacheableData = false;
	bool needOutputRank = false;

	struct Context;
	// precalc context size
	static constexpr int kSizeofContext = 144;	// sizeof(void *) * 2 + sizeof(void *) * 3 + 32 + sizeof(void *) + sizeof(void *)*2;

	// Order of storing contexts for namespaces:
	// [0]      - main NS context
	// [1;N]    - contexts of all the merged namespaces
	// [N+1; M] - contexts of all the joined namespaces for all the merged namespaces:
	using ContextsVector = h_vector<Context, 1, kSizeofContext>;
	ContextsVector ctxs;

	void addNSContext(const PayloadType &type, const TagsMatcher &tagsMatcher, const FieldsSet &fieldsFilter,
					  std::shared_ptr<const Schema> schema);
	const TagsMatcher &getTagsMatcher(int nsid) const;
	const PayloadType &getPayloadType(int nsid) const;
	const FieldsSet &getFieldsFilter(int nsid) const;
	TagsMatcher &getTagsMatcher(int nsid);
	PayloadType &getPayloadType(int nsid);
	std::shared_ptr<const Schema> getSchema(int nsid) const;
	int getNsNumber(int nsid) const;
	int getMergedNSCount() const;
	ItemRefVector &Items() { return items_; }
	const ItemRefVector &Items() const { return items_; }
	int GetJoinedNsCtxIndex(int nsid) const;
	void AddNamespace(std::shared_ptr<NamespaceImpl> ns);
	void RemoveNamespace(const NamespaceImpl *ns);
	bool IsNamespaceAdded(const NamespaceImpl *ns) const noexcept {
		return std::find_if(namespaces_.cbegin(), namespaces_.cend(),
							[ns](const std::shared_ptr<NamespaceImpl> &ptr) { return ptr.get() == ns; }) != namespaces_.cend();
	}

	string explainResults;

protected:
	class EncoderDatasourceWithJoins;
	class EncoderAdditionalDatasource;

private:
	void encodeJSON(int idx, WrSerializer &ser) const;
	ItemRefVector items_;
	bool holdActivity_ = false;
	union {
		int noActivity_;
		RdxActivityContext activityCtx_;
	};
	friend InternalRdxContext;
	friend SelectFunctionsHolder;
	h_vector<std::shared_ptr<NamespaceImpl>, 1> namespaces_;
	vector<key_string> stringsHolder_;
};

}  // namespace reindexer
