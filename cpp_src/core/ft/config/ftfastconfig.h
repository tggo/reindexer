#pragma once

#include "baseftconfig.h"
#include "estl/h_vector.h"

namespace reindexer {

struct FtFastFieldConfig {
	double bm25Boost = 1.0;
	double bm25Weight = 0.1;
	double termLenBoost = 1.0;
	double termLenWeight = 0.3;
	double positionBoost = 1.0;
	double positionWeight = 0.1;
};

struct FtFastConfig : public BaseFTConfig {
	FtFastConfig(size_t fieldsCount) : fieldsCfg(fieldsCount ? fieldsCount : 1) {}
	void parse(string_view json, const fast_hash_map<string, int> &fields) final;

	double distanceBoost = 1.0;
	double distanceWeight = 0.5;
	double fullMatchBoost = 1.1;
	// Relevancy step of partial match: relevancy = kFullMatchProc - partialMatchDecrease * (non matched symbols) / (matched symbols)
	// For example: partialMatchDecrease: 15, word in index 'terminator', pattern 'termin'. matched: 6 symbols, unmatched: 4. relevancy =
	// 100 - (15*4)/6 = 80
	int partialMatchDecrease = 15;
	double minRelevancy = 0.05;

	int maxTyposInWord = 1;
	int maxTypoLen = 15;

	int maxRebuildSteps = 50;
	int maxStepSize = 4000;

	h_vector<FtFastFieldConfig, 8> fieldsCfg;
};

}  // namespace reindexer
