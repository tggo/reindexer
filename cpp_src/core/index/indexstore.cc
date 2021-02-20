
#include "indexstore.h"
#include "core/rdxcontext.h"
#include "tools/errors.h"
#include "tools/logger.h"

namespace reindexer {

template <>
IndexStore<key_string>::IndexStore(const IndexStore &other)
	: Index{other}, str_map{other.str_map}, idx_data{other.idx_data}, memStat_{other.memStat_} {}

template <typename T>
IndexStore<T>::IndexStore(const IndexStore &) = default;

template <>
IndexStore<key_string>::IndexStore(IndexStore &&other)
	: Index{std::move(other)},
	  str_map{std::move(other.str_map)},
	  idx_data{std::move(other.idx_data)},
	  memStat_{std::move(other.memStat_)},
	  expiredStrings_{std::move(other.expiredStrings_)},
	  expiredStringsMemStat_{other.expiredStringsMemStat_} {
	other.expiredStringsMemStat_ = 0;
}

template <typename T>
IndexStore<T>::IndexStore(IndexStore &&) = default;

template <>
IndexStore<Point>::IndexStore(const IndexDef &idef, const PayloadType payloadType, const FieldsSet &fields)
	: Index(idef, payloadType, fields) {
	keyType_ = selectKeyType_ = KeyValueDouble;
	opts_.Array(true);
}

template <>
void IndexStore<key_string>::RemoveExpiredStrings() {
	memStat_.dataSize -= sizeof(vector<key_string>::value_type) * expiredStrings_.size() + expiredStringsMemStat_;
	expiredStringsMemStat_ = 0;
	expiredStrings_.clear();
}

template <typename T>
void IndexStore<T>::RemoveExpiredStrings() {}

template <>
void IndexStore<key_string>::Delete(const Variant &key, IdType id) {
	if (key.Type() == KeyValueNull) return;
	auto keyIt = str_map.find(string_view(key));
	// assertf(keyIt != str_map.end(), "Delete unexists key from index '%s' id=%d", name_, id);
	if (keyIt == str_map.end()) return;
	if (keyIt->second) keyIt->second--;
	if (!keyIt->second) {
		memStat_.dataSize -= sizeof(unordered_str_map<int>::value_type);
		memStat_.dataSize += sizeof(vector<key_string>::value_type);
		expiredStringsMemStat_ += sizeof(*keyIt->first.get()) + keyIt->first->heap_size();
		expiredStrings_.emplace_back(std::move(keyIt->first));
		str_map.template erase<no_deep_clean>(keyIt);
	}

	(void)id;
}
template <typename T>
void IndexStore<T>::Delete(const Variant & /*key*/, IdType /* id */) {}

template <typename T>
void IndexStore<T>::Delete(const VariantArray &keys, IdType id) {
	if (keys.empty()) {
		Delete(Variant{}, id);
	} else {
		for (const auto &key : keys) Delete(key, id);
	}
}

template <>
void IndexStore<Point>::Delete(const VariantArray & /*keys*/, IdType /*id*/) {
	assert(0);
}

template <>
Variant IndexStore<key_string>::Upsert(const Variant &key, IdType /*id*/) {
	if (key.Type() == KeyValueNull) return Variant();

	auto keyIt = str_map.find(string_view(key));
	if (keyIt == str_map.end()) {
		keyIt = str_map.emplace(static_cast<key_string>(key), 0).first;
		memStat_.dataSize += sizeof(unordered_str_map<int>::value_type) + sizeof(*keyIt->first.get()) + keyIt->first->heap_size();
	}
	keyIt->second++;

	return Variant(keyIt->first);
}

template <>
Variant IndexStore<PayloadValue>::Upsert(const Variant &key, IdType /*id*/) {
	return Variant(key);
}

template <typename T>
Variant IndexStore<T>::Upsert(const Variant &key, IdType id) {
	if (!opts_.IsArray() && !opts_.IsDense() && !opts_.IsSparse() && key.Type() != KeyValueNull) {
		idx_data.resize(std::max(id + 1, int(idx_data.size())));
		idx_data[id] = static_cast<T>(key);
	}
	return Variant(key);
}

template <typename T>
void IndexStore<T>::Upsert(VariantArray &result, const VariantArray &keys, IdType id, bool needUpsertEmptyValue) {
	if (keys.empty()) {
		if (needUpsertEmptyValue) {
			Upsert(Variant{}, id);
		}
	} else {
		result.reserve(keys.size());
		for (const auto &key : keys) result.emplace_back(Upsert(key, id));
	}
}

template <>
void IndexStore<Point>::Upsert(VariantArray & /*result*/, const VariantArray & /*keys*/, IdType /*id*/, bool /*needUpsertEmptyValue*/) {
	assert(0);
}

template <typename T>
void IndexStore<T>::Commit() {
	logPrintf(LogTrace, "IndexStore::Commit (%s) %d uniq strings", name_, str_map.size());
}

template <typename T>
SelectKeyResults IndexStore<T>::SelectKey(const VariantArray &keys, CondType condition, SortType /*sortId*/, Index::SelectOpts sopts,
										  BaseFunctionCtx::Ptr /*ctx*/, const RdxContext &rdxCtx) {
	const auto indexWard(rdxCtx.BeforeIndexWork());
	SelectKeyResult res;
	if (condition == CondEmpty && !this->opts_.IsArray() && !this->opts_.IsSparse())
		throw Error(errParams, "The 'is NULL' condition is suported only by 'sparse' or 'array' indexes");

	if (condition == CondAny && !this->opts_.IsArray() && !this->opts_.IsSparse() && !sopts.distinct)
		throw Error(errParams, "The 'NOT NULL' condition is suported only by 'sparse' or 'array' indexes");

	res.comparators_.push_back(Comparator(condition, KeyType(), keys, opts_.IsArray(), sopts.distinct, payloadType_, fields_,
										  idx_data.size() ? idx_data.data() : nullptr, opts_.collateOpts_));
	return SelectKeyResults(std::move(res));
}

template <typename T>
std::unique_ptr<Index> IndexStore<T>::Clone() {
	std::unique_ptr<Index> ret{new IndexStore<T>(*this)};
	std::swap(static_cast<IndexStore *>(ret.get())->expiredStrings_, this->expiredStrings_);
	return ret;
}

template <typename T>
IndexMemStat IndexStore<T>::GetMemStat() {
	IndexMemStat ret = memStat_;
	ret.name = name_;
	ret.uniqKeysCount = str_map.size();
	ret.columnSize = idx_data.size() * sizeof(T);
	return ret;
}

std::unique_ptr<Index> IndexStore_New(const IndexDef &idef, const PayloadType payloadType, const FieldsSet &fields) {
	switch (idef.Type()) {
		case IndexBool:
			return std::unique_ptr<Index>{new IndexStore<bool>(idef, payloadType, fields)};
		case IndexIntStore:
			return std::unique_ptr<Index>{new IndexStore<int>(idef, payloadType, fields)};
		case IndexInt64Store:
			return std::unique_ptr<Index>{new IndexStore<int64_t>(idef, payloadType, fields)};
		case IndexDoubleStore:
			return std::unique_ptr<Index>{new IndexStore<double>(idef, payloadType, fields)};
		case IndexStrStore:
			return std::unique_ptr<Index>{new IndexStore<key_string>(idef, payloadType, fields)};
		default:
			abort();
	}
}

template class IndexStore<bool>;
template class IndexStore<int>;
template class IndexStore<int64_t>;
template class IndexStore<double>;
template class IndexStore<key_string>;
template class IndexStore<PayloadValue>;
template class IndexStore<Point>;

}  // namespace reindexer
