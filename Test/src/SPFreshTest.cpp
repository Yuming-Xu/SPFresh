// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inc/Test.h"

#include "inc/Core/Common.h"
#include "inc/Core/Common/TruthSet.h"
#include "inc/Core/SPANN/Index.h"
#include "inc/Core/VectorIndex.h"
#include "inc/Helper/SimpleIniReader.h"
#include "inc/Helper/StringConvert.h"
#include "inc/Helper/VectorSetReader.h"
#include <future>

#include <iomanip>
#include <iostream>
#include <fstream>

using namespace SPTAG;

namespace SPTAG {
	namespace SSDServing {
        namespace SPFresh {

            typedef std::chrono::steady_clock SteadClock;

            double getMsInterval(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
                return (std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() * 1.0) / 1000.0;
            }

            double getSecInterval(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
                return (std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() * 1.0) / 1000.0;
            }

            double getMinInterval(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
                return (std::chrono::duration_cast<std::chrono::seconds>(end - start).count() * 1.0) / 60.0;
            }

            /// Clock class
            class StopWSPFresh {
            private:
                std::chrono::steady_clock::time_point time_begin;
            public:
                StopWSPFresh() {
                    time_begin = std::chrono::steady_clock::now();
                }

                double getElapsedMs() {
                    std::chrono::steady_clock::time_point time_end = std::chrono::steady_clock::now();
                    return getMsInterval(time_begin, time_end);
                }

                double getElapsedSec() {
                    std::chrono::steady_clock::time_point time_end = std::chrono::steady_clock::now();
                    return getSecInterval(time_begin, time_end);
                }
                    
                double getElapsedMin() {
                    std::chrono::steady_clock::time_point time_end = std::chrono::steady_clock::now();
                    return getMinInterval(time_begin, time_end);
                }

                void reset() {
                    time_begin = std::chrono::steady_clock::now();
                }
            };

            template <typename ValueType>
            void OutputResult(const std::string& p_output, std::vector<QueryResult>& p_results, int p_resultNum)
            {
                if (!p_output.empty())
                {
                    auto ptr = f_createIO();
                    if (ptr == nullptr || !ptr->Initialize(p_output.c_str(), std::ios::binary | std::ios::out)) {
                        LOG(Helper::LogLevel::LL_Error, "Failed create file: %s\n", p_output.c_str());
                        exit(1);
                    }
                    int32_t i32Val = static_cast<int32_t>(p_results.size());
                    if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char*>(&i32Val)) != sizeof(i32Val)) {
                        LOG(Helper::LogLevel::LL_Error, "Fail to write result file!\n");
                        exit(1);
                    }
                    i32Val = p_resultNum;
                    if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char*>(&i32Val)) != sizeof(i32Val)) {
                        LOG(Helper::LogLevel::LL_Error, "Fail to write result file!\n");
                        exit(1);
                    }

                    float fVal = 0;
                    for (size_t i = 0; i < p_results.size(); ++i)
                    {
                        for (int j = 0; j < p_resultNum; ++j)
                        {
                            i32Val = p_results[i].GetResult(j)->VID;
                            if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char*>(&i32Val)) != sizeof(i32Val)) {
                                LOG(Helper::LogLevel::LL_Error, "Fail to write result file!\n");
                                exit(1);
                            }

                            fVal = p_results[i].GetResult(j)->Dist;
                            if (ptr->WriteBinary(sizeof(fVal), reinterpret_cast<char*>(&fVal)) != sizeof(fVal)) {
                                LOG(Helper::LogLevel::LL_Error, "Fail to write result file!\n");
                                exit(1);
                            }
                        }
                    }
                }
            }

            void ShowMemoryStatus(std::shared_ptr<SPTAG::VectorSet> vectorSet, double second)
            {
                int tSize = 0, resident = 0, share = 0;
                std::ifstream buffer("/proc/self/statm");
                buffer >> tSize >> resident >> share;
                buffer.close();
                long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024; // in case x86-64 is configured to use 2MB pages
                long rss = resident * page_size_kb;
                long vector_size = vectorSet->PerVectorDataSize() * (vectorSet->Count() / 1024);
                long vector_size_mb = vector_size / 1024;

                LOG(Helper::LogLevel::LL_Info,"Current time: %.0lf. RSS : %ld MB, Vector Set Size : %ld MB, True Size: %ld MB\n", second, rss / 1024, vector_size_mb, rss / 1024 - vector_size_mb);
            }

            template<typename T, typename V>
            void PrintPercentiles(const std::vector<V>& p_values, std::function<T(const V&)> p_get, const char* p_format, bool reverse=false)
            {
                double sum = 0;
                std::vector<T> collects;
                collects.reserve(p_values.size());
                for (const auto& v : p_values)
                {
                    T tmp = p_get(v);
                    sum += tmp;
                    collects.push_back(tmp);
                }
                if (reverse) {
                    std::sort(collects.begin(), collects.end(), std::greater<T>());
                }
                else {
                    std::sort(collects.begin(), collects.end());
                }
                if (reverse) {
                    LOG(Helper::LogLevel::LL_Info, "Avg\t50tiles\t90tiles\t95tiles\t99tiles\t99.9tiles\tMin\n");
                }
                else {
                    LOG(Helper::LogLevel::LL_Info, "Avg\t50tiles\t90tiles\t95tiles\t99tiles\t99.9tiles\tMax\n");
                }

                std::string formatStr("%.3lf");
                for (int i = 1; i < 7; ++i)
                {
                    formatStr += '\t';
                    formatStr += p_format;
                }

                formatStr += '\n';

                LOG(Helper::LogLevel::LL_Info,
                    formatStr.c_str(),
                    sum / collects.size(),
                    collects[static_cast<size_t>(collects.size() * 0.50)],
                    collects[static_cast<size_t>(collects.size() * 0.90)],
                    collects[static_cast<size_t>(collects.size() * 0.95)],
                    collects[static_cast<size_t>(collects.size() * 0.99)],
                    collects[static_cast<size_t>(collects.size() * 0.999)],
                    collects[static_cast<size_t>(collects.size() - 1)]);
            }

            template <typename T>
            static float CalculateRecallSPFresh(VectorIndex* index, std::vector<QueryResult>& results, const std::vector<std::set<SizeType>>& truth, int K, int truthK, std::shared_ptr<SPTAG::VectorSet> querySet, std::shared_ptr<SPTAG::VectorSet> vectorSet, SizeType NumQuerys, std::ofstream* log = nullptr, bool debug = false)
            {
                float meanrecall = 0, minrecall = MaxDist, maxrecall = 0, stdrecall = 0;
                std::vector<float> thisrecall(NumQuerys, 0);
                std::unique_ptr<bool[]> visited(new bool[K]);
                LOG(Helper::LogLevel::LL_Info, "Start Calculating Recall\n");
                for (SizeType i = 0; i < NumQuerys; i++)
                {
                    memset(visited.get(), 0, K * sizeof(bool));
                    for (SizeType id : truth[i])
                    {
                        for (int j = 0; j < K; j++)
                        {
                            if (visited[j] || results[i].GetResult(j)->VID < 0) continue;
                            if (vectorSet != nullptr) {
                                float dist = results[i].GetResult(j)->Dist;
                                float truthDist = COMMON::DistanceUtils::ComputeDistance((const T*)querySet->GetVector(i), (const T*)vectorSet->GetVector(id), vectorSet->Dimension(), index->GetDistCalcMethod());
                                if (index->GetDistCalcMethod() == SPTAG::DistCalcMethod::Cosine && fabs(dist - truthDist) < Epsilon) {
                                    thisrecall[i] += 1;
                                    visited[j] = true;
                                    break;
                                }
                                else if (index->GetDistCalcMethod() == SPTAG::DistCalcMethod::L2 && fabs(dist - truthDist) < Epsilon * (dist + Epsilon)) {
                                    thisrecall[i] += 1;
                                    visited[j] = true;
                                    break;
                                }
                            }
                        }
                    }
                    thisrecall[i] /= truthK;
                    meanrecall += thisrecall[i];
                    if (thisrecall[i] < minrecall) minrecall = thisrecall[i];
                    if (thisrecall[i] > maxrecall) maxrecall = thisrecall[i];

                    if (debug) {
                        std::string ll("recall:" + std::to_string(thisrecall[i]) + "\ngroundtruth:");
                        std::vector<NodeDistPair> truthvec;
                        for (SizeType id : truth[i]) {
                            float truthDist = 0.0;
                            if (vectorSet != nullptr) {
                                truthDist = COMMON::DistanceUtils::ComputeDistance((const T*)querySet->GetVector(i), (const T*)vectorSet->GetVector(id), querySet->Dimension(), index->GetDistCalcMethod());
                            }
                            truthvec.emplace_back(id, truthDist);
                        }
                        std::sort(truthvec.begin(), truthvec.end());
                        for (int j = 0; j < truthvec.size(); j++)
                            ll += std::to_string(truthvec[j].node) + "@" + std::to_string(truthvec[j].distance) + ",";
                        LOG(Helper::LogLevel::LL_Info, "%s\n", ll.c_str());
                        ll = "ann:";
                        for (int j = 0; j < K; j++)
                            ll += std::to_string(results[i].GetResult(j)->VID) + "@" + std::to_string(results[i].GetResult(j)->Dist) + ",";
                        LOG(Helper::LogLevel::LL_Info, "%s\n", ll.c_str());
                    }
                }
                meanrecall /= NumQuerys;
                for (SizeType i = 0; i < NumQuerys; i++)
                {
                    stdrecall += (thisrecall[i] - meanrecall) * (thisrecall[i] - meanrecall);
                }
                stdrecall = std::sqrt(stdrecall / NumQuerys);

                LOG(Helper::LogLevel::LL_Info, "stdrecall: %.6lf, maxrecall: %.2lf, minrecall: %.2lf\n", stdrecall, maxrecall, minrecall);

                LOG(Helper::LogLevel::LL_Info, "\nRecall Distribution:\n");
                PrintPercentiles<float, float>(thisrecall,
                    [](const float recall) -> float
                    {
                        return recall;
                    },
                    "%.3lf", true);

                LOG(Helper::LogLevel::LL_Info, "Recall%d@%d: %f\n", K, truthK, meanrecall);
                
                if (log) (*log) << meanrecall << " " << stdrecall << " " << minrecall << " " << maxrecall << std::endl;
                return meanrecall;
            }

            template <typename ValueType>
            double SearchSequential(SPANN::Index<ValueType>* p_index,
                int p_numThreads,
                std::vector<QueryResult>& p_results,
                std::vector<SPANN::SearchStats>& p_stats,
                int p_maxQueryCount, int p_internalResultNum)
            {
                int numQueries = min(static_cast<int>(p_results.size()), p_maxQueryCount);

                std::atomic_size_t queriesSent(0);

                std::vector<std::thread> threads;

                StopWSPFresh sw;

                auto func = [&]()
                {
                    StopWSPFresh threadws;
                    size_t index = 0;
                    while (true)
                    {
                        index = queriesSent.fetch_add(1);
                        if (index < numQueries)
                        {

                            double startTime = threadws.getElapsedMs();
                            p_index->GetMemoryIndex()->SearchIndex(p_results[index]);
                            double endTime = threadws.getElapsedMs();

                            p_stats[index].m_totalLatency = endTime - startTime;

                            p_index->DebugSearchDiskIndex(p_results[index], p_internalResultNum, p_internalResultNum, &(p_stats[index]));
                            double exEndTime = threadws.getElapsedMs();

                            p_stats[index].m_exLatency = exEndTime - endTime;
                            p_stats[index].m_totalLatency = p_stats[index].m_totalSearchLatency = exEndTime - startTime;
                        }
                        else
                        {
                            return;
                        }
                    }
                };
                for (int i = 0; i < p_numThreads; i++) { threads.emplace_back(func); }
                for (auto& thread : threads) { thread.join(); }

                auto sendingCost = sw.getElapsedSec();

                return numQueries / sendingCost;
            }

            template <typename ValueType>
            void PrintStats(std::vector<SPANN::SearchStats>& stats)
            {
                LOG(Helper::LogLevel::LL_Info, "\nEx Elements Count:\n");
                PrintPercentiles<double, SPANN::SearchStats>(stats,
                    [](const SPANN::SearchStats& ss) -> double
                    {
                        return ss.m_totalListElementsCount;
                    },
                    "%.3lf");

                LOG(Helper::LogLevel::LL_Info, "\nHead Latency Distribution:\n");
                PrintPercentiles<double, SPANN::SearchStats>(stats,
                    [](const SPANN::SearchStats& ss) -> double
                    {
                        return ss.m_totalSearchLatency - ss.m_exLatency;
                    },
                    "%.3lf");
                
                LOG(Helper::LogLevel::LL_Info, "\nSetup Latency Distribution:\n");
                PrintPercentiles<double, SPANN::SearchStats>(stats,
                    [](const SPANN::SearchStats& ss) -> double
                    {
                        return ss.m_exSetUpLatency;
                    },
                    "%.3lf");

                LOG(Helper::LogLevel::LL_Info, "\nComp Latency Distribution:\n");
                PrintPercentiles<double, SPANN::SearchStats>(stats,
                    [](const SPANN::SearchStats& ss) -> double
                    {
                        return ss.m_compLatency;
                    },
                    "%.3lf");

                LOG(Helper::LogLevel::LL_Info, "\nRocksDB Latency Distribution:\n");
                PrintPercentiles<double, SPANN::SearchStats>(stats,
                    [](const SPANN::SearchStats& ss) -> double
                    {
                        return ss.m_diskReadLatency;
                    },
                    "%.3lf");

                LOG(Helper::LogLevel::LL_Info, "\nEx Latency Distribution:\n");
                PrintPercentiles<double, SPANN::SearchStats>(stats,
                    [](const SPANN::SearchStats& ss) -> double
                    {
                        return ss.m_exLatency;
                    },
                    "%.3lf");

                LOG(Helper::LogLevel::LL_Info, "\nTotal Latency Distribution:\n");
                PrintPercentiles<double, SPANN::SearchStats>(stats,
                    [](const SPANN::SearchStats& ss) -> double
                    {
                        return ss.m_totalSearchLatency;
                    },
                    "%.3lf");

                LOG(Helper::LogLevel::LL_Info, "\nTotal Disk Page Access Distribution(KB):\n");
                PrintPercentiles<int, SPANN::SearchStats>(stats,
                    [](const SPANN::SearchStats& ss) -> int
                    {
                        return ss.m_diskAccessCount;
                    },
                    "%4d");

                LOG(Helper::LogLevel::LL_Info, "\nTotal Disk IO Distribution:\n");
                PrintPercentiles<int, SPANN::SearchStats>(stats,
                    [](const SPANN::SearchStats& ss) -> int
                    {
                        return ss.m_diskIOCount;
                    },
                    "%4d");

                LOG(Helper::LogLevel::LL_Info, "\n");
            }

            void ResetStats(std::vector<SPANN::SearchStats>& totalStats) 
            {
                for (int i = 0; i < totalStats.size(); i++)
                {
                    totalStats[i].m_totalListElementsCount = 0;
                    totalStats[i].m_exLatency = 0;
                    totalStats[i].m_totalSearchLatency = 0;
                    totalStats[i].m_diskAccessCount = 0;
                    totalStats[i].m_diskIOCount = 0;
                    totalStats[i].m_compLatency = 0;
                    totalStats[i].m_diskReadLatency = 0;
                    totalStats[i].m_exSetUpLatency = 0;
                }
            }

            void AddStats(std::vector<SPANN::SearchStats>& totalStats, std::vector<SPANN::SearchStats>& addedStats)
            {
                for (int i = 0; i < totalStats.size(); i++)
                {
                    totalStats[i].m_totalListElementsCount += addedStats[i].m_totalListElementsCount;
                    totalStats[i].m_exLatency += addedStats[i].m_exLatency;
                    totalStats[i].m_totalSearchLatency += addedStats[i].m_totalSearchLatency;
                    totalStats[i].m_diskAccessCount += addedStats[i].m_diskAccessCount;
                    totalStats[i].m_diskIOCount += addedStats[i].m_diskIOCount;
                    totalStats[i].m_compLatency += addedStats[i].m_compLatency;
                    totalStats[i].m_diskReadLatency += addedStats[i].m_diskReadLatency;
                    totalStats[i].m_exSetUpLatency += addedStats[i].m_exSetUpLatency;
                }
            }

            void AvgStats(std::vector<SPANN::SearchStats>& totalStats, int avgStatsNum)
            {
                for (int i = 0; i < totalStats.size(); i++)
                {
                    totalStats[i].m_totalListElementsCount /= avgStatsNum;
                    totalStats[i].m_exLatency /= avgStatsNum;
                    totalStats[i].m_totalSearchLatency /= avgStatsNum;
                    totalStats[i].m_diskAccessCount /= avgStatsNum;
                    totalStats[i].m_diskIOCount /= avgStatsNum;
                    totalStats[i].m_compLatency /= avgStatsNum;
                    totalStats[i].m_diskReadLatency /= avgStatsNum;
                    totalStats[i].m_exSetUpLatency /= avgStatsNum;
                }
            }

            std::string convertFloatToString(const float value, const int precision = 0)
            {
                std::stringstream stream{};
                stream<<std::fixed<<std::setprecision(precision)<<value;
                return stream.str();
            }

            std::string GetTruthFileName(std::string& truthFilePrefix, int vectorCount)
            {
                std::string fileName(truthFilePrefix);
                fileName += "-";
                if (vectorCount < 1000)
                {
                    fileName += std::to_string(vectorCount);
                } 
                else if (vectorCount < 1000000)
                {
                    fileName += std::to_string(vectorCount/1000);
                    fileName += "k";
                }
                else if (vectorCount < 1000000000)
                {
                    if (vectorCount % 1000000 == 0) {
                        fileName += std::to_string(vectorCount/1000000);
                        fileName += "M";
                    } 
                    else 
                    {
                        float vectorCountM = ((float)vectorCount)/1000000;
                        fileName += convertFloatToString(vectorCountM, 2);
                        fileName += "M";
                    }
                }
                else
                {
                    fileName += std::to_string(vectorCount/1000000000);
                    fileName += "B";
                }
                return fileName;
            }

            std::shared_ptr<VectorSet> LoadVectorSet(SPANN::Options& p_opts, int numThreads)
            {
                std::shared_ptr<VectorSet> vectorSet;
                LOG(Helper::LogLevel::LL_Info, "Start loading VectorSet...\n");
                if (!p_opts.m_vectorPath.empty() && fileexists(p_opts.m_vectorPath.c_str())) {
                    std::shared_ptr<Helper::ReaderOptions> vectorOptions(new Helper::ReaderOptions(p_opts.m_valueType, p_opts.m_dim, p_opts.m_vectorType, p_opts.m_vectorDelimiter));
                    auto vectorReader = Helper::VectorSetReader::CreateInstance(vectorOptions);
                    if (ErrorCode::Success == vectorReader->LoadFile(p_opts.m_fullVectorPath))
                    {
                        vectorSet = vectorReader->GetVectorSet();
                        if (p_opts.m_distCalcMethod == DistCalcMethod::Cosine) vectorSet->Normalize(numThreads);
                        LOG(Helper::LogLevel::LL_Info, "\nLoad VectorSet(%d,%d).\n", vectorSet->Count(), vectorSet->Dimension());
                    }
                }
                return vectorSet;
            }

            std::shared_ptr<VectorSet> LoadQuerySet(SPANN::Options& p_opts)
            {
                LOG(Helper::LogLevel::LL_Info, "Start loading QuerySet...\n");
                std::shared_ptr<Helper::ReaderOptions> queryOptions(new Helper::ReaderOptions(p_opts.m_valueType, p_opts.m_dim, p_opts.m_queryType, p_opts.m_queryDelimiter));
                auto queryReader = Helper::VectorSetReader::CreateInstance(queryOptions);
                if (ErrorCode::Success != queryReader->LoadFile(p_opts.m_queryPath))
                {
                    LOG(Helper::LogLevel::LL_Error, "Failed to read query file.\n");
                    exit(1);
                }
                return queryReader->GetVectorSet();
            }

            void LoadTruth(SPANN::Options& p_opts, std::vector<std::set<SizeType>>& truth, int numQueries, std::string truthfilename, int truthK)
            {
                auto ptr = f_createIO();
                if (p_opts.m_update) {
                    LOG(Helper::LogLevel::LL_Info, "Start loading TruthFile...: %s\n", truthfilename.c_str());
                    
                    if (ptr == nullptr || !ptr->Initialize(truthfilename.c_str(), std::ios::in | std::ios::binary)) {
                        LOG(Helper::LogLevel::LL_Error, "Failed open truth file: %s\n", truthfilename.c_str());
                        exit(1);
                    }
                } else {
                    LOG(Helper::LogLevel::LL_Info, "Start loading TruthFile...: %s\n", p_opts.m_truthPath.c_str());
                    
                    if (ptr == nullptr || !ptr->Initialize(p_opts.m_truthPath.c_str(), std::ios::in | std::ios::binary)) {
                        LOG(Helper::LogLevel::LL_Error, "Failed open truth file: %s\n", truthfilename.c_str());
                        exit(1);
                    }
                }
                LOG(Helper::LogLevel::LL_Error, "K: %d, TruthResultNum: %d\n", p_opts.m_resultNum, p_opts.m_truthResultNum);    
                COMMON::TruthSet::LoadTruth(ptr, truth, numQueries, p_opts.m_truthResultNum, p_opts.m_resultNum, p_opts.m_truthType);
                char tmp[4];
                if (ptr->ReadBinary(4, tmp) == 4) {
                    LOG(Helper::LogLevel::LL_Error, "Truth number is larger than query number(%d)!\n", numQueries);
                }
            }

            template <typename ValueType>
            void StableSearch(SPANN::Index<ValueType>* p_index,
                int numThreads,
                std::shared_ptr<SPTAG::VectorSet> querySet,
                std::shared_ptr<SPTAG::VectorSet> vectorSet,
                int avgStatsNum,
                int queryCountLimit,
                int internalResultNum,
                int curCount,
                SPANN::Options& p_opts,
                double second = 0)
            {
                if (avgStatsNum == 0) return;
                int numQueries = querySet->Count();

                std::vector<QueryResult> results(numQueries, QueryResult(NULL, internalResultNum, false));

                LOG(Helper::LogLevel::LL_Info, "Searching: numThread: %d, numQueries: %d, searchTimes: %d.\n", numThreads, numQueries, avgStatsNum);
                std::vector<SPANN::SearchStats> stats(numQueries);
                std::vector<SPANN::SearchStats> TotalStats(numQueries);
                ResetStats(TotalStats);
                double totalQPS = 0;
                for (int i = 0; i < avgStatsNum; i++)
                {
                    for (int j = 0; j < numQueries; ++j)
                    {
                        results[j].SetTarget(reinterpret_cast<ValueType*>(querySet->GetVector(j)));
                        results[j].Reset();
                    }
                    totalQPS += SearchSequential(p_index, numThreads, results, stats, queryCountLimit, internalResultNum);
                    //PrintStats<ValueType>(stats);
                    AddStats(TotalStats, stats);
                }
                LOG(Helper::LogLevel::LL_Info, "Current time: %.0lf, Searching Times: %d, AvgQPS: %.2lf.\n", second, avgStatsNum, totalQPS/avgStatsNum);

                AvgStats(TotalStats, avgStatsNum);

                PrintStats<ValueType>(TotalStats);

                if (p_opts.m_calTruth)
                {
                    // std::vector<std::set<SizeType>> truth;
                    // int truthK = p_opts.m_resultNum;
                    // LoadTruth(p_opts, truth, numQueries, GetTruthFileName(p_opts.m_truthFilePrefix, curCount), truthK);
                    // CalculateRecallSPFresh<ValueType>((p_index->GetMemoryIndex()).get(), results, truth, p_opts.m_resultNum, truthK, querySet, vectorSet, numQueries);
                    OutputResult<ValueType>(GetTruthFileName(p_opts.m_searchResult, curCount), results, p_opts.m_resultNum);
                }
            }

            template <typename ValueType>
            void InsertVectors(SPANN::Index<ValueType>* p_index, 
                int insertThreads, 
                std::shared_ptr<SPTAG::VectorSet> vectorSet, 
                int curCount, 
                int step,
                SPANN::Options& p_opts)
            {
                StopWSPFresh sw;
                std::vector<std::thread> threads;

                std::atomic_size_t vectorsSent(0);

                auto func = [&]()
                {
                    size_t index = 0;
                    while (true)
                    {
                        index = vectorsSent.fetch_add(1);
                        if (index < step)
                        {
                            if ((index & ((1 << 14) - 1)) == 0)
                            {
                                LOG(Helper::LogLevel::LL_Info, "Sent %.2lf%%...\n", index * 100.0 / step);
                            }
                            p_index->AddIndex(vectorSet->GetVector(index + curCount), 1, p_opts.m_dim, nullptr);
                        }
                        else
                        {
                            return;
                        }
                    }
                };
                for (int j = 0; j < insertThreads; j++) { threads.emplace_back(func); }
                for (auto& thread : threads) { thread.join(); }

                double sendingCost = sw.getElapsedSec();
                LOG(Helper::LogLevel::LL_Info,
                "Finish sending in %.3lf seconds, sending throughput is %.2lf , insertion count %u.\n",
                sendingCost,
                step/ sendingCost,
                static_cast<uint32_t>(step));

                LOG(Helper::LogLevel::LL_Info,"During Update\n");
                while(!p_index->AllFinishedExceptReassign())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                double appendSyncingCost = sw.getElapsedSec();
                LOG(Helper::LogLevel::LL_Info,
                "Finish syncing append in %.3lf seconds, actuall throughput is %.2lf, insertion count %u.\n",
                appendSyncingCost,
                step / appendSyncingCost,
                static_cast<uint32_t>(step));

                while(!p_index->AllFinished())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                double syncingCost = sw.getElapsedSec();
                LOG(Helper::LogLevel::LL_Info,
                "Finish syncing in %.3lf seconds, actuall throughput is %.2lf, insertion count %u.\n",
                syncingCost,
                step / syncingCost,
                static_cast<uint32_t>(step));
            }

            template <typename ValueType>
            void UpdateSPFresh(SPANN::Index<ValueType>* p_index)
            {
                SPANN::Options& p_opts = *(p_index->GetOptions());
                int step = p_opts.m_step;
                if (step == 0)
                {
                    LOG(Helper::LogLevel::LL_Error, "Incremental Test Error, Need to set step.\n");
                    exit(1);
                }
                StopWSPFresh sw;

                int numThreads = p_opts.m_searchThreadNum;
                int internalResultNum = p_opts.m_searchInternalResultNum;
                int searchTimes = p_opts.m_searchTimes;

                auto vectorSet = LoadVectorSet(p_opts, numThreads);

                auto querySet = LoadQuerySet(p_opts);

                int curCount = p_index->GetNumSamples();
                int insertCount = vectorSet->Count() - curCount;

                bool calTruthOrigin = p_opts.m_calTruth;

                if (p_opts.m_endVectorNum != -1)
                {
                    insertCount = p_opts.m_endVectorNum - curCount;
                }

                p_index->ForceCompaction();

                if (!p_opts.m_onlySearchFinalBatch) {
                    if (p_opts.m_maxInternalResultNum != -1) 
                    {
                        for (int iterInternalResultNum = p_opts.m_minInternalResultNum; iterInternalResultNum <= p_opts.m_maxInternalResultNum; iterInternalResultNum += p_opts.m_stepInternalResultNum) 
                        {
                            StableSearch(p_index, numThreads, querySet, vectorSet, searchTimes, p_opts.m_queryCountLimit, iterInternalResultNum, curCount, p_opts, sw.getElapsedSec());
                        }
                    }
                    else 
                    {
                        StableSearch(p_index, numThreads, querySet, vectorSet, searchTimes, p_opts.m_queryCountLimit, internalResultNum, curCount, p_opts, sw.getElapsedSec());
                    }
                }

                ShowMemoryStatus(vectorSet, sw.getElapsedSec());
                p_index->GetSomeMemorySize();

                int batch;
                if (step == 0) {
                    batch = 0;
                } else {
                    batch = insertCount / step;
                }
                
                int finishedInsert = 0;
                int insertThreads = p_opts.m_insertThreadNum;

                LOG(Helper::LogLevel::LL_Info, "Updating: numThread: %d, step: %d, totalBatch: %d.\n", insertThreads, step, batch);

                LOG(Helper::LogLevel::LL_Info, "Start updating...\n");
                for (int i = 0; i < batch; i++)
                {   
                    // p_index->QuantifyAssumptionBrokenTotally();
                    LOG(Helper::LogLevel::LL_Info, "Updating Batch %d: numThread: %d, step: %d.\n", i, insertThreads, step);

                    std::future<void> insert_future =
                        std::async(std::launch::async, InsertVectors<ValueType>, p_index,
                                insertThreads, vectorSet, curCount, step, std::ref(p_opts));

                    std::future_status insert_status;

                    p_opts.m_calTruth = false;
                    do {
                        insert_status = insert_future.wait_for(std::chrono::milliseconds(1000));
                        if (insert_status == std::future_status::timeout) {
                            // ShowMemoryStatus(vectorSet, sw.getElapsedSec());
                            if(p_opts.m_searchDuringUpdate) StableSearch(p_index, numThreads, querySet, vectorSet, searchTimes, p_opts.m_queryCountLimit, internalResultNum, curCount, p_opts, sw.getElapsedSec());
                        }
                    }while (insert_status != std::future_status::ready);

                    curCount += step;
                    finishedInsert += step;
                    LOG(Helper::LogLevel::LL_Info, "Total Vector num %d \n", curCount);

                    LOG(Helper::LogLevel::LL_Info, "After %d insertion, head vectors split %d times, head missing %d times, same head %d times, reassign %d(%.2lf) times, reassign scan %ld times, garbage collection %d times\n", finishedInsert, p_index->getSplitTimes(), p_index->getHeadMiss(), p_index->getSameHead(), p_index->getReassignNum(), p_index->getReassignNum(), p_index->getReAssignScanNum(), p_index->getGarbageNum());

                    ShowMemoryStatus(vectorSet, sw.getElapsedSec());
                    p_index->CalculatePostingDistribution();
                    // p_index->ForceCompaction();

                    p_opts.m_calTruth = calTruthOrigin;
                    if (p_opts.m_onlySearchFinalBatch && batch - 1 != i) continue;
                    if (p_opts.m_maxInternalResultNum != -1) 
                    {
                        for (int iterInternalResultNum = p_opts.m_minInternalResultNum; iterInternalResultNum <= p_opts.m_maxInternalResultNum; iterInternalResultNum += p_opts.m_stepInternalResultNum) 
                        {
                            StableSearch(p_index, numThreads, querySet, vectorSet, searchTimes, p_opts.m_queryCountLimit, iterInternalResultNum, curCount, p_opts, sw.getElapsedSec());
                        }
                    }
                    else 
                    {
                        StableSearch(p_index, numThreads, querySet, vectorSet, searchTimes, p_opts.m_queryCountLimit, internalResultNum, curCount, p_opts, sw.getElapsedSec());
                    }
                }
            }

            template <typename ValueType>
            void SearchSPFresh(SPANN::Index<ValueType>* p_index)
            {
                SPANN::Options& p_opts = *(p_index->GetOptions());

                if (p_opts.m_update) 
                {
                    UpdateSPFresh(p_index);
                    return;
                }

                std::string outputFile = p_opts.m_searchResult;
                std::string truthFile = p_opts.m_truthPath;
                std::string warmupFile = p_opts.m_warmupPath;

                if (COMMON::DistanceUtils::Quantizer)
                {
                    LOG(Helper::LogLevel::LL_Error, "Unsupport quantizer!\n");
                    exit(1);
                }

                if (!p_opts.m_logFile.empty())
                {
                    g_pLogger.reset(new Helper::FileLogger(Helper::LogLevel::LL_Info, p_opts.m_logFile.c_str()));
                }
                int numThreads = p_opts.m_searchThreadNum;
                int internalResultNum = p_opts.m_searchInternalResultNum;
                int searchTimes = p_opts.m_searchTimes;

                LOG(Helper::LogLevel::LL_Info, "Start loading QuerySet...\n");
                std::shared_ptr<Helper::ReaderOptions> queryOptions(new Helper::ReaderOptions(p_opts.m_valueType, p_opts.m_dim, p_opts.m_queryType, p_opts.m_queryDelimiter));
                auto queryReader = Helper::VectorSetReader::CreateInstance(queryOptions);
                if (ErrorCode::Success != queryReader->LoadFile(p_opts.m_queryPath))
                {
                    LOG(Helper::LogLevel::LL_Error, "Failed to read query file.\n");
                    exit(1);
                }
                auto querySet = queryReader->GetVectorSet();

                p_index->ForceCompaction();

                std::shared_ptr<VectorSet> vectorSet;

                LOG(Helper::LogLevel::LL_Info, "Start loading VectorSet...\n");
                if (!p_opts.m_vectorPath.empty() && fileexists(p_opts.m_vectorPath.c_str())) {
                    std::shared_ptr<Helper::ReaderOptions> vectorOptions(new Helper::ReaderOptions(p_opts.m_valueType, p_opts.m_dim, p_opts.m_vectorType, p_opts.m_vectorDelimiter));
                    auto vectorReader = Helper::VectorSetReader::CreateInstance(vectorOptions);
                    if (ErrorCode::Success == vectorReader->LoadFile(p_opts.m_vectorPath))
                    {
                        vectorSet = vectorReader->GetVectorSet();
                        if (p_opts.m_distCalcMethod == DistCalcMethod::Cosine) vectorSet->Normalize(numThreads);
                        LOG(Helper::LogLevel::LL_Info, "\nLoad VectorSet(%d,%d).\n", vectorSet->Count(), vectorSet->Dimension());
                    }
                }

                StableSearch(p_index, numThreads, querySet, vectorSet, searchTimes, p_opts.m_queryCountLimit, internalResultNum, vectorSet->Count(), p_opts);
            }

            int UpdateTest(std::map<std::string, std::map<std::string, std::string>>* config_map, 
                const char* configurationPath) {

                VectorValueType valueType;
                DistCalcMethod distCalcMethod;

                bool searchSSD = false;
                Helper::IniReader iniReader;
                iniReader.LoadIniFile(configurationPath);
                (*config_map)["Base"] = iniReader.GetParameters("Base");
                (*config_map)["SelectHead"] = iniReader.GetParameters("SelectHead");
                (*config_map)["BuildHead"] = iniReader.GetParameters("BuildHead");
                (*config_map)["BuildSSDIndex"] = iniReader.GetParameters("BuildSSDIndex");

                valueType = iniReader.GetParameter("Base", "ValueType", valueType);
                distCalcMethod = iniReader.GetParameter("Base", "DistCalcMethod", distCalcMethod);
                bool buildSSD = iniReader.GetParameter("BuildSSDIndex", "isExecute", false);
                searchSSD = iniReader.GetParameter("SearchSSDIndex", "isExecute", false);
                
                for (auto& KV : iniReader.GetParameters("SearchSSDIndex")) {
                    std::string param = KV.first, value = KV.second;
                    if (buildSSD && Helper::StrUtils::StrEqualIgnoreCase(param.c_str(), "BuildSsdIndex")) continue;
                    if (buildSSD && Helper::StrUtils::StrEqualIgnoreCase(param.c_str(), "isExecute")) continue;
                    if (Helper::StrUtils::StrEqualIgnoreCase(param.c_str(), "PostingPageLimit")) param = "SearchPostingPageLimit";
                    if (Helper::StrUtils::StrEqualIgnoreCase(param.c_str(), "InternalResultNum")) param = "SearchInternalResultNum";
                    (*config_map)["BuildSSDIndex"][param] = value;
                }
                
                std::shared_ptr<VectorIndex> index = VectorIndex::CreateInstance(IndexAlgoType::SPANN, valueType);
                if (index == nullptr) {
                    LOG(Helper::LogLevel::LL_Error, "Cannot create Index with ValueType %s!\n", (*config_map)["Base"]["ValueType"].c_str());
                    return -1;
                }

                for (auto& sectionKV : *config_map) {
                    for (auto& KV : sectionKV.second) {
                        index->SetParameter(KV.first, KV.second, sectionKV.first);
                    }
                }

                if (index->BuildIndex() != ErrorCode::Success) {
                    LOG(Helper::LogLevel::LL_Error, "Failed to build index.\n");
                    exit(1);
                }

                SPANN::Options* opts = nullptr;

            #define DefineVectorValueType(Name, Type) \
            if (index->GetVectorValueType() == VectorValueType::Name) { \
            opts = ((SPANN::Index<Type>*)index.get())->GetOptions(); \
            } \

            #include "inc/Core/DefinitionList.h"
            #undef DefineVectorValueType

                if (opts == nullptr) {
                    LOG(Helper::LogLevel::LL_Error, "Cannot get options.\n");
                    exit(1);
                }

                if (opts->m_generateTruth)
                {
                    LOG(Helper::LogLevel::LL_Info, "Start generating truth. It's maybe a long time.\n");
                    if (COMMON::DistanceUtils::Quantizer) valueType = VectorValueType::UInt8;
                    std::shared_ptr<Helper::ReaderOptions> vectorOptions(new Helper::ReaderOptions(valueType, opts->m_dim, opts->m_vectorType, opts->m_vectorDelimiter));
                    auto vectorReader = Helper::VectorSetReader::CreateInstance(vectorOptions);
                    if (ErrorCode::Success != vectorReader->LoadFile(opts->m_vectorPath))
                    {
                        LOG(Helper::LogLevel::LL_Error, "Failed to read vector file.\n");
                        exit(1);
                    }
                    std::shared_ptr<Helper::ReaderOptions> queryOptions(new Helper::ReaderOptions(valueType, opts->m_dim, opts->m_queryType, opts->m_queryDelimiter));
                    auto queryReader = Helper::VectorSetReader::CreateInstance(queryOptions);
                    if (ErrorCode::Success != queryReader->LoadFile(opts->m_queryPath))
                    {
                        LOG(Helper::LogLevel::LL_Error, "Failed to read query file.\n");
                        exit(1);
                    }
                    auto vectorSet = vectorReader->GetVectorSet();
                    auto querySet = queryReader->GetVectorSet();
                    if (distCalcMethod == DistCalcMethod::Cosine) vectorSet->Normalize(opts->m_iSSDNumberOfThreads);

                    omp_set_num_threads(opts->m_iSSDNumberOfThreads);

            #define DefineVectorValueType(Name, Type) \
            if (opts->m_valueType == VectorValueType::Name) { \
            COMMON::TruthSet::GenerateTruth<Type>(querySet, vectorSet, opts->m_truthPath, \
                distCalcMethod, opts->m_resultNum, opts->m_truthType); \
            } \

            #include "inc/Core/DefinitionList.h"
            #undef DefineVectorValueType

                    LOG(Helper::LogLevel::LL_Info, "End generating truth.\n");
                }
                if (searchSSD) {
#define DefineVectorValueType(Name, Type) \
	if (opts->m_valueType == VectorValueType::Name) { \
        SearchSPFresh((SPANN::Index<Type>*)(index.get())); \
	} \

#include "inc/Core/DefinitionList.h"
#undef DefineVectorValueType
			}
                return 0;
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE(SPFreshTest)

BOOST_AUTO_TEST_CASE(SPFreshUpdate)
{
    std::map<std::string, std::map<std::string, std::string>> my_map;
    std::string configPath = "test.ini";
	SSDServing::SPFresh::UpdateTest(&my_map, configPath.data());
}

BOOST_AUTO_TEST_SUITE_END()