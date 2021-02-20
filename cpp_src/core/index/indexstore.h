#pragma once

#include "core/index/index.h"
#include "core/index/string_map.h"

namespace reindexer {

template <typename T>
class IndexStore : public Index {
public:
	IndexStore(const IndexStore &);
	IndexStore(IndexStore &&);
	~IndexStore() override { assert(expiredStrings_.empty()); }
	IndexStore(const IndexDef &idef, const PayloadType payloadType, const FieldsSet &fields) : Index(idef, payloadType, fields) {
		static T a;
		keyType_ = selectKeyType_ = Variant(a).Type();
	}

	Variant Upsert(const Variant &key, IdType id) override;
	void Upsert(VariantArray &result, const VariantArray &keys, IdType id, bool needUpsertEmptyValue) override;
	void Delete(const Variant &key, IdType id) override;
	void Delete(const VariantArray &keys, IdType id) override;
	SelectKeyResults SelectKey(const VariantArray &keys, CondType condition, SortType stype, Index::SelectOpts res_type,
							   BaseFunctionCtx::Ptr ctx, const RdxContext &) override;
	void Commit() override;
	void UpdateSortedIds(const UpdateSortedContext & /*ctx*/) override {}
	std::unique_ptr<Index> Clone() override;
	IndexMemStat GetMemStat() override;
	void RemoveExpiredStrings() override;

protected:
	unordered_str_map<int> str_map;
	h_vector<T> idx_data;

	IndexMemStat memStat_;
	vector<key_string> expiredStrings_;
	size_t expiredStringsMemStat_ = 0;
};

template <>
IndexStore<Point>::IndexStore(const IndexDef &, const PayloadType, const FieldsSet &);

std::unique_ptr<Index> IndexStore_New(const IndexDef &idef, const PayloadType payloadType, const FieldsSet &fields_);

}  // namespace reindexer
