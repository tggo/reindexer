#pragma once
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include "core/indexdef.h"
#include "core/indexopts.h"
#include "core/keyvalue/geometry.h"
#include "gtests/tests/gtest_cout.h"
#include "tools/errors.h"
#include "tools/stringstools.h"
#include "vendor/utf8cpp/utf8.h"

typedef std::tuple<const char *, const char *, const char *, IndexOpts, int64_t> IndexDeclaration;

template <typename DB>
class ReindexerTestApi {
public:
	using ItemType = typename DB::ItemT;
	using QueryResultsType = typename DB::QueryResultsT;

	ReindexerTestApi() : reindexer(std::shared_ptr<DB>(new DB)) {}
	void DefineNamespaceDataset(const std::string &ns, std::initializer_list<const IndexDeclaration> fields) {
		auto err = reindexer::Error();
		for (auto field : fields) {
			std::string indexName = std::get<0>(field);
			std::string fieldType = std::get<1>(field);
			std::string indexType = std::get<2>(field);
			int64_t expireAfter = std::get<4>(field);

			if (indexType != "composite") {
				err = reindexer->AddIndex(ns, {indexName, {indexName}, fieldType, indexType, std::get<3>(field)});
			} else {
				std::string realName = indexName;
				std::string idxContents = indexName;
				auto eqPos = indexName.find_first_of('=');
				if (eqPos != std::string::npos) {
					idxContents = indexName.substr(0, eqPos);
					realName = indexName.substr(eqPos + 1);
				}
				reindexer::JsonPaths jsonPaths;
				jsonPaths = reindexer::split(idxContents, "+", true, jsonPaths);

				err = reindexer->AddIndex(ns, {realName, jsonPaths, fieldType, indexType, std::get<3>(field), expireAfter});
			}
			ASSERT_TRUE(err.ok()) << err.what();
		}
		err = reindexer->Commit(ns);
		ASSERT_TRUE(err.ok()) << err.what();
	}

	reindexer::Error RegisterQueryResultsInNs(reindexer::string_view ns, reindexer::QueryResults &qr) {
		return reindexer->RegisterQueryResults(ns, qr);
	}
	ItemType NewItem(const std::string &ns) { return reindexer->NewItem(ns); }
	reindexer::Error Commit(const std::string &ns) { return reindexer->Commit(ns); }
	void Upsert(const std::string &ns, ItemType &item) {
		assert(!!item);
		auto err = reindexer->Upsert(ns, item);
		ASSERT_TRUE(err.ok()) << err.what();
	}
	void Upsert(const std::string &ns, ItemType &item, std::function<void(const reindexer::Error &)> cmpl) {
		assert(!!item);
		auto err = reindexer->WithCompletion(cmpl).Upsert(ns, item);
		ASSERT_TRUE(err.ok()) << err.what();
	}
	void PrintQueryResults(const std::string &ns, const QueryResultsType &res) {
		if (!verbose) return;
		{
			ItemType rdummy(reindexer->NewItem(ns));
			std::string outBuf;
			for (auto idx = 1; idx < rdummy.NumFields(); idx++) {
				outBuf += "\t";
				auto sv = rdummy[idx].Name();
				outBuf.append(sv.begin(), sv.end());
			}
			TestCout() << outBuf << std::endl;
		}

		for (auto it : res) {
			ItemType ritem(it.GetItem());
			std::string outBuf = "";
			for (auto idx = 1; idx < ritem.NumFields(); idx++) {
				outBuf += "\t";
				outBuf += ritem[idx].template As<std::string>();
			}
			TestCout() << outBuf << std::endl;
		}
		TestCout() << std::endl;
	}
	std::string PrintItem(ItemType &item) {
		std::string outBuf = "";
		for (auto idx = 1; idx < item.NumFields(); idx++) {
			outBuf += string(item[idx].Name()) + "=";
			outBuf += item[idx].template As<std::string>() + " ";
		}
		return outBuf;
	}
	std::string RandString() {
		std::string res;
		uint8_t len = rand() % 4 + 4;
		res.resize(len);
		for (int i = 0; i < len; ++i) {
			int f = rand() % letters.size();
			res[i] = letters[f];
		}
		return res;
	}
	std::string RandLikePattern() {
		std::string res;
		const uint8_t len = rand() % 4 + 4;
		for (uint8_t i = 0; i < len;) {
			if (rand() % 3 == 0) {
				res += '%';
				const uint8_t skipLen = rand() % (len - i + 1);
				i += skipLen;
			} else {
				if (rand() % 3 == 0) {
					res += '_';
				} else {
					int f = rand() % letters.size();
					res += letters[f];
				}
				++i;
			}
		}
		return res;
	}
	std::string RuRandString() {
		std::string res;
		uint8_t len = rand() % 20 + 4;
		res.resize(len * 3);
		auto it = res.begin();
		for (int i = 0; i < len; ++i) {
			int f = rand() % ru_letters.size();
			it = utf8::append(ru_letters[f], it);
		}
		res.erase(it, res.end());
		return res;
	}
	std::vector<int> RandIntVector(size_t size, int start, int range) {
		std::vector<int> vec;
		vec.reserve(size);
		for (size_t i = 0; i < size; ++i) {
			vec.push_back(start + rand() % range);
		}
		return vec;
	}
	std::shared_ptr<DB> reindexer;

	double RandDouble(double min, double max, int points) noexcept {
		assert(points > 0);
		return min + (max - min) * (rand() % points) / static_cast<double>(points);
	}

	reindexer::Point RandPoint() noexcept {
		static constexpr int points = 100;
		static constexpr double range = 10.0;
		return {RandDouble(-range, range, points), RandDouble(-range, range, points)};
	}

private:
	const std::string letters = "abcdefghijklmnopqrstuvwxyz";
	const std::wstring ru_letters = L"абвгдеёжзийклмнопрстуфхцчшщъыьэюя";
	bool verbose = false;
};
