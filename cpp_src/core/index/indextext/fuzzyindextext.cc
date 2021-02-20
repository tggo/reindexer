#include <stdio.h>

#include "fuzzyindextext.h"
#include "tools/customlocal.h"
#include "tools/errors.h"
using std::make_shared;

namespace reindexer {
using std::wstring;
using search_engine::MergedData;

template <typename T>
std::unique_ptr<Index> FuzzyIndexText<T>::Clone() {
	return std::unique_ptr<Index>{new FuzzyIndexText<T>(*this)};
}

template <typename T>
IdSet::Ptr FuzzyIndexText<T>::Select(FtCtx::Ptr fctx, FtDSLQuery& dsl) {
	auto result = engine_.Search(dsl);

	auto mergedIds = make_intrusive<intrusive_atomic_rc_wrapper<IdSet>>();

	mergedIds->reserve(result.data_->size() * 2);
	fctx->Reserve(result.data_->size() * 2);
	double coof = 1;
	if (result.max_proc_ > 100) {
		coof = 100 / result.max_proc_;
	}
	for (auto it = result.data_->begin(); it != result.data_->end(); ++it) {
		it->proc_ *= coof;
		if (it->proc_ < GetConfig()->minOkProc) continue;
		assert(it->id_ < this->vdocs_.size());
		const auto& id_set = this->vdocs_[it->id_].keyEntry->Sorted(0);
		fctx->Add(id_set.begin(), id_set.end(), it->proc_);
		mergedIds->Append(id_set.begin(), id_set.end(), IdSet::Unordered);
	}

	return mergedIds;
}

template <typename T>
void FuzzyIndexText<T>::commitFulltext() {
	this->cache_ft_->Clear();
	vector<unique_ptr<string>> bufStrs;
	auto gt = this->Getter();
	for (auto& doc : this->idx_map) {
		auto res = gt.getDocFields(doc.first, bufStrs);
#ifdef REINDEX_FT_EXTRA_DEBUG
		string text(res[0].first);
		this->vdocs_.push_back({(text.length() > 48) ? text.substr(0, 48) + "..." : text, doc.second.get(), {}, {}});
#else
		this->vdocs_.push_back({doc.second.get(), {}, {}});
#endif
		for (auto& r : res) {
			engine_.AddData(r.first, this->vdocs_.size() - 1, r.second, this->cfg_->extraWordSymbols);
		}
	}
	engine_.Commit();
}
template <typename T>
FtFuzzyConfig* FuzzyIndexText<T>::GetConfig() const {
	return dynamic_cast<FtFuzzyConfig*>(this->cfg_.get());
}
template <typename T>
void FuzzyIndexText<T>::CreateConfig(const FtFuzzyConfig* cfg) {
	if (cfg) {
		this->cfg_.reset(new FtFuzzyConfig(*cfg));
		return;
	}
	this->cfg_.reset(new FtFuzzyConfig());
	this->cfg_->parse(this->opts_.config, this->ftFields_);
}

std::unique_ptr<Index> FuzzyIndexText_New(const IndexDef& idef, const PayloadType payloadType, const FieldsSet& fields) {
	switch (idef.Type()) {
		case IndexFuzzyFT:
			return std::unique_ptr<Index>{new FuzzyIndexText<unordered_str_map<FtKeyEntry>>(idef, payloadType, fields)};
		case IndexCompositeFuzzyFT:
			return std::unique_ptr<Index>{new FuzzyIndexText<unordered_payload_map<FtKeyEntry, true>>(idef, payloadType, fields)};
		default:
			abort();
	}
}

}  // namespace reindexer
