/*
 * (C) Copyright 2015 ETH Zurich Systems Group (http://www.systems.ethz.ch/) and others.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Contributors:
 *     Markus Pilman <mpilman@inf.ethz.ch>
 *     Simon Loesing <sloesing@inf.ethz.ch>
 *     Thomas Etter <etterth@gmail.com>
 *     Kevin Bocksrocker <kevin.bocksrocker@gmail.com>
 *     Lucas Braun <braunl@inf.ethz.ch>
 */
#include "Indexes.hpp"
#include <telldb/Exceptions.hpp>

using namespace tell::db;
using namespace tell::db::impl;

namespace bdtree {

template<>
struct null_key<tell::db::impl::KeyType> {
    static tell::db::impl::KeyType value() {
        return std::make_tuple(std::vector<Field>{},
                std::numeric_limits<uint64_t>::max(),
                0u);
    }
};
} // namespace bdtree

template<class Archiver>
struct size_policy<Archiver, crossbow::string> {
    size_t operator() (Archiver& ar, const crossbow::string& str) const {
        return 4 + str.size();
    }
};

template<class Archiver>
struct serialize_policy<Archiver, crossbow::string> {
    uint8_t* operator() (Archiver& ar, const crossbow::string& str, uint8_t* pos) const {
        uint32_t sz = str.size();
        memcpy(pos, &sz, sizeof(sz));
        pos += sizeof(sz);
        memcpy(pos, str.data(), sz);
        return pos + sz;
    }
};

template<class Archiver>
struct deserialize_policy<Archiver, crossbow::string> {
    const uint8_t* operator() (Archiver&, crossbow::string& out, const uint8_t* ptr) const
    {
        const std::uint32_t s = *reinterpret_cast<const std::uint32_t*>(ptr);
        out = crossbow::string(reinterpret_cast<const char*>(ptr + sizeof(std::uint32_t)), s);
        return ptr + sizeof(s) + s;
    }
};

template<class Archiver>
struct size_policy<Archiver, Field>
{
    size_t operator() (Archiver& ar, const Field& field) const
    {
        size_t res = 1;
        switch (field.type()) {
        case tell::store::FieldType::NOTYPE:
            return res;
        case tell::store::FieldType::NULLTYPE:
            return res;
        case tell::store::FieldType::SMALLINT:
            return res + 2;
        case tell::store::FieldType::INT:
            return res + 4;
        case tell::store::FieldType::BIGINT:
            return res + 8;
        case tell::store::FieldType::FLOAT:
            return res + 4;
        case tell::store::FieldType::DOUBLE:
            return res + 8;
        case tell::store::FieldType::TEXT:
        case tell::store::FieldType::BLOB:
            return res + 4 + boost::any_cast<const crossbow::string&>(field.value()).size();
        }
    }
};

template<class Archiver>
struct serialize_policy<Archiver, Field>
{
    uint8_t* operator() (Archiver& ar, const Field& field, uint8_t* pos) const {
        ar & field.type();
        switch (field.type()) {
        case tell::store::FieldType::NOTYPE:
        case tell::store::FieldType::NULLTYPE:
            break;
        case tell::store::FieldType::SMALLINT:
            ar & boost::any_cast<int16_t>(field.value());
            break;
        case tell::store::FieldType::INT:
            ar & boost::any_cast<int32_t>(field.value());
            break;
        case tell::store::FieldType::BIGINT:
            ar & boost::any_cast<int64_t>(field.value());
            break;
        case tell::store::FieldType::FLOAT:
            ar & boost::any_cast<float>(field.value());
            break;
        case tell::store::FieldType::DOUBLE:
            ar & boost::any_cast<double>(field.value());
            break;
        case tell::store::FieldType::TEXT:
        case tell::store::FieldType::BLOB:
            ar & boost::any_cast<const crossbow::string&>(field.value());
            break;
        }
        return ar.pos;
    }
};

template<class Archiver>
struct deserialize_policy<Archiver, Field>
{
    const uint8_t* operator() (Archiver& ar, Field& field, const uint8_t* ptr) const {
        tell::store::FieldType type;
        ar & type;
        switch (type) {
        case tell::store::FieldType::NOTYPE:
            break;
        case tell::store::FieldType::NULLTYPE:
            field = Field::createNull();
            break;
        case tell::store::FieldType::SMALLINT:
            {
                int16_t value;
                ar & value;
                field = Field::create(value);
            }
            break;
        case tell::store::FieldType::INT:
            {
                int32_t value;
                ar & value;
                field = Field::create(value);
            }
            break;
        case tell::store::FieldType::BIGINT:
            {
                int64_t value;
                ar & value;
                field = Field::create(value);
            }
            break;
        case tell::store::FieldType::FLOAT:
            {
                float value;
                ar & value;
                field = Field::create(value);
            }
            break;
        case tell::store::FieldType::DOUBLE:
            {
                double value;
                ar & value;
                field = Field::create(value);
            }
            break;
        case tell::store::FieldType::TEXT:
        case tell::store::FieldType::BLOB:
            {
                crossbow::string value;
                ar & value;
                field = Field::create(value);
            }
            break;
        }
        return ar.pos;
    }
};

namespace bdtree {

template class bdtree::map<tell::db::impl::KeyType, tell::db::impl::ValueType, tell::db::BdTreeBackend>;

} // namespace bdtree

namespace tell {
namespace db {
namespace impl {

bool IndexWrapper::Iterator::done() const {
    return cacheIter == cacheEnd && idxIter == idxEnd;
}

IndexWrapper::IndexWrapper(
        const std::vector<store::Schema::id_t>& fields,
        BdTreeBackend&& backend,
        IndexCache& cache,
        uint64_t txId,
        bool init)
    : mFields(fields)
    , mBackend(std::move(backend))
    , mBdTree(mBackend, cache, txId, init)
    , mTxId(txId)
{
}

auto IndexWrapper::lower_bound(const std::vector<Field>& key) -> Iterator {
    KeyType k{key, 0ul, 0u};
    return Iterator(
            mBdTree.find(k),
            mBdTree.end(),
            mCache.lower_bound(k),
            mCache.end());
}

void IndexWrapper::insert(key_t k, const Tuple& tuple) {
    mCache.emplace(KeyType{keyOf(tuple), std::numeric_limits<uint64_t>::max(), 0u}, std::make_pair(IndexOperation::Insert, k));
}

void IndexWrapper::update(key_t key, const Tuple& old, const Tuple& next) {
    auto oldKey = keyOf(old);
    auto newKey = keyOf(next);
    if (oldKey != newKey) {
        mCache.emplace(
                KeyType{
                    keyOf(old),
                    std::numeric_limits<uint64_t>::max(),
                    0u},
                std::make_pair(IndexOperation::Delete, key));
        mCache.emplace(
                KeyType{
                    keyOf(next),
                    std::numeric_limits<uint64_t>::max(),
                    0u},
                std::make_pair(IndexOperation::Insert, key));
    }
}

void IndexWrapper::remove(key_t key, const Tuple& tuple) {
    mCache.emplace(KeyType{keyOf(tuple), std::numeric_limits<uint64_t>::max(), 0u}, std::make_pair(IndexOperation::Delete, key));
}

std::vector<Field> IndexWrapper::keyOf(const Tuple& tuple) {
    std::vector<Field> key;
    key.reserve(mFields.size());
    for (auto f : mFields) {
        key.emplace_back(tuple[f]);
    }
    return key;
}

Indexes::Indexes(store::ClientHandle& handle) {
    auto tableRes = handle.getTable("__counter");
    if (tableRes->error()) {
        mCounterTable = RemoteCounter::createTable(handle, "__counter");
    } else {
        mCounterTable = std::make_shared<store::Table>(tableRes->get());
    }
}

std::unordered_map<crossbow::string, IndexWrapper>
Indexes::openIndexes(uint64_t txId, store::ClientHandle& handle, const store::Table& table) {
    std::unordered_map<crossbow::string, IndexWrapper> res;
    auto iter = mIndexes.find(table_t{table.tableId()});
    if (iter != mIndexes.end()) {
        for (auto& idx : iter->second) {
            res.emplace(idx.first,
                    IndexWrapper(
                        idx.second->fields,
                        BdTreeBackend(
                            handle,
                            idx.second->ptrTable,
                            idx.second->nodeTable),
                        mBdTreeCache,
                        txId,
                        false));
        }
        return res;
    }
    const auto& indexes = table.record().schema().indexes();
    std::vector<std::tuple<crossbow::string,
        const std::vector<store::Schema::id_t>*,
        std::shared_ptr<store::GetTableResponse>,
        std::shared_ptr<store::GetTableResponse>>> responses;
    for (const auto& idx : indexes) {
        crossbow::string nodeTableName = "__index_nodes_" + idx.first;
        crossbow::string ptrTableName = "__index_ptrs_" + idx.first;
        responses.emplace_back(std::make_tuple(idx.first,
                    &idx.second,
                    handle.getTable(nodeTableName),
                    handle.getTable(ptrTableName)));
    }
    std::unordered_map<crossbow::string, IndexTables*> indexMap;
    for (auto it = responses.rbegin(); it != responses.rend(); ++it) {
        {
            const auto& ec = std::get<2>(*it)->error();
            if (ec) {
                const auto& str = ec.message();
                throw OpenTableException(crossbow::string(str.c_str(), str.size()));
            }
        }
        {
            const auto& ec = std::get<2>(*it)->error();
            if (ec) {
                const auto& str = ec.message();
                throw OpenTableException(crossbow::string(str.c_str(), str.size()));
            }
        }
        auto insRes = indexMap.emplace(std::get<0>(*it),
                new IndexTables{
                    *std::get<1>(*it),
                    TableData(std::get<3>(*it)->get(), mCounterTable),
                    TableData(std::get<2>(*it)->get(), mCounterTable)
                });
        res.emplace(std::get<0>(*it),
                IndexWrapper(
                    *std::get<1>(*it),
                    BdTreeBackend(
                        handle,
                        insRes.first->second->ptrTable,
                        insRes.first->second->nodeTable),
                    mBdTreeCache,
                    txId,
                    false));
    }
    mIndexes.emplace(table_t{table.tableId()}, std::move(indexMap));
    return res;
}

std::unordered_map<crossbow::string, IndexWrapper>
Indexes::createIndexes(uint64_t txId, store::ClientHandle& handle, const store::Table& table) {
    std::unordered_map<crossbow::string, IndexWrapper> res;
    const auto& indexes = table.record().schema().indexes();
    std::unordered_map<crossbow::string, IndexTables*> indexMap;
    for (const auto& idx : indexes) {
        crossbow::string nodeTableName = "__index_nodes_" + idx.first;
        crossbow::string ptrTableName = "__index_ptrs_" + idx.first;
        auto insRes = indexMap.emplace(idx.first,
                new IndexTables{idx.second,
                                TableData(BdTreePointerTable::createTable(handle, ptrTableName), mCounterTable),
                                TableData(BdTreeNodeTable::createTable(handle, nodeTableName), mCounterTable)});
        res.emplace(idx.first,
                IndexWrapper(
                    idx.second,
                    BdTreeBackend(
                        handle,
                        insRes.first->second->ptrTable,
                        insRes.first->second->nodeTable),
                    mBdTreeCache,
                    txId,
                    true));
    }
    mIndexes.emplace(table_t{table.tableId()}, indexMap);
    return res;
}

} // namespace impl
} // namespace db
} // namespace tell

