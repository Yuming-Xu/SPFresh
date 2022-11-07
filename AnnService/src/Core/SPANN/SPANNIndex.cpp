// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inc/Core/SPANN/Index.h"
#include "inc/Helper/VectorSetReaders/MemoryReader.h"
#include "inc/Core/SPANN/ExtraFullGraphSearcher.h"
#include "inc/Core/SPANN/ExtraRocksDBController.h"
#include <shared_mutex>
#include <chrono>
#include <random>

#pragma warning(disable:4242)  // '=' : conversion from 'int' to 'short', possible loss of data
#pragma warning(disable:4244)  // '=' : conversion from 'int' to 'short', possible loss of data
#pragma warning(disable:4127)  // conditional expression is constant

namespace SPTAG
{
    namespace SPANN
    {
        std::atomic_int ExtraWorkSpace::g_spaceCount(0);
        EdgeCompare Selection::g_edgeComparer;

        std::function<std::shared_ptr<Helper::DiskPriorityIO>(void)> f_createAsyncIO = []() -> std::shared_ptr<Helper::DiskPriorityIO> { return std::shared_ptr<Helper::DiskPriorityIO>(new Helper::AsyncFileIO()); };

        template <typename T>
        bool Index<T>::CheckHeadIndexType() {
            SPTAG::VectorValueType v1 = m_index->GetVectorValueType(), v2 = GetEnumValueType<T>();
            if (v1 != v2) {
                LOG(Helper::LogLevel::LL_Error, "Head index and vectors don't have the same value types, which are %s %s\n",
                    SPTAG::Helper::Convert::ConvertToString(v1).c_str(),
                    SPTAG::Helper::Convert::ConvertToString(v2).c_str()
                );
                if (!SPTAG::COMMON::DistanceUtils::Quantizer) return false;
            }
            return true;
        }

        template <typename T>
        ErrorCode Index<T>::LoadConfig(Helper::IniReader& p_reader)
        {
            IndexAlgoType algoType = p_reader.GetParameter("Base", "IndexAlgoType", IndexAlgoType::Undefined);
            VectorValueType valueType = p_reader.GetParameter("Base", "ValueType", VectorValueType::Undefined);
            if ((m_index = CreateInstance(algoType, valueType)) == nullptr) return ErrorCode::FailedParseValue;

            std::string sections[] = { "Base", "SelectHead", "BuildHead", "BuildSSDIndex" };
            for (int i = 0; i < 4; i++) {
                auto parameters = p_reader.GetParameters(sections[i].c_str());
                for (auto iter = parameters.begin(); iter != parameters.end(); iter++) {
                    SetParameter(iter->first.c_str(), iter->second.c_str(), sections[i].c_str());
                }
            }
            return ErrorCode::Success;
        }

        template <typename T>
        ErrorCode Index<T>::LoadIndexDataFromMemory(const std::vector<ByteArray>& p_indexBlobs)
        {
            if (m_index->LoadIndexDataFromMemory(p_indexBlobs) != ErrorCode::Success) return ErrorCode::Fail;

            m_index->SetParameter("NumberOfThreads", std::to_string(m_options.m_iSSDNumberOfThreads));
            m_index->SetParameter("MaxCheck", std::to_string(m_options.m_maxCheck));
            m_index->SetParameter("HashTableExponent", std::to_string(m_options.m_hashExp));
            m_index->UpdateIndex();
            m_index->SetReady(true);

            m_extraSearcher.reset(new ExtraFullGraphSearcher<T>());
            if (!m_extraSearcher->LoadIndex(m_options)) return ErrorCode::Fail;

            m_vectorTranslateMap.reset((std::uint64_t*)(p_indexBlobs.back().Data()), [=](std::uint64_t* ptr) {});

            omp_set_num_threads(m_options.m_iSSDNumberOfThreads);
            m_workSpacePool.reset(new COMMON::WorkSpacePool<ExtraWorkSpace>());
            m_workSpacePool->Init(m_options.m_iSSDNumberOfThreads, m_options.m_maxCheck, m_options.m_hashExp, m_options.m_searchInternalResultNum, min(m_options.m_postingPageLimit, m_options.m_searchPostingPageLimit + 1) << PageSizeEx);
            return ErrorCode::Success;
        }

        template <typename T>
        ErrorCode Index<T>::LoadIndexData(const std::vector<std::shared_ptr<Helper::DiskPriorityIO>>& p_indexStreams)
        {
            if (m_index->LoadIndexData(p_indexStreams) != ErrorCode::Success) return ErrorCode::Fail;

            m_index->SetParameter("NumberOfThreads", std::to_string(m_options.m_iSSDNumberOfThreads));
            m_index->SetParameter("MaxCheck", std::to_string(m_options.m_maxCheck));
            m_index->SetParameter("HashTableExponent", std::to_string(m_options.m_hashExp));
            m_index->UpdateIndex();
            m_index->SetReady(true);

            // TODO: Choose an extra searcher based on config
            m_extraSearcher.reset(new ExtraFullGraphSearcher<T>());
            if (!m_extraSearcher->LoadIndex(m_options)) return ErrorCode::Fail;

            m_vectorTranslateMap.reset(new std::uint64_t[m_index->GetNumSamples()], std::default_delete<std::uint64_t[]>());
            IOBINARY(p_indexStreams.back(), ReadBinary, sizeof(std::uint64_t) * m_index->GetNumSamples(), reinterpret_cast<char*>(m_vectorTranslateMap.get()));

            omp_set_num_threads(m_options.m_iSSDNumberOfThreads);
            m_workSpacePool = std::make_unique<COMMON::WorkSpacePool<ExtraWorkSpace>>();
            m_workSpacePool->Init(m_options.m_iSSDNumberOfThreads, m_options.m_maxCheck, m_options.m_hashExp, m_options.m_searchInternalResultNum, min(m_options.m_postingPageLimit, m_options.m_searchPostingPageLimit + 1) << PageSizeEx);

            m_versionMap.Load(m_options.m_fullDeletedIDFile, m_index->m_iDataBlockSize, m_index->m_iDataCapacity);

            m_postingSizes = std::make_unique<std::atomic_uint32_t[]>(m_options.m_maxHeadNode);

            for (int idx = 0; idx < m_extraSearcher->GetIndexSize(); idx++) {
                uint32_t tmp;
                IOBINARY(p_indexStreams.back(), ReadBinary, sizeof(uint32_t), reinterpret_cast<char*>(&tmp));
                m_postingSizes[idx].store(tmp);
            }

            return ErrorCode::Success;
        }

        template <typename T>
        ErrorCode Index<T>::SaveConfig(std::shared_ptr<Helper::DiskPriorityIO> p_configOut)
        {
            IOSTRING(p_configOut, WriteString, "[Base]\n");
#define DefineBasicParameter(VarName, VarType, DefaultValue, RepresentStr) \
                IOSTRING(p_configOut, WriteString, (RepresentStr + std::string("=") + SPTAG::Helper::Convert::ConvertToString(m_options.VarName) + std::string("\n")).c_str()); \

#include "inc/Core/SPANN/ParameterDefinitionList.h"
#undef DefineBasicParameter

            IOSTRING(p_configOut, WriteString, "[SelectHead]\n");
#define DefineSelectHeadParameter(VarName, VarType, DefaultValue, RepresentStr) \
                IOSTRING(p_configOut, WriteString, (RepresentStr + std::string("=") + SPTAG::Helper::Convert::ConvertToString(m_options.VarName) + std::string("\n")).c_str()); \

#include "inc/Core/SPANN/ParameterDefinitionList.h"
#undef DefineSelectHeadParameter

            IOSTRING(p_configOut, WriteString, "[BuildHead]\n");
#define DefineBuildHeadParameter(VarName, VarType, DefaultValue, RepresentStr) \
                IOSTRING(p_configOut, WriteString, (RepresentStr + std::string("=") + SPTAG::Helper::Convert::ConvertToString(m_options.VarName) + std::string("\n")).c_str()); \

#include "inc/Core/SPANN/ParameterDefinitionList.h"
#undef DefineBuildHeadParameter

            m_index->SaveConfig(p_configOut);

            Helper::Convert::ConvertStringTo<int>(m_index->GetParameter("HashTableExponent").c_str(), m_options.m_hashExp);
            IOSTRING(p_configOut, WriteString, "[BuildSSDIndex]\n");
#define DefineSSDParameter(VarName, VarType, DefaultValue, RepresentStr) \
                IOSTRING(p_configOut, WriteString, (RepresentStr + std::string("=") + SPTAG::Helper::Convert::ConvertToString(m_options.VarName) + std::string("\n")).c_str()); \

#include "inc/Core/SPANN/ParameterDefinitionList.h"
#undef DefineSSDParameter

            IOSTRING(p_configOut, WriteString, "\n");
            return ErrorCode::Success;
        }

        template<typename T>
        ErrorCode Index<T>::SaveIndexData(const std::vector<std::shared_ptr<Helper::DiskPriorityIO>>& p_indexStreams)
        {
            if (m_index == nullptr || m_vectorTranslateMap == nullptr) return ErrorCode::EmptyIndex;

            ErrorCode ret;
            if ((ret = m_index->SaveIndexData(p_indexStreams)) != ErrorCode::Success) return ret;

            IOBINARY(p_indexStreams.back(), WriteBinary, sizeof(std::uint64_t) * m_index->GetNumSamples(), (char*)(m_vectorTranslateMap.get()));
            m_versionMap.Save(m_options.m_fullDeletedIDFile);
            return ErrorCode::Success;
        }

#pragma region K-NN search

        template<typename T>
        ErrorCode Index<T>::SearchIndex(QueryResult &p_query, bool p_searchDeleted) const
        {
            if (!m_bReady) return ErrorCode::EmptyIndex;

            m_index->SearchIndex(p_query);

            auto* p_queryResults = (COMMON::QueryResultSet<T>*) & p_query;
            std::shared_ptr<ExtraWorkSpace> workSpace = nullptr;
            if (m_extraSearcher != nullptr) {
                workSpace = m_workSpacePool->Rent();
                workSpace->m_postingIDs.clear();

                float limitDist = p_queryResults->GetResult(0)->Dist * m_options.m_maxDistRatio;
                for (int i = 0; i < m_options.m_searchInternalResultNum; ++i)
                {
                    auto res = p_queryResults->GetResult(i);
                    if (res->VID == -1 || (limitDist > 0.1 && res->Dist > limitDist)) break;
                    workSpace->m_postingIDs.emplace_back(res->VID);
                }

                for (int i = 0; i < p_queryResults->GetResultNum(); ++i)
                {
                    auto res = p_queryResults->GetResult(i);
                    if (res->VID == -1) break;
                    res->VID = static_cast<SizeType>((m_vectorTranslateMap.get())[res->VID]);
                }

                p_queryResults->Reverse();
                m_extraSearcher->SearchIndex(workSpace.get(), *p_queryResults, m_index, nullptr, m_versionMap);
                p_queryResults->SortResult();
                m_workSpacePool->Return(workSpace);
            }

            if (p_query.WithMeta() && nullptr != m_pMetadata)
            {
                for (int i = 0; i < p_query.GetResultNum(); ++i)
                {
                    SizeType result = p_query.GetResult(i)->VID;
                    p_query.SetMetadata(i, (result < 0) ? ByteArray::c_empty : m_pMetadata->GetMetadataCopy(result));
                }
            }
            return ErrorCode::Success;
        }

        template <typename T>
        ErrorCode Index<T>::DebugSearchDiskIndex(QueryResult& p_query, int p_subInternalResultNum, int p_internalResultNum,
                                                 SearchStats* p_stats, std::set<int>* truth, std::map<int, std::set<int>>* found)
        {
            auto exStart = std::chrono::high_resolution_clock::now();

            if (nullptr == m_extraSearcher) return ErrorCode::EmptyIndex;

            COMMON::QueryResultSet<T> newResults(*((COMMON::QueryResultSet<T>*)&p_query));
            for (int i = 0; i < newResults.GetResultNum() && !m_options.m_useKV; ++i)
            {
                auto res = newResults.GetResult(i);
                if (res->VID == -1) break;

                auto global_VID = static_cast<SizeType>((m_vectorTranslateMap.get())[res->VID]);
                if (truth && truth->count(global_VID)) (*found)[res->VID].insert(global_VID);
                res->VID = global_VID;
            }
            newResults.Reset();
            newResults.Reverse();

            auto auto_ws = m_workSpacePool->Rent();

            int partitions = (p_internalResultNum + p_subInternalResultNum - 1) / p_subInternalResultNum;
            float limitDist = p_query.GetResult(0)->Dist * m_options.m_maxDistRatio;
            for (SizeType p = 0; p < partitions; p++) {
                int subInternalResultNum = min(p_subInternalResultNum, p_internalResultNum - p_subInternalResultNum * p);

                auto_ws->m_postingIDs.clear();

                for (int i = p * p_subInternalResultNum; i < p * p_subInternalResultNum + subInternalResultNum; i++)
                {
                    auto res = p_query.GetResult(i);
                    if (res->VID == -1 || (limitDist > 0.1 && res->Dist > limitDist)) break;
                    auto_ws->m_postingIDs.emplace_back(res->VID);
                }

                auto exEnd = std::chrono::high_resolution_clock::now();

                p_stats->m_totalLatency += ((double)std::chrono::duration_cast<std::chrono::milliseconds>(exEnd - exStart).count());


                m_extraSearcher->SearchIndex(auto_ws.get(), newResults, m_index, p_stats, m_versionMap, truth, found);
            }

            m_workSpacePool->Return(auto_ws);

            newResults.SortResult();
            std::copy(newResults.GetResults(), newResults.GetResults() + newResults.GetResultNum(), p_query.GetResults());

            return ErrorCode::Success;
        }
#pragma endregion

        template <typename T>
        void Index<T>::SelectHeadAdjustOptions(int p_vectorCount) {
            if (m_options.m_headVectorCount != 0) m_options.m_ratio = m_options.m_headVectorCount * 1.0 / p_vectorCount;
            int headCnt = static_cast<int>(std::round(m_options.m_ratio * p_vectorCount));
            if (headCnt == 0)
            {
                for (double minCnt = 1; headCnt == 0; minCnt += 0.2)
                {
                    m_options.m_ratio = minCnt / p_vectorCount;
                    headCnt = static_cast<int>(std::round(m_options.m_ratio * p_vectorCount));
                }

                LOG(Helper::LogLevel::LL_Info, "Setting requires to select none vectors as head, adjusted it to %d vectors\n", headCnt);
            }

            if (m_options.m_iBKTKmeansK > headCnt)
            {
                m_options.m_iBKTKmeansK = headCnt;
                LOG(Helper::LogLevel::LL_Info, "Setting of cluster number is less than head count, adjust it to %d\n", headCnt);
            }

            if (m_options.m_selectThreshold == 0)
            {
                m_options.m_selectThreshold = min(p_vectorCount - 1, static_cast<int>(1 / m_options.m_ratio));
                LOG(Helper::LogLevel::LL_Info, "Set SelectThreshold to %d\n", m_options.m_selectThreshold);
            }

            if (m_options.m_splitThreshold == 0)
            {
                m_options.m_splitThreshold = min(p_vectorCount - 1, static_cast<int>(m_options.m_selectThreshold * 2));
                LOG(Helper::LogLevel::LL_Info, "Set SplitThreshold to %d\n", m_options.m_splitThreshold);
            }

            if (m_options.m_splitFactor == 0)
            {
                m_options.m_splitFactor = min(p_vectorCount - 1, static_cast<int>(std::round(1 / m_options.m_ratio) + 0.5));
                LOG(Helper::LogLevel::LL_Info, "Set SplitFactor to %d\n", m_options.m_splitFactor);
            }
        }

        template <typename T>
        int Index<T>::SelectHeadDynamicallyInternal(const std::shared_ptr<COMMON::BKTree> p_tree, int p_nodeID,
                                                    const Options& p_opts, std::vector<int>& p_selected)
        {
            typedef std::pair<int, int> CSPair;
            std::vector<CSPair> children;
            int childrenSize = 1;
            const auto& node = (*p_tree)[p_nodeID];
            if (node.childStart >= 0)
            {
                children.reserve(node.childEnd - node.childStart);
                for (int i = node.childStart; i < node.childEnd; ++i)
                {
                    int cs = SelectHeadDynamicallyInternal(p_tree, i, p_opts, p_selected);
                    if (cs > 0)
                    {
                        children.emplace_back(i, cs);
                        childrenSize += cs;
                    }
                }
            }

            if (childrenSize >= p_opts.m_selectThreshold)
            {
                if (node.centerid < (*p_tree)[0].centerid)
                {
                    p_selected.push_back(node.centerid);
                }

                if (childrenSize > p_opts.m_splitThreshold)
                {
                    std::sort(children.begin(), children.end(), [](const CSPair& a, const CSPair& b)
                    {
                        return a.second > b.second;
                    });

                    size_t selectCnt = static_cast<size_t>(std::ceil(childrenSize * 1.0 / p_opts.m_splitFactor) + 0.5);
                    //if (selectCnt > 1) selectCnt -= 1;
                    for (size_t i = 0; i < selectCnt && i < children.size(); ++i)
                    {
                        p_selected.push_back((*p_tree)[children[i].first].centerid);
                    }
                }

                return 0;
            }

            return childrenSize;
        }

        template <typename T>
        void Index<T>::SelectHeadDynamically(const std::shared_ptr<COMMON::BKTree> p_tree, int p_vectorCount, std::vector<int>& p_selected) {
            p_selected.clear();
            p_selected.reserve(p_vectorCount);

            if (static_cast<int>(std::round(m_options.m_ratio * p_vectorCount)) >= p_vectorCount)
            {
                for (int i = 0; i < p_vectorCount; ++i)
                {
                    p_selected.push_back(i);
                }

                return;
            }
            Options opts = m_options;

            int selectThreshold = m_options.m_selectThreshold;
            int splitThreshold = m_options.m_splitThreshold;

            double minDiff = 100;
            for (int select = 2; select <= m_options.m_selectThreshold; ++select)
            {
                opts.m_selectThreshold = select;
                opts.m_splitThreshold = m_options.m_splitThreshold;

                int l = m_options.m_splitFactor;
                int r = m_options.m_splitThreshold;

                while (l < r - 1)
                {
                    opts.m_splitThreshold = (l + r) / 2;
                    p_selected.clear();

                    SelectHeadDynamicallyInternal(p_tree, 0, opts, p_selected);
                    std::sort(p_selected.begin(), p_selected.end());
                    p_selected.erase(std::unique(p_selected.begin(), p_selected.end()), p_selected.end());

                    double diff = static_cast<double>(p_selected.size()) / p_vectorCount - m_options.m_ratio;

                    LOG(Helper::LogLevel::LL_Info,
                        "Select Threshold: %d, Split Threshold: %d, diff: %.2lf%%.\n",
                        opts.m_selectThreshold,
                        opts.m_splitThreshold,
                        diff * 100.0);

                    if (minDiff > fabs(diff))
                    {
                        minDiff = fabs(diff);

                        selectThreshold = opts.m_selectThreshold;
                        splitThreshold = opts.m_splitThreshold;
                    }

                    if (diff > 0)
                    {
                        l = (l + r) / 2;
                    }
                    else
                    {
                        r = (l + r) / 2;
                    }
                }
            }

            opts.m_selectThreshold = selectThreshold;
            opts.m_splitThreshold = splitThreshold;

            LOG(Helper::LogLevel::LL_Info,
                "Final Select Threshold: %d, Split Threshold: %d.\n",
                opts.m_selectThreshold,
                opts.m_splitThreshold);

            p_selected.clear();
            SelectHeadDynamicallyInternal(p_tree, 0, opts, p_selected);
            std::sort(p_selected.begin(), p_selected.end());
            p_selected.erase(std::unique(p_selected.begin(), p_selected.end()), p_selected.end());
        }

        template <typename T>
        bool Index<T>::SelectHead(std::shared_ptr<Helper::VectorSetReader>& p_reader) {
            std::shared_ptr<VectorSet> vectorset = p_reader->GetVectorSet();
            if (m_options.m_distCalcMethod == DistCalcMethod::Cosine && !p_reader->IsNormalized())
                vectorset->Normalize(m_options.m_iSelectHeadNumberOfThreads);
            COMMON::Dataset<T> data(vectorset->Count(), vectorset->Dimension(), vectorset->Count(), vectorset->Count() + 1, (T*)vectorset->GetData());

            auto t1 = std::chrono::high_resolution_clock::now();
            SelectHeadAdjustOptions(data.R());
            std::vector<int> selected;
            if (data.R() == 1) {
                selected.push_back(0);
            }
            else if (Helper::StrUtils::StrEqualIgnoreCase(m_options.m_selectType.c_str(), "Random")) {
                LOG(Helper::LogLevel::LL_Info, "Start generating Random head.\n");
                selected.resize(data.R());
                for (int i = 0; i < data.R(); i++) selected[i] = i;
                std::random_shuffle(selected.begin(), selected.end());
                int headCnt = static_cast<int>(std::round(m_options.m_ratio * data.R()));
                selected.resize(headCnt);
            }
            else if (Helper::StrUtils::StrEqualIgnoreCase(m_options.m_selectType.c_str(), "BKT")) {
                LOG(Helper::LogLevel::LL_Info, "Start generating BKT.\n");
                std::shared_ptr<COMMON::BKTree> bkt = std::make_shared<COMMON::BKTree>();
                bkt->m_iBKTKmeansK = m_options.m_iBKTKmeansK;
                bkt->m_iBKTLeafSize = m_options.m_iBKTLeafSize;
                bkt->m_iSamples = m_options.m_iSamples;
                bkt->m_iTreeNumber = m_options.m_iTreeNumber;
                bkt->m_fBalanceFactor = m_options.m_fBalanceFactor;
                LOG(Helper::LogLevel::LL_Info, "Start invoking BuildTrees.\n");
                LOG(Helper::LogLevel::LL_Info, "BKTKmeansK: %d, BKTLeafSize: %d, Samples: %d, BKTLambdaFactor:%f TreeNumber: %d, ThreadNum: %d.\n",
                    bkt->m_iBKTKmeansK, bkt->m_iBKTLeafSize, bkt->m_iSamples, bkt->m_fBalanceFactor, bkt->m_iTreeNumber, m_options.m_iSelectHeadNumberOfThreads);

                bkt->BuildTrees<T>(data, m_options.m_distCalcMethod, m_options.m_iSelectHeadNumberOfThreads, nullptr, nullptr, true);
                auto t2 = std::chrono::high_resolution_clock::now();
                double elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(t2 - t1).count();
                LOG(Helper::LogLevel::LL_Info, "End invoking BuildTrees.\n");
                LOG(Helper::LogLevel::LL_Info, "Invoking BuildTrees used time: %.2lf minutes (about %.2lf hours).\n", elapsedSeconds / 60.0, elapsedSeconds / 3600.0);

                if (m_options.m_saveBKT) {
                    std::stringstream bktFileNameBuilder;
                    bktFileNameBuilder << m_options.m_vectorPath << ".bkt." << m_options.m_iBKTKmeansK << "_"
                                       << m_options.m_iBKTLeafSize << "_" << m_options.m_iTreeNumber << "_" << m_options.m_iSamples << "_"
                                       << static_cast<int>(m_options.m_distCalcMethod) << ".bin";
                    bkt->SaveTrees(bktFileNameBuilder.str());
                }
                LOG(Helper::LogLevel::LL_Info, "Finish generating BKT.\n");

                LOG(Helper::LogLevel::LL_Info, "Start selecting nodes...Select Head Dynamically...\n");
                SelectHeadDynamically(bkt, data.R(), selected);

                if (selected.empty()) {
                    LOG(Helper::LogLevel::LL_Error, "Can't select any vector as head with current settings\n");
                    return false;
                }
            }

            LOG(Helper::LogLevel::LL_Info,
                "Seleted Nodes: %u, about %.2lf%% of total.\n",
                static_cast<unsigned int>(selected.size()),
                selected.size() * 100.0 / data.R());

            if (!m_options.m_noOutput)
            {
                std::sort(selected.begin(), selected.end());

                std::shared_ptr<Helper::DiskPriorityIO> output = SPTAG::f_createIO(), outputIDs = SPTAG::f_createIO();
                if (output == nullptr || outputIDs == nullptr ||
                    !output->Initialize((m_options.m_indexDirectory + FolderSep + m_options.m_headVectorFile).c_str(), std::ios::binary | std::ios::out) ||
                    !outputIDs->Initialize((m_options.m_indexDirectory + FolderSep + m_options.m_headIDFile).c_str(), std::ios::binary | std::ios::out)) {
                    LOG(Helper::LogLevel::LL_Error, "Failed to create output file:%s %s\n",
                        (m_options.m_indexDirectory + FolderSep + m_options.m_headVectorFile).c_str(),
                        (m_options.m_indexDirectory + FolderSep + m_options.m_headIDFile).c_str());
                    return false;
                }

                SizeType val = static_cast<SizeType>(selected.size());
                if (output->WriteBinary(sizeof(val), reinterpret_cast<char*>(&val)) != sizeof(val)) {
                    LOG(Helper::LogLevel::LL_Error, "Failed to write output file!\n");
                    return false;
                }
                DimensionType dt = data.C();
                if (output->WriteBinary(sizeof(dt), reinterpret_cast<char*>(&dt)) != sizeof(dt)) {
                    LOG(Helper::LogLevel::LL_Error, "Failed to write output file!\n");
                    return false;
                }

                for (int i = 0; i < selected.size(); i++)
                {
                    uint64_t vid = static_cast<uint64_t>(selected[i]);
                    if (outputIDs->WriteBinary(sizeof(vid), reinterpret_cast<char*>(&vid)) != sizeof(vid)) {
                        LOG(Helper::LogLevel::LL_Error, "Failed to write output file!\n");
                        return false;
                    }

                    if (output->WriteBinary(sizeof(T) * data.C(), (char*)(data[vid])) != sizeof(T) * data.C()) {
                        LOG(Helper::LogLevel::LL_Error, "Failed to write output file!\n");
                        return false;
                    }
                }
            }
            auto t3 = std::chrono::high_resolution_clock::now();
            double elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(t3 - t1).count();
            LOG(Helper::LogLevel::LL_Info, "Total used time: %.2lf minutes (about %.2lf hours).\n", elapsedSeconds / 60.0, elapsedSeconds / 3600.0);
            return true;
        }

        template <typename T>
        ErrorCode Index<T>::BuildIndexInternal(std::shared_ptr<Helper::VectorSetReader>& p_reader) {
            if (!m_options.m_indexDirectory.empty()) {
                if (!direxists(m_options.m_indexDirectory.c_str()))
                {
                    mkdir(m_options.m_indexDirectory.c_str());
                }
            }

            auto t1 = std::chrono::high_resolution_clock::now();
            if (m_options.m_selectHead) {
                omp_set_num_threads(m_options.m_iSelectHeadNumberOfThreads);
                if (!SelectHead(p_reader)) {
                    LOG(Helper::LogLevel::LL_Error, "SelectHead Failed!\n");
                    return ErrorCode::Fail;
                }
            }
            auto t2 = std::chrono::high_resolution_clock::now();
            double selectHeadTime = std::chrono::duration_cast<std::chrono::seconds>(t2 - t1).count();
            LOG(Helper::LogLevel::LL_Info, "select head time: %.2lfs\n", selectHeadTime);

            if (m_options.m_buildHead) {
                auto valueType = SPTAG::COMMON::DistanceUtils::Quantizer ? SPTAG::VectorValueType::UInt8 : m_options.m_valueType;
                m_index = SPTAG::VectorIndex::CreateInstance(m_options.m_indexAlgoType, valueType);
                m_index->SetParameter("DistCalcMethod", SPTAG::Helper::Convert::ConvertToString(m_options.m_distCalcMethod));
                for (const auto& iter : m_headParameters)
                {
                    m_index->SetParameter(iter.first.c_str(), iter.second.c_str());
                }

                std::shared_ptr<Helper::ReaderOptions> vectorOptions(new Helper::ReaderOptions(valueType, m_options.m_dim, VectorFileType::DEFAULT));
                auto vectorReader = Helper::VectorSetReader::CreateInstance(vectorOptions);
                if (ErrorCode::Success != vectorReader->LoadFile(m_options.m_indexDirectory + FolderSep + m_options.m_headVectorFile))
                {
                    LOG(Helper::LogLevel::LL_Error, "Failed to read head vector file.\n");
                    return ErrorCode::Fail;
                }
                if (m_index->BuildIndex(vectorReader->GetVectorSet(), nullptr, false, true) != ErrorCode::Success ||
                    m_index->SaveIndex(m_options.m_indexDirectory + FolderSep + m_options.m_headIndexFolder) != ErrorCode::Success) {
                    LOG(Helper::LogLevel::LL_Error, "Failed to build head index.\n");
                    return ErrorCode::Fail;
                }
            }
            auto t3 = std::chrono::high_resolution_clock::now();
            double buildHeadTime = std::chrono::duration_cast<std::chrono::seconds>(t3 - t2).count();
            LOG(Helper::LogLevel::LL_Info, "select head time: %.2lfs build head time: %.2lfs\n", selectHeadTime, buildHeadTime);

            if (m_options.m_enableSSD) {
                omp_set_num_threads(m_options.m_iSSDNumberOfThreads);

                if (m_index == nullptr && LoadIndex(m_options.m_indexDirectory + FolderSep + m_options.m_headIndexFolder, m_index) != ErrorCode::Success) {
                    LOG(Helper::LogLevel::LL_Error, "Cannot load head index from %s!\n", (m_options.m_indexDirectory + FolderSep + m_options.m_headIndexFolder).c_str());
                    return ErrorCode::Fail;
                }
                if (!CheckHeadIndexType()) return ErrorCode::Fail;

                m_index->SetParameter("NumberOfThreads", std::to_string(m_options.m_iSSDNumberOfThreads));
                m_index->SetParameter("MaxCheck", std::to_string(m_options.m_maxCheck));
                m_index->SetParameter("HashTableExponent", std::to_string(m_options.m_hashExp));
                m_index->UpdateIndex();

                if (m_options.m_useKV)
                {
                    if (m_options.m_inPlace) {
                        m_extraSearcher.reset(new ExtraRocksDBController<T>(m_options.m_KVPath.c_str(), m_options.m_dim, INT_MAX, m_options.m_useDirectIO, m_options.m_latencyLimit));
                    }
                    else {
                        m_extraSearcher.reset(new ExtraRocksDBController<T>(m_options.m_KVPath.c_str(), m_options.m_dim, m_options.m_postingPageLimit * PageSize / (sizeof(T)*m_options.m_dim + sizeof(int) + sizeof(uint8_t) ), m_options.m_useDirectIO, m_options.m_latencyLimit));
                    }
                } else {
                    m_extraSearcher.reset(new ExtraFullGraphSearcher<T>());
                }
                if (m_options.m_buildSsdIndex) {
                    if (!m_extraSearcher->BuildIndex(p_reader, m_index, m_options)) {
                        LOG(Helper::LogLevel::LL_Error, "BuildSSDIndex Failed!\n");
                        return ErrorCode::Fail;
                    }
                }
                if (!m_extraSearcher->LoadIndex(m_options)) {
                    LOG(Helper::LogLevel::LL_Error, "Cannot Load SSDIndex!\n");
                    return ErrorCode::Fail;
                }

                if (!m_options.m_useKV) {
                    m_vectorTranslateMap.reset(new std::uint64_t[m_index->GetNumSamples()], std::default_delete<std::uint64_t[]>());
                    std::shared_ptr<Helper::DiskPriorityIO> ptr = SPTAG::f_createIO();
                    if (ptr == nullptr || !ptr->Initialize((m_options.m_indexDirectory + FolderSep + m_options.m_headIDFile).c_str(), std::ios::binary | std::ios::in)) {
                        LOG(Helper::LogLevel::LL_Error, "Failed to open headIDFile file:%s\n", (m_options.m_indexDirectory + FolderSep + m_options.m_headIDFile).c_str());
                        return ErrorCode::Fail;
                    }
                    IOBINARY(ptr, ReadBinary, sizeof(std::uint64_t) * m_index->GetNumSamples(), (char*)(m_vectorTranslateMap.get()));
                } else {
                    //data structrue initialization
                    m_versionMap.Load(m_options.m_fullDeletedIDFile, m_index->m_iDataBlockSize, m_index->m_iDataCapacity);
                    m_postingSizes = std::make_unique<std::atomic_uint32_t[]>(m_options.m_maxHeadNode);
                    std::ifstream input(m_options.m_ssdInfoFile, std::ios::binary);
                    if (!input.is_open())
                    {
                        fprintf(stderr, "Failed to open file: %s\n", m_options.m_ssdInfoFile.c_str());
                        exit(1);
                    }

					int vectorNum;
                    input.read(reinterpret_cast<char*>(&vectorNum), sizeof(vectorNum));
					m_vectorNum.store(vectorNum);

                    m_totalReplicaCount.resize(vectorNum);
                    for (int i = 0; i < vectorNum; i++) {
                        m_totalReplicaCount[i] = 0;
                    }

					LOG(Helper::LogLevel::LL_Info, "Current vector num: %d.\n", m_vectorNum.load());

					uint32_t postingNum;
					input.read(reinterpret_cast<char*>(&postingNum), sizeof(postingNum));

					LOG(Helper::LogLevel::LL_Info, "Current posting num: %d.\n", postingNum);

					for (int idx = 0; idx < postingNum; idx++) {
						uint32_t tmp;
						input.read(reinterpret_cast<char*>(&tmp), sizeof(uint32_t));
						m_postingSizes[idx].store(tmp);
					}

					input.close();
                    //ForceCompaction();
                }
            }
            
            LOG(Helper::LogLevel::LL_Info, "SPFresh: initialize persistent buffer\n");
            std::shared_ptr<Helper::KeyValueIO> db;
            db.reset(new SPANN::RocksDBIO());
            m_persistentBuffer = std::make_shared<PersistentBuffer>(m_options.m_persistentBufferPath, db);
            LOG(Helper::LogLevel::LL_Info, "SPFresh: finish initialization\n");
            LOG(Helper::LogLevel::LL_Info, "SPFresh: initialize thread pools, append: %d, reassign %d\n", m_options.m_appendThreadNum, m_options.m_reassignThreadNum);
            m_appendThreadPool = std::make_shared<ThreadPool>();
            m_appendThreadPool->init(m_options.m_appendThreadNum);
            m_reassignThreadPool = std::make_shared<ThreadPool>();
            m_reassignThreadPool->init(m_options.m_reassignThreadNum);
            LOG(Helper::LogLevel::LL_Info, "SPFresh: finish initialization\n");

            LOG(Helper::LogLevel::LL_Info, "SPFresh: initialize dispatcher\n");
            m_dispatcher = std::make_shared<Dispatcher>(m_persistentBuffer, m_options.m_batch, m_appendThreadPool, m_reassignThreadPool, this);

            m_dispatcher->run();
            LOG(Helper::LogLevel::LL_Info, "SPFresh: finish initialization\n");

            SimplyCountSplit.resize(20);
            for (int i = 0; i < 20; i++) {
                SimplyCountSplit[i] = 0;
            }
            
            auto t4 = std::chrono::high_resolution_clock::now();
            double buildSSDTime = std::chrono::duration_cast<std::chrono::seconds>(t4 - t3).count();
            LOG(Helper::LogLevel::LL_Info, "select head time: %.2lfs build head time: %.2lfs build ssd time: %.2lfs\n", selectHeadTime, buildHeadTime, buildSSDTime);

            if (m_options.m_deleteHeadVectors) {
                if (fileexists((m_options.m_indexDirectory + FolderSep + m_options.m_headVectorFile).c_str()) &&
                    remove((m_options.m_indexDirectory + FolderSep + m_options.m_headVectorFile).c_str()) != 0) {
                    LOG(Helper::LogLevel::LL_Warning, "Head vector file can't be removed.\n");
                }
            }

            m_workSpacePool.reset(new COMMON::WorkSpacePool<ExtraWorkSpace>());
            m_workSpacePool->Init(m_options.m_iSSDNumberOfThreads, m_options.m_maxCheck, m_options.m_hashExp, m_options.m_searchInternalResultNum, min(m_options.m_postingPageLimit, m_options.m_searchPostingPageLimit + 1) << PageSizeEx);
            m_bReady = true;
            return ErrorCode::Success;
        }

        template <typename T>
        ErrorCode Index<T>::BuildIndex(bool p_normalized)
        {
            SPTAG::VectorValueType valueType = SPTAG::COMMON::DistanceUtils::Quantizer ? SPTAG::VectorValueType::UInt8 : m_options.m_valueType;
            std::shared_ptr<Helper::ReaderOptions> vectorOptions(new Helper::ReaderOptions(valueType, m_options.m_dim, m_options.m_vectorType, m_options.m_vectorDelimiter, p_normalized));
            auto vectorReader = Helper::VectorSetReader::CreateInstance(vectorOptions);
            if (m_options.m_vectorPath.empty())
            {
                LOG(Helper::LogLevel::LL_Info, "Vector file is empty. Skipping loading.\n");
            }
            else {
                if (ErrorCode::Success != vectorReader->LoadFile(m_options.m_vectorPath))
                {
                    LOG(Helper::LogLevel::LL_Error, "Failed to read vector file.\n");
                    return ErrorCode::Fail;
                }
                m_options.m_vectorSize = vectorReader->GetVectorSet()->Count();
            }

            return BuildIndexInternal(vectorReader);
        }

        template <typename T>
        ErrorCode Index<T>::BuildIndex(const void* p_data, SizeType p_vectorNum, DimensionType p_dimension, bool p_normalized)
        {
            if (p_data == nullptr || p_vectorNum == 0 || p_dimension == 0) return ErrorCode::EmptyData;

            if (m_options.m_distCalcMethod == DistCalcMethod::Cosine && !p_normalized) {
                COMMON::Utils::BatchNormalize((T*)p_data, p_vectorNum, p_dimension, COMMON::Utils::GetBase<T>(), m_options.m_iSSDNumberOfThreads);
            }
            std::shared_ptr<VectorSet> vectorSet(new BasicVectorSet(ByteArray((std::uint8_t*)p_data, p_vectorNum * p_dimension * sizeof(T), false),
                                                                    GetEnumValueType<T>(), p_dimension, p_vectorNum));
            SPTAG::VectorValueType valueType = SPTAG::COMMON::DistanceUtils::Quantizer ? SPTAG::VectorValueType::UInt8 : m_options.m_valueType;
            std::shared_ptr<Helper::VectorSetReader> vectorReader(new Helper::MemoryVectorReader(std::make_shared<Helper::ReaderOptions>(valueType, p_dimension, VectorFileType::DEFAULT, m_options.m_vectorDelimiter, m_options.m_iSSDNumberOfThreads, true),
                                                                                                 vectorSet));

            m_options.m_vectorSize = p_vectorNum;
            return BuildIndexInternal(vectorReader);
        }

        template <typename T>
        ErrorCode Index<T>::UpdateIndex()
        {
            omp_set_num_threads(m_options.m_iSSDNumberOfThreads);
            m_index->UpdateIndex();
            m_workSpacePool.reset(new COMMON::WorkSpacePool<ExtraWorkSpace>());
            m_workSpacePool->Init(m_options.m_iSSDNumberOfThreads, m_options.m_maxCheck, m_options.m_hashExp, m_options.m_searchInternalResultNum, min(m_options.m_postingPageLimit, m_options.m_searchPostingPageLimit + 1) << PageSizeEx);
            return ErrorCode::Success;
        }

        template <typename T>
        ErrorCode Index<T>::SetParameter(const char* p_param, const char* p_value, const char* p_section)
        {
            if (SPTAG::Helper::StrUtils::StrEqualIgnoreCase(p_section, "BuildHead") && !SPTAG::Helper::StrUtils::StrEqualIgnoreCase(p_param, "isExecute")) {
                if (m_index != nullptr) return m_index->SetParameter(p_param, p_value);
                else m_headParameters[p_param] = p_value;
            }
            else {
                m_options.SetParameter(p_section, p_param, p_value);
            }
            if (SPTAG::Helper::StrUtils::StrEqualIgnoreCase(p_param, "DistCalcMethod")) {
                m_fComputeDistance = COMMON::DistanceCalcSelector<T>(m_options.m_distCalcMethod);
                m_iBaseSquare = (m_options.m_distCalcMethod == DistCalcMethod::Cosine) ? COMMON::Utils::GetBase<T>() * COMMON::Utils::GetBase<T>() : 1;
            }
            return ErrorCode::Success;
        }

        template <typename T>
        std::string Index<T>::GetParameter(const char* p_param, const char* p_section) const
        {
            if (SPTAG::Helper::StrUtils::StrEqualIgnoreCase(p_section, "BuildHead") && !SPTAG::Helper::StrUtils::StrEqualIgnoreCase(p_param, "isExecute")) {
                if (m_index != nullptr) return m_index->GetParameter(p_param);
                else {
                    auto iter = m_headParameters.find(p_param);
                    if (iter != m_headParameters.end()) return iter->second;
                    return "Undefined!";
                }
            }
            else {
                return m_options.GetParameter(p_section, p_param);
            }
        }

        // Add insert entry to persistent buffer
        template <typename T>
        ErrorCode Index<T>::AddIndex(const void *p_data, SizeType p_vectorNum, DimensionType p_dimension,
                                     std::shared_ptr<MetadataSet> p_metadataSet, bool p_withMetaIndex,
                                     bool p_normalized)
        {
            if (m_options.m_indexAlgoType != IndexAlgoType::BKT || m_extraSearcher == nullptr) {
                LOG(Helper::LogLevel::LL_Error, "Only Support BKT Update");
                return ErrorCode::Fail;
            }

            std::vector<QueryResult> p_queryResults(p_vectorNum, QueryResult(nullptr, m_options.m_internalResultNum, false));

            for (int k = 0; k < p_vectorNum; k++)
            {
                p_queryResults[k].SetTarget(reinterpret_cast<const T*>(reinterpret_cast<const char*>(p_data) + k * p_dimension));
                p_queryResults[k].Reset();
                auto VID = m_vectorNum++;
                {
                    std::lock_guard<std::mutex> lock(m_dataAddLock);
                    auto ret = m_versionMap.AddBatch(1);
                    if (ret == ErrorCode::MemoryOverFlow) {
                        LOG(Helper::LogLevel::LL_Info, "VID: %d, Map Size:%d\n", VID, m_versionMap.BufferSize());
                        exit(1);
                    }
                    //m_reassignedID.AddBatch(1);
                }
                m_totalReplicaCount.push_back(0);

                m_index->SearchIndex(p_queryResults[k]);

                int replicaCount = 0;
                BasicResult* queryResults = p_queryResults[k].GetResults();
                std::vector<EdgeInsert> selections(static_cast<size_t>(m_options.m_replicaCount));
                for (int i = 0; i < p_queryResults[k].GetResultNum() && replicaCount < m_options.m_replicaCount; ++i)
                {
                    if (queryResults[i].VID == -1) {
                        break;
                    }
                    // RNG Check.
                    bool rngAccpeted = true;
                    for (int j = 0; j < replicaCount; ++j)
                    {
                        float nnDist = m_index->ComputeDistance(m_index->GetSample(queryResults[i].VID),
                                                                m_index->GetSample(selections[j].headID));
                        if (nnDist <= queryResults[i].Dist)
                        {
                            rngAccpeted = false;
                            break;
                        }
                    }
                    if (!rngAccpeted)
                        continue;
                    selections[replicaCount].headID = queryResults[i].VID;
                    selections[replicaCount].fullID = VID;
                    selections[replicaCount].distance = queryResults[i].Dist;
                    selections[replicaCount].order = (char)replicaCount;
                    ++replicaCount;
                }

                char insertCode = 0;
                uint8_t version = 0;
                m_versionMap.UpdateVersion(VID, version);

                std::string assignment;
                assignment += Helper::Convert::Serialize<char>(&insertCode, 1);
                assignment += Helper::Convert::Serialize<char>(&replicaCount, 1);
                for (int i = 0; i < replicaCount; i++)
                {
                    // LOG(Helper::LogLevel::LL_Info, "VID: %d, HeadID: %d, Write To PersistentBuffer\n", VID, selections[i].headID);
                    assignment += Helper::Convert::Serialize<int>(&selections[i].headID, 1);
                    assignment += Helper::Convert::Serialize<int>(&VID, 1);
                    assignment += Helper::Convert::Serialize<uint8_t>(&version, 1);
                    // assignment += Helper::Convert::Serialize<float>(&selections[i].distance, 1);
                    assignment += Helper::Convert::Serialize<T>(p_queryResults[k].GetTarget(), m_options.m_dim);
                }
                m_assignmentQueue.push(m_persistentBuffer->PutAssignment(assignment));
            }
            return ErrorCode::Success;
        }

        template <typename T>
        ErrorCode Index<T>::DeleteIndex(const SizeType &p_id)
        {
            if (m_options.m_addDeleteTaskToPM) {
                char deleteCode = 1;
                int VID = p_id;
                std::string assignment;
                assignment += Helper::Convert::Serialize<char>(&deleteCode, 1);
                assignment += Helper::Convert::Serialize<int>(&VID, 1);
                m_persistentBuffer->PutAssignment(assignment);
            } else {
                m_versionMap.Delete(p_id);
            }
            return ErrorCode::Success;
        }

        template <typename T>
        void SPTAG::SPANN::Index<T>::Dispatcher::dispatch()
        {
            // int32_t vectorInfoSize = m_index->GetValueSize() + sizeof(int) + sizeof(uint8_t) + sizeof(float);
            int32_t vectorInfoSize = m_index->GetValueSize() + sizeof(int) + sizeof(uint8_t);
            while (running) {

                std::map<SizeType, std::shared_ptr<std::string>> newPart;
                newPart.clear();
                int i;
                for (i = 0; i < batch; i++) {
                    std::string assignment;
                    int assignId = m_index->GetNextAssignment();

                    if (assignId == -1) break;

                    m_persistentBuffer->GetAssignment(assignId, &assignment);
                    if(assignment.empty()) {
                        LOG(Helper::LogLevel::LL_Info, "Error: Get Assignment\n");
                        exit(0);
                    }
                    char code = *(reinterpret_cast<char*>(assignment.data()));
                    if (code == 0) {
                        // insert
                        char* replicaCount = assignment.data() + sizeof(char);
                        // LOG(Helper::LogLevel::LL_Info, "dispatch: replica count: %d\n", *replicaCount);

                        for (char index = 0; index < *replicaCount; index++) {
                            char* headPointer = assignment.data() + sizeof(char) + sizeof(char) + index * (vectorInfoSize + sizeof(int));
                            int32_t headID = *(reinterpret_cast<int*>(headPointer));
                            // LOG(Helper::LogLevel::LL_Info, "dispatch: headID: %d\n", headID);
                            int32_t vid = *(reinterpret_cast<int*>(headPointer + sizeof(int)));
                            // LOG(Helper::LogLevel::LL_Info, "dispatch: vid: %d\n", vid);
                            uint8_t version = *(reinterpret_cast<uint8_t*>(headPointer + sizeof(int) + sizeof(int)));
                            // LOG(Helper::LogLevel::LL_Info, "dispatch: version: %d\n", version);

                            if (m_index->CheckIdDeleted(vid) || !m_index->CheckVersionValid(vid, version)) {
                                // LOG(Helper::LogLevel::LL_Info, "Unvalid Vector: %d, version: %d, current version: %d\n", vid, version);
                                continue;
                            }
                            // LOG(Helper::LogLevel::LL_Info, "Vector: %d, Plan to append to: %d\n", vid, headID);
                            if (newPart.find(headID) == newPart.end()) {
                                newPart[headID] = std::make_shared<std::string>(assignment.substr(sizeof(char) + sizeof(char) + index * (vectorInfoSize + sizeof(int)) + sizeof(int), vectorInfoSize));
                            } else {
                                newPart[headID]->append(assignment.substr(sizeof(char) + sizeof(char) + index * (vectorInfoSize + sizeof(int)) + sizeof(int), vectorInfoSize));
                            }
                        }
                    } else {
                        // delete
                        char* vectorPointer = assignment.data() + sizeof(char);
                        int VID = *(reinterpret_cast<int*>(vectorPointer));
                        //LOG(Helper::LogLevel::LL_Info, "Scanner: delete: %d\n", VID);
                        m_index->DeleteIndex(VID);
                    }
                }

                for (auto & iter : newPart) {
                    int appendNum = (*iter.second).size() / (vectorInfoSize);
                    if (appendNum == 0) LOG(Helper::LogLevel::LL_Info, "Error!, headID :%d, appendNum :%d, size :%d\n", iter.first, appendNum, iter.second);
                    m_index->AppendAsync(iter.first, appendNum, iter.second);
                }

                if (i == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                } else {
                    //LOG(Helper::LogLevel::LL_Info, "Process Append Assignments: %d, Delete Assignments: %d\n", newPart.size(), deletedVector.size());
                }
            }
        }

        template <typename ValueType>
        ErrorCode SPTAG::SPANN::Index<ValueType>::Split(const SizeType headID, int appendNum, std::string& appendPosting)
        {
            // TimeUtils::StopW sw;
            std::unique_lock<std::shared_timed_mutex> lock(m_rwLocks[headID]);
            if (m_postingSizes[headID].load() + appendNum < m_extraSearcher->GetPostingSizeLimit()) {
                return ErrorCode::FailSplit;
            }
            m_splitTaskNum++;
            std::string postingList;
            m_extraSearcher->SearchIndex(headID, postingList);
            postingList += appendPosting;
            // reinterpret postingList to vectors and IDs
            auto* postingP = reinterpret_cast<uint8_t*>(&postingList.front());
            size_t vectorInfoSize = m_options.m_dim * sizeof(ValueType) + m_metaDataSize;
            size_t postVectorNum = postingList.size() / vectorInfoSize;
            COMMON::Dataset<ValueType> smallSample;  // smallSample[i] -> VID
            std::shared_ptr<uint8_t> vectorBuffer(new uint8_t[m_options.m_dim * sizeof(ValueType) * postVectorNum], std::default_delete<uint8_t[]>());
            std::vector<int> localIndicesInsert(postVectorNum);  // smallSample[i] = j <-> localindices[j] = i
            std::vector<uint8_t> localIndicesInsertVersion(postVectorNum);
            // std::vector<float> localIndicesInsertFloat(postVectorNum);
            std::vector<int> localIndices(postVectorNum);
            auto vectorBuf = vectorBuffer.get();
            size_t realVectorNum = postVectorNum;
            int index = 0;
            // LOG(Helper::LogLevel::LL_Info, "Scanning\n");
            // for (int i = 0; i < appendNum; i++)
            // {
            //     uint32_t idx = i * vectorInfoSize;
            //     // m_totalReplicaCount[*(int*)(&appendPosting[idx])]++;
            //     LOG(Helper::LogLevel::LL_Info, "VID: %d, append to %d, version: %d\n", *(int*)(&appendPosting[idx]), headID, *(uint8_t*)(&appendPosting[idx + sizeof(int)]));
            // }
            for (int j = 0; j < postVectorNum; j++)
            {
                uint8_t* vectorId = postingP + j * vectorInfoSize;
                //LOG(Helper::LogLevel::LL_Info, "vector index/total:id: %d/%d:%d\n", j, m_postingSizes[headID].load(), *(reinterpret_cast<int*>(vectorId)));
                uint8_t version = *(reinterpret_cast<uint8_t*>(vectorId + sizeof(int)));
                if (CheckIdDeleted(*(reinterpret_cast<int*>(vectorId))) || !CheckVersionValid(*(reinterpret_cast<int*>(vectorId)), version)) {
                    // m_totalReplicaCount[*(reinterpret_cast<int*>(vectorId))]--;
                    // tbb::concurrent_hash_map<SizeType, SizeType>::const_accessor VIDAccessor;
                    // if (m_totalReplicaCount[*(reinterpret_cast<int*>(vectorId))] == 0 && !m_reassignMap.find(VIDAccessor, *(reinterpret_cast<int*>(vectorId)))) {
                    //     LOG(Helper::LogLevel::LL_Info, "Vector Error: %d, current version: %d, real version: %d\n", *(reinterpret_cast<int*>(vectorId)), version, m_versionMap.GetVersion(*(reinterpret_cast<int*>(vectorId))));
                    //     exit(0);
                    // }
                    // if (m_reassignMap.find(VIDAccessor, *(reinterpret_cast<int*>(vectorId)))) LOG(Helper::LogLevel::LL_Info, "VID: %d is still re-assigning\n", *(reinterpret_cast<int*>(vectorId)));
                    realVectorNum--;
                } else {
                    localIndicesInsert[index] = *(reinterpret_cast<int*>(vectorId));
                    localIndicesInsertVersion[index] = *(reinterpret_cast<uint8_t*>(vectorId + sizeof(int)));
                    // localIndicesInsertFloat[index] = *(reinterpret_cast<float*>(vectorId + sizeof(int) + sizeof(uint8_t)));
                    localIndices[index] = index;
                    index++;
                    memcpy(vectorBuf, vectorId + m_metaDataSize, m_options.m_dim * sizeof(ValueType));
                    vectorBuf += m_options.m_dim * sizeof(ValueType);
                }
            }
            // double gcEndTime = sw.getElapsedMs();
            // m_splitGcCost += gcEndTime;
            if (realVectorNum < m_extraSearcher->GetPostingSizeLimit())
            {
                postingList.clear();
                for (int j = 0; j < realVectorNum; j++)
                {
                    postingList += Helper::Convert::Serialize<int>(&localIndicesInsert[j], 1);
                    postingList += Helper::Convert::Serialize<uint8_t>(&localIndicesInsertVersion[j], 1);
                    // postingList += Helper::Convert::Serialize<float>(&localIndicesInsertFloat[j], 1);
                    postingList += Helper::Convert::Serialize<ValueType>(vectorBuffer.get() + j * m_options.m_dim * sizeof(ValueType), m_options.m_dim);
                }
                m_postingSizes[headID].store(realVectorNum);
                m_extraSearcher->OverrideIndex(headID, postingList);
                m_garbageNum++;
                // m_splitWriteBackCost += sw.getElapsedMs() - gcEndTime;
                return ErrorCode::Success;
            }
            //LOG(Helper::LogLevel::LL_Info, "Resize\n");
            localIndicesInsert.resize(realVectorNum);
            localIndices.resize(realVectorNum);
            smallSample.Initialize(realVectorNum, m_options.m_dim, m_index->m_iDataBlockSize, m_index->m_iDataCapacity, reinterpret_cast<ValueType*>(vectorBuffer.get()), false);

            // k = 2, maybe we can change the split number, now it is fixed
            SPTAG::COMMON::KmeansArgs<ValueType> args(2, smallSample.C(), (SizeType)localIndicesInsert.size(), 1, m_index->GetDistCalcMethod());
            std::shuffle(localIndices.begin(), localIndices.end(), std::mt19937(std::random_device()()));
            int numClusters = SPTAG::COMMON::KmeansClustering(smallSample, localIndices, 0, (SizeType)localIndices.size(), args, 1000, 100.0F, false, nullptr, false);
            if (numClusters <= 1)
            {
                LOG(Helper::LogLevel::LL_Info, "Cluserting Failed\n");
                postingList.clear();
                for (int j = 0; j < realVectorNum; j++)
                {
                    postingList += Helper::Convert::Serialize<int>(&localIndicesInsert[j], 1);
                    postingList += Helper::Convert::Serialize<uint8_t>(&localIndicesInsertVersion[j], 1);
                    // postingList += Helper::Convert::Serialize<float>(&localIndicesInsertFloat[j], 1);
                    postingList += Helper::Convert::Serialize<ValueType>(vectorBuffer.get() + j * m_options.m_dim * sizeof(ValueType), m_options.m_dim);
                }
                m_postingSizes[headID].store(realVectorNum);
                m_extraSearcher->AddIndex(headID, postingList);
                return ErrorCode::Success;
            }

            long long newHeadVID = -1;
            int first = 0;
            std::vector<SizeType> newHeadsID;
            std::vector<std::string> newPostingLists;
            bool theSameHead = false;
            for (int k = 0; k < 2; k++) {
                std::string postingList;
                if (args.counts[k] == 0)	continue;
                SimplyCountSplit[args.counts[k] / 10]++;
                if (!theSameHead && m_index->ComputeDistance(args.centers + k * args._D, m_index->GetSample(headID)) < Epsilon) {
                    newHeadsID.push_back(headID);
                    newHeadVID = headID;
                    theSameHead = true;
                    for (int j = 0; j < args.counts[k]; j++)
                    {

                        postingList += Helper::Convert::Serialize<SizeType>(&localIndicesInsert[localIndices[first + j]], 1);
                        postingList += Helper::Convert::Serialize<uint8_t>(&localIndicesInsertVersion[localIndices[first + j]], 1);
                        // postingList += Helper::Convert::Serialize<float>(&localIndicesInsertFloat[localIndices[first + j]], 1);
                        postingList += Helper::Convert::Serialize<ValueType>(smallSample[localIndices[first + j]], m_options.m_dim);
                    }
                    m_extraSearcher->OverrideIndex(newHeadVID, postingList);
                    m_theSameHeadNum++;
                }
                else {
                    int begin, end = 0;
                    m_index->AddIndexId(args.centers + k * args._D, 1, m_options.m_dim, begin, end);
                    newHeadVID = begin;
                    if (begin == m_options.m_maxHeadNode) exit(0);
                    newHeadsID.push_back(begin);
                    for (int j = 0; j < args.counts[k]; j++)
                    {
                        // float dist = m_index->ComputeDistance(smallSample[args.clusterIdx[k]], smallSample[localIndices[first + j]]);
                        postingList += Helper::Convert::Serialize<SizeType>(&localIndicesInsert[localIndices[first + j]], 1);
                        postingList += Helper::Convert::Serialize<uint8_t>(&localIndicesInsertVersion[localIndices[first + j]], 1);
                        // postingList += Helper::Convert::Serialize<float>(&dist, 1);
                        postingList += Helper::Convert::Serialize<ValueType>(smallSample[localIndices[first + j]], m_options.m_dim);
                    }
                    m_extraSearcher->AddIndex(newHeadVID, postingList);
                    m_index->AddIndexIdx(begin, end);
                }
                newPostingLists.push_back(postingList);
                // LOG(Helper::LogLevel::LL_Info, "Head id: %d split into : %d, length: %d\n", headID, newHeadVID, args.counts[k]);
                first += args.counts[k];
                m_postingSizes[newHeadVID] = args.counts[k];
            }
            if (!theSameHead) {
                m_index->DeleteIndex(headID);
                // m_extraSearcher->DeleteIndex(headID);
                m_postingSizes[headID] = 0;
            }
            lock.unlock();
            int split_order = ++m_splitNum;
            // if (theSameHead) LOG(Helper::LogLevel::LL_Info, "The Same Head\n");
            // LOG(Helper::LogLevel::LL_Info, "head1:%d, head2:%d\n", newHeadsID[0], newHeadsID[1]);

            // QuantifySplit(headID, newPostingLists, newHeadsID, headID, split_order);
            // QuantifyAssumptionBrokenTotally();
            
            if (!m_options.m_disableReassign) ReAssign(headID, newPostingLists, newHeadsID);

            
            // while (!ReassignFinished())
            // {
            //     std::this_thread::sleep_for(std::chrono::milliseconds(10));
            // }
            
            
            // LOG(Helper::LogLevel::LL_Info, "After ReAssign\n");

            // QuantifySplit(headID, newPostingLists, newHeadsID, headID, split_order);
            return ErrorCode::Success;
        }

        template <typename ValueType>
        ErrorCode SPTAG::SPANN::Index<ValueType>::ReAssign(SizeType headID, std::vector<std::string>& postingLists, std::vector<SizeType>& newHeadsID) {
//            TimeUtils::StopW sw;
            auto headVector = reinterpret_cast<const ValueType*>(m_index->GetSample(headID));
            std::vector<SizeType> HeadPrevTopK;
            std::vector<float> HeadPrevToSplitHeadDist;
            if (m_options.m_reassignK > 0) {
                COMMON::QueryResultSet<ValueType> nearbyHeads(NULL, m_options.m_reassignK);
                nearbyHeads.SetTarget(headVector);
                nearbyHeads.Reset();
                m_index->SearchIndex(nearbyHeads);
                BasicResult* queryResults = nearbyHeads.GetResults();
                for (int i = 0; i < nearbyHeads.GetResultNum(); i++) {
                    std::string tempPostingList;
                    auto vid = queryResults[i].VID;
                    if (vid == -1) {
                        break;
                    }
                    if (find(newHeadsID.begin(), newHeadsID.end(), vid) == newHeadsID.end()) {
                        // m_extraSearcher->SearchIndex(vid, tempPostingList);
                        // postingLists.push_back(tempPostingList);
                        HeadPrevTopK.push_back(vid);
                        HeadPrevToSplitHeadDist.push_back(queryResults[i].Dist);
                    }
                }
                std::vector<std::string> tempPostingLists;
                m_extraSearcher->SearchIndexMulti(HeadPrevTopK, &tempPostingLists);
                for (int i = 0; i < HeadPrevTopK.size(); i++) {
                    postingLists.push_back(tempPostingLists[i]);
                }
            }

            int vectorInfoSize = m_options.m_dim * sizeof(ValueType) + m_metaDataSize;
            std::map<SizeType, ValueType*> reAssignVectorsTop0;
            std::map<SizeType, SizeType> reAssignVectorsHeadPrevTop0;
            std::map<SizeType, uint8_t> versionsTop0;
            std::map<SizeType, ValueType*> reAssignVectorsTopK;
            std::map<SizeType, SizeType> reAssignVectorsHeadPrevTopK;
            std::map<SizeType, uint8_t> versionsTopK;

            std::vector<float_t> newHeadDist;

            newHeadDist.push_back(m_index->ComputeDistance(m_index->GetSample(headID), m_index->GetSample(newHeadsID[0])));
            newHeadDist.push_back(m_index->ComputeDistance(m_index->GetSample(headID), m_index->GetSample(newHeadsID[1])));

            for (int i = 0; i < postingLists.size(); i++) {
                auto& postingList = postingLists[i];
                size_t postVectorNum = postingList.size() / vectorInfoSize;
                auto* postingP = reinterpret_cast<uint8_t*>(&postingList.front());
                for (int j = 0; j < postVectorNum; j++) {
                    uint8_t* vectorId = postingP + j * vectorInfoSize;
                    SizeType vid = *(reinterpret_cast<SizeType*>(vectorId));
                    uint8_t version = *(reinterpret_cast<uint8_t*>(vectorId + sizeof(int)));
                    // float dist = *(reinterpret_cast<float*>(vectorId + sizeof(int) + sizeof(uint8_t)));
                    float dist;
                    if (i <= 1) {
                        if (!CheckIdDeleted(vid) && CheckVersionValid(vid, version)) {
                            m_reAssignScanNum++;
                            dist = m_index->ComputeDistance(m_index->GetSample(newHeadsID[i]), reinterpret_cast<ValueType*>(vectorId + m_metaDataSize));
                            if (CheckIsNeedReassign(newHeadsID, reinterpret_cast<ValueType*>(vectorId + m_metaDataSize), headID, newHeadDist[i], dist, true, newHeadsID[i])) {
                                reAssignVectorsTop0[vid] = reinterpret_cast<ValueType*>(vectorId + m_metaDataSize);
                                reAssignVectorsHeadPrevTop0[vid] = newHeadsID[i];
                                versionsTop0[vid] = version;
                            }
                        }
                    } else {
                        if ((reAssignVectorsTop0.find(vid) == reAssignVectorsTop0.end()))
                        {
                            if (reAssignVectorsTopK.find(vid) == reAssignVectorsTopK.end() && !CheckIdDeleted(vid) && CheckVersionValid(vid, version)) {
                                m_reAssignScanNum++;
                                dist = m_index->ComputeDistance(m_index->GetSample(HeadPrevTopK[i-2]), reinterpret_cast<ValueType*>(vectorId + m_metaDataSize));
                                if (CheckIsNeedReassign(newHeadsID, reinterpret_cast<ValueType*>(vectorId + m_metaDataSize), headID, HeadPrevToSplitHeadDist[i-2], dist, false, HeadPrevTopK[i-2])) {
                                    reAssignVectorsTopK[vid] = reinterpret_cast<ValueType*>(vectorId + m_metaDataSize);
                                    reAssignVectorsHeadPrevTopK[vid] = HeadPrevTopK[i-2];
                                    versionsTopK[vid] = version;
                                }
                            }
                        }
                    }
                }
            }
            // LOG(Helper::LogLevel::LL_Info, "Scan: %d\n", m_reAssignScanNum.load());
            // exit(0);
            

            ReAssignVectors(reAssignVectorsTop0, reAssignVectorsHeadPrevTop0, versionsTop0);
            ReAssignVectors(reAssignVectorsTopK, reAssignVectorsHeadPrevTopK, versionsTopK);
//            m_reassignTotalCost += sw.getElapsedMs();
            return ErrorCode::Success;
        }

        template <typename ValueType>
        void SPTAG::SPANN::Index<ValueType>::ReAssignVectors(std::map<SizeType, ValueType*>& reAssignVectors,
                             std::map<SizeType, SizeType>& HeadPrevs, std::map<SizeType, uint8_t>& versions)
        {
            for (auto it = reAssignVectors.begin(); it != reAssignVectors.end(); ++it) {
                //PrintFirstFiveDimInt8(reinterpret_cast<uint8_t*>(it->second), it->first);
                auto vectorContain = std::make_shared<std::string>(Helper::Convert::Serialize<uint8_t>(it->second, m_options.m_dim));
                //PrintFirstFiveDimInt8(reinterpret_cast<uint8_t*>(&vectorContain->front()), it->first);
                ReassignAsync(vectorContain, it->first, HeadPrevs[it->first], versions[it->first]);
            }
            /*
            while (!m_dispatcher->reassignFinished()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            */
        }

        template <typename ValueType>
        bool SPTAG::SPANN::Index<ValueType>::ReAssignUpdate
                (const std::shared_ptr<std::string>& vectorContain, SizeType VID, SizeType HeadPrev, uint8_t version)
        {
            m_reAssignNum++;

            bool isNeedReassign = true;
            /*
            uint8_t version;
            if (isNeedReassign) {
                m_versionMap.IncVersion(VID, &version);
            */


            COMMON::QueryResultSet<ValueType> p_queryResults(NULL, m_options.m_internalResultNum);
            p_queryResults.SetTarget(reinterpret_cast<ValueType*>(&vectorContain->front()));
            p_queryResults.Reset();
            m_index->SearchIndex(p_queryResults);

            int replicaCount = 0;
            BasicResult* queryResults = p_queryResults.GetResults();
            std::vector<EdgeInsert> selections(static_cast<size_t>(m_options.m_replicaCount));

            int i;
            for (i = 0; i < p_queryResults.GetResultNum() && replicaCount < m_options.m_replicaCount; ++i) {
                if (queryResults[i].VID == -1) {
                    break;
                }
                // RNG Check.
                bool rngAccpeted = true;
                for (int j = 0; j < replicaCount; ++j) {
                    float nnDist = m_index->ComputeDistance(
                            m_index->GetSample(queryResults[i].VID),
                            m_index->GetSample(selections[j].headID));
                    if (m_options.m_rngFactor * nnDist <= queryResults[i].Dist) {
                        rngAccpeted = false;
                        break;
                    }
                }
                if (!rngAccpeted)
                    continue;

                selections[replicaCount].headID = queryResults[i].VID;

                /*
                if (queryResults[i].VID == HeadPrev) {
                    isNeedReassign = false;
                    break;
                }
                */

                selections[replicaCount].fullID = VID;
                selections[replicaCount].distance = queryResults[i].Dist;
                selections[replicaCount].order = (char)replicaCount;
                ++replicaCount;
            }

            if (CheckVersionValid(VID, version)) {
                // LOG(Helper::LogLevel::LL_Info, "Update Version: VID: %d, version: %d, current version: %d\n", VID, version, m_versionMap.GetVersion(VID));
                m_versionMap.IncVersion(VID, &version);
            } else {
                isNeedReassign = false;
            }

            //LOG(Helper::LogLevel::LL_Info, "Reassign: oldVID:%d, replicaCount:%d, candidateNum:%d, dist0:%f\n", oldVID, replicaCount, i, selections[0].distance);

            for (i = 0; isNeedReassign && i < replicaCount && CheckVersionValid(VID, version); i++) {
                std::string newPart;
                newPart += Helper::Convert::Serialize<int>(&VID, 1);
                newPart += Helper::Convert::Serialize<uint8_t>(&version, 1);
                // newPart += Helper::Convert::Serialize<float>(&selections[i].distance, 1);
                newPart += Helper::Convert::Serialize<ValueType>(p_queryResults.GetTarget(), m_options.m_dim);
                auto headID = selections[i].headID;
                //LOG(Helper::LogLevel::LL_Info, "Reassign: headID :%d, oldVID:%d, newVID:%d, posting length: %d, dist: %f, string size: %d\n", headID, oldVID, VID, m_postingSizes[headID].load(), selections[i].distance, newPart.size());
                if (ErrorCode::Undefined == Append(headID, 1, newPart)) {
                    // LOG(Helper::LogLevel::LL_Info, "Head Miss: VID: %d, current version: %d, another re-assign\n", VID, version);
                    isNeedReassign = false;
                }
                
                // if (m_extraSearcher->AppendPosting(headID, newPart) != ErrorCode::Success) {
                //     LOG(Helper::LogLevel::LL_Error, "Merge failed!\n");
                // }
                // m_postingSizes[headID].fetch_add(1, std::memory_order_relaxed);
            }
            return isNeedReassign;
        }

        template <typename ValueType>
        ErrorCode SPTAG::SPANN::Index<ValueType>::Append(SizeType headID, int appendNum, std::string& appendPosting)
        {
            int reassignExtraLimit = 0;
            if (appendPosting.empty()) {
                LOG(Helper::LogLevel::LL_Error, "Error! empty append posting!\n");
            }
            int vectorInfoSize = m_options.m_dim * sizeof(ValueType) + m_metaDataSize;
//            TimeUtils::StopW sw;
            m_appendTaskNum++;

            if (appendNum == 0) {
                LOG(Helper::LogLevel::LL_Info, "Error!, headID :%d, appendNum:%d\n", headID, appendNum);
            }

            if (appendNum == -1) {
                appendNum = 1;
                reassignExtraLimit = 3;
            }

        checkDeleted:
            if (!m_index->ContainSample(headID)) {
                for (int i = 0; i < appendNum; i++)
                {
//                  m_currerntReassignTaskNum++;
                    uint32_t idx = i * vectorInfoSize;
                    uint8_t version = *(uint8_t*)(&appendPosting[idx + sizeof(int)]);
                    auto vectorContain = std::make_shared<std::string>(appendPosting.substr(idx + m_metaDataSize, m_options.m_dim * sizeof(ValueType)));
                    if (CheckVersionValid(*(int*)(&appendPosting[idx]), version)) {
                        // LOG(Helper::LogLevel::LL_Info, "Head Miss To ReAssign: VID: %d, current version: %d\n", *(int*)(&appendPosting[idx]), version);
                        m_headMiss++;
                        ReassignAsync(vectorContain, *(int*)(&appendPosting[idx]), headID, version);
                    }
                    // LOG(Helper::LogLevel::LL_Info, "Head Miss Do Not To ReAssign: VID: %d, version: %d, current version: %d\n", *(int*)(&appendPosting[idx]), m_versionMap.GetVersion(*(int*)(&appendPosting[idx])), version);
                }
                return ErrorCode::Undefined;
            }
            if (m_postingSizes[headID].load() + appendNum > (m_extraSearcher->GetPostingSizeLimit() + reassignExtraLimit) ) {
                // double splitStartTime = sw.getElapsedMs();
                if (Split(headID, appendNum, appendPosting) == ErrorCode::FailSplit) {
                    goto checkDeleted;
                }
                // m_splitTotalCost += sw.getElapsedMs() - splitStartTime;
            } else {
                // double appendSsdStartTime = sw.getElapsedMs();
                {
                    std::shared_lock<std::shared_timed_mutex> lock(m_rwLocks[headID]);
                    if (!m_index->ContainSample(headID)) {
                        goto checkDeleted;
                    }
                    // LOG(Helper::LogLevel::LL_Info, "Merge: headID: %d, appendNum:%d\n", headID, appendNum);
                    if (m_extraSearcher->AppendPosting(headID, appendPosting) != ErrorCode::Success) {
                        LOG(Helper::LogLevel::LL_Error, "Merge failed!\n");
                    }
                    m_postingSizes[headID].fetch_add(appendNum, std::memory_order_relaxed);
                    // for (int i = 0; i < appendNum; i++)
                    // {
                    //     uint32_t idx = i * vectorInfoSize;
                    //     m_totalReplicaCount[*(int*)(&appendPosting[idx])]++;
                    //     // LOG(Helper::LogLevel::LL_Info, "VID: %d, append to %d, version: %d\n", *(int*)(&appendPosting[idx]), headID, *(uint8_t*)(&appendPosting[idx + sizeof(int)]));
                    // }
                }
                // m_appendSsdCost += sw.getElapsedMs() - appendSsdStartTime;
            }

            // m_appendTotalCost += sw.getElapsedMs();
            return ErrorCode::Success;
        }

        template <typename T>
        void SPTAG::SPANN::Index<T>::ProcessAsyncReassign(std::shared_ptr<std::string> vectorContain, SizeType VID, SizeType HeadPrev, uint8_t version, std::function<void()> p_callback)
        {

            if (m_versionMap.Contains(VID) || !CheckVersionValid(VID, version)) {
                // LOG(Helper::LogLevel::LL_Info, "ReassignID: %d, version: %d, current version: %d\n", VID, version, m_versionMap.GetVersion(VID));
                return;
            }

            
            // tbb::concurrent_hash_map<SizeType, SizeType>::const_accessor VIDAccessor;
            // if (m_reassignMap.find(VIDAccessor, VID) && VIDAccessor->second < version) {
            //     return;
            // }
            // tbb::concurrent_hash_map<SizeType, SizeType>::value_type workPair(VID, version);
            // m_reassignMap.insert(workPair);

            if (ReAssignUpdate(vectorContain, VID, HeadPrev, version))
            //     m_reassignMap.erase(VID);

            if (p_callback != nullptr) {
                p_callback();
            }
        }
    }
}

#define DefineVectorValueType(Name, Type) \
template class SPTAG::SPANN::Index<Type>; \

#include "inc/Core/DefinitionList.h"
#undef DefineVectorValueType


