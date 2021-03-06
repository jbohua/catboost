#include "static_ctr_provider.h"
#include "json_model_helpers.h"

#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/model/model_export/export_helpers.h>

#include <util/generic/set.h>



NJson::TJsonValue TStaticCtrProvider::ConvertCtrsToJson(const TVector<TModelCtr>& neededCtrs) const {
    NJson::TJsonValue jsonValue;
    if (neededCtrs.empty()) {
        return jsonValue;
    }
    auto compressedModelCtrs = NCatboostModelExportHelpers::CompressModelCtrs(neededCtrs);
    for (size_t idx = 0; idx < compressedModelCtrs.size(); ++idx) {
        auto& proj = *compressedModelCtrs[idx].Projection;
        for (const auto& ctr: compressedModelCtrs[idx].ModelCtrs) {
            NJson::TJsonValue hashValue;
            auto& learnCtr = CtrData.LearnCtrs.at(ctr->Base);
            auto hashIndexResolver = learnCtr.GetIndexHashViewer();
            const ECtrType ctrType = ctr->Base.CtrType;
            TSet<ui64> hashIndexes;
            for (const auto& bucket: hashIndexResolver.GetBuckets()) {
                auto value = bucket.IndexValue;
                if (value == NCatboost::TDenseIndexHashView::NotFoundIndex) {
                    continue;
                }
                if (hashIndexes.find(bucket.Hash) != hashIndexes.end()) {
                    continue;
                } else {
                    hashIndexes.insert(bucket.Hash);
                }
                hashValue.AppendValue(ToString<ui64>(bucket.Hash));
                if (ctrType == ECtrType::BinarizedTargetMeanValue || ctrType == ECtrType::FloatTargetMeanValue) {
                    if (value != NCatboost::TDenseIndexHashView::NotFoundIndex) {
                        auto ctrMean = learnCtr.GetTypedArrayRefForBlobData<TCtrMeanHistory>();
                        const TCtrMeanHistory& ctrMeanHistory = ctrMean[value];
                        hashValue.AppendValue(ctrMeanHistory.Sum);
                        hashValue.AppendValue(ctrMeanHistory.Count);
                    }
                } else  if (ctrType == ECtrType::Counter || ctrType == ECtrType::FeatureFreq) {
                    TConstArrayRef<int> ctrTotal = learnCtr.GetTypedArrayRefForBlobData<int>();
                    hashValue.AppendValue(ctrTotal[value]);
                } else {
                    auto ctrIntArray = learnCtr.GetTypedArrayRefForBlobData<int>();
                    const int targetClassesCount = learnCtr.TargetClassesCount;
                    auto ctrHistory = MakeArrayRef(ctrIntArray.data() + value * targetClassesCount, targetClassesCount);
                    for (int classId = 0; classId < targetClassesCount; ++classId) {
                        hashValue.AppendValue(ctrHistory[classId]);
                    }
                }
            }
            NJson::TJsonValue hash;
            hash["hash_map"] = hashValue;
            hash["hash_stride"] =  hashValue.GetArray().ysize() / hashIndexes.size();
            hash["counter_denominator"] = learnCtr.CounterDenominator;
            TModelCtrBase modelCtrBase;
            modelCtrBase.Projection = proj;
            modelCtrBase.CtrType = ctrType;
            jsonValue.InsertValue(ModelCtrBaseToStr(modelCtrBase), hash);
        }
    }
    return jsonValue;
}

void TStaticCtrProvider::CalcCtrs(const TVector<TModelCtr>& neededCtrs,
                                  const TConstArrayRef<ui8>& binarizedFeatures,
                                  const TConstArrayRef<int>& hashedCatFeatures,
                                  size_t docCount,
                                  TArrayRef<float> result) {
    if (neededCtrs.empty()) {
        return;
    }
    auto compressedModelCtrs = NCatboostModelExportHelpers::CompressModelCtrs(neededCtrs);
    size_t samplesCount = docCount;
    TVector<ui64> ctrHashes(samplesCount);
    TVector<ui64> buckets(samplesCount);
    size_t resultIdx = 0;
    float* resultPtr = result.data();
    TVector<int> transposedCatFeatureIndexes;
    TVector<TBinFeatureIndexValue> binarizedIndexes;
    for (size_t idx = 0; idx < compressedModelCtrs.size(); ++idx) {
        auto& proj = *compressedModelCtrs[idx].Projection;
        binarizedIndexes.clear();
        transposedCatFeatureIndexes.clear();
        for (const auto feature : proj.CatFeatures) {
            transposedCatFeatureIndexes.push_back(CatFeatureIndex.at(feature));
        }
        for (const auto feature : proj.BinFeatures ) {
            binarizedIndexes.push_back(FloatFeatureIndexes.at(feature));
        }
        for (const auto feature : proj.OneHotFeatures ) {
            binarizedIndexes.push_back(OneHotFeatureIndexes.at(feature));
        }
        CalcHashes(binarizedFeatures, hashedCatFeatures, transposedCatFeatureIndexes, binarizedIndexes, docCount, &ctrHashes);
        for (const auto& ctr: compressedModelCtrs[idx].ModelCtrs) {
            auto& learnCtr = CtrData.LearnCtrs.at(ctr->Base);
            auto hashIndexResolver = learnCtr.GetIndexHashViewer();
            const ECtrType ctrType = ctr->Base.CtrType;
            auto ptrBuckets = buckets.data();
            for (size_t docId = 0; docId < samplesCount; ++docId) {
                ptrBuckets[docId] = hashIndexResolver.GetIndex(ctrHashes[docId]);
            }
            if (ctrType == ECtrType::BinarizedTargetMeanValue || ctrType == ECtrType::FloatTargetMeanValue) {
                const auto emptyVal = ctr->Calc(0.f, 0.f);
                auto ctrMean = learnCtr.GetTypedArrayRefForBlobData<TCtrMeanHistory>();
                for (size_t doc = 0; doc < samplesCount; ++doc) {
                    if (ptrBuckets[doc] != NCatboost::TDenseIndexHashView::NotFoundIndex) {
                        const TCtrMeanHistory& ctrMeanHistory = ctrMean[ptrBuckets[doc]];
                        resultPtr[doc + resultIdx] = ctr->Calc(ctrMeanHistory.Sum, ctrMeanHistory.Count);
                    } else {
                        resultPtr[doc + resultIdx] = emptyVal;
                    }
                }
            } else if (ctrType == ECtrType::Counter || ctrType == ECtrType::FeatureFreq) {
                TConstArrayRef<int> ctrTotal = learnCtr.GetTypedArrayRefForBlobData<int>();
                const int denominator = learnCtr.CounterDenominator;
                auto emptyVal = ctr->Calc(0, denominator);
                for (size_t doc = 0; doc < samplesCount; ++doc) {
                    if (ptrBuckets[doc] != NCatboost::TDenseIndexHashView::NotFoundIndex) {
                        resultPtr[doc + resultIdx] = ctr->Calc(ctrTotal[ptrBuckets[doc]], denominator);
                    } else {
                        resultPtr[doc + resultIdx] = emptyVal;
                    }
                }
            } else if (ctrType == ECtrType::Buckets) {
                auto ctrIntArray = learnCtr.GetTypedArrayRefForBlobData<int>();
                const int targetClassesCount = learnCtr.TargetClassesCount;
                auto emptyVal = ctr->Calc(0, 0);
                for (size_t doc = 0; doc < samplesCount; ++doc) {
                    if (ptrBuckets[doc] != NCatboost::TDenseIndexHashView::NotFoundIndex) {
                        int goodCount = 0;
                        int totalCount = 0;
                        auto ctrHistory = MakeArrayRef(ctrIntArray.data() + ptrBuckets[doc] * targetClassesCount, targetClassesCount);
                        goodCount = ctrHistory[ctr->TargetBorderIdx];
                        for (int classId = 0; classId < targetClassesCount; ++classId) {
                            totalCount += ctrHistory[classId];
                        }
                        resultPtr[doc + resultIdx] = ctr->Calc(goodCount, totalCount);
                    } else {
                        resultPtr[doc + resultIdx] = emptyVal;
                    }
                }
            } else {
                auto ctrIntArray = learnCtr.GetTypedArrayRefForBlobData<int>();
                const int targetClassesCount = learnCtr.TargetClassesCount;

                auto emptyVal = ctr->Calc(0, 0);
                if (targetClassesCount > 2) {
                    for (size_t doc = 0; doc < samplesCount; ++doc) {
                        int goodCount = 0;
                        int totalCount = 0;
                        if (ptrBuckets[doc] != NCatboost::TDenseIndexHashView::NotFoundIndex) {
                            auto ctrHistory = MakeArrayRef(ctrIntArray.data() + ptrBuckets[doc] * targetClassesCount, targetClassesCount);
                            for (int classId = 0; classId < ctr->TargetBorderIdx + 1; ++classId) {
                                totalCount += ctrHistory[classId];
                            }
                            for (int classId = ctr->TargetBorderIdx + 1; classId < targetClassesCount; ++classId) {
                                goodCount += ctrHistory[classId];
                            }
                            totalCount += goodCount;
                        }
                        resultPtr[doc + resultIdx] = ctr->Calc(goodCount, totalCount);
                    }
                } else {
                    for (size_t doc = 0; doc < samplesCount; ++doc) {
                        if (ptrBuckets[doc] != NCatboost::TDenseIndexHashView::NotFoundIndex) {
                            const int* ctrHistory = &ctrIntArray[ptrBuckets[doc] * 2];
                            resultPtr[doc + resultIdx] = ctr->Calc(ctrHistory[1], ctrHistory[0] + ctrHistory[1]);
                        } else {
                            resultPtr[doc + resultIdx] = emptyVal;
                        }
                    }
                }
            }
            resultIdx += docCount;
        }
    }
}

bool TStaticCtrProvider::HasNeededCtrs(const TVector<TModelCtr>& neededCtrs) const {
    for (const auto& ctr : neededCtrs) {
        if (!CtrData.LearnCtrs.has(ctr.Base)) {
            return false;
        }
    }
    return true;
}

void TStaticCtrProvider::SetupBinFeatureIndexes(const TVector<TFloatFeature> &floatFeatures,
                                                const TVector<TOneHotFeature> &oheFeatures,
                                                const TVector<TCatFeature> &catFeatures) {
    ui32 currentIndex = 0;
    FloatFeatureIndexes.clear();
    for (const auto& floatFeature : floatFeatures) {
        for (size_t borderIdx = 0; borderIdx < floatFeature.Borders.size(); ++borderIdx) {
            TBinFeatureIndexValue featureIdx{currentIndex, false, (ui8)(borderIdx + 1)};
            TFloatSplit split{floatFeature.FeatureIndex, floatFeature.Borders[borderIdx]};
            FloatFeatureIndexes[split] = featureIdx;
        }
        ++currentIndex;
    }
    OneHotFeatureIndexes.clear();
    for (const auto& oheFeature : oheFeatures) {
        for (int valueId = 0; valueId < oheFeature.Values.ysize(); ++valueId) {
            TBinFeatureIndexValue featureIdx{currentIndex, true, (ui8)(valueId + 1)};
            TOneHotSplit feature{oheFeature.CatFeatureIndex, oheFeature.Values[valueId]};
            OneHotFeatureIndexes[feature] = featureIdx;
        }
        ++currentIndex;
    }
    CatFeatureIndex.clear();
    for (const auto& catFeature : catFeatures) {
        const int prevSize = CatFeatureIndex.ysize();
        CatFeatureIndex[catFeature.FeatureIndex] = prevSize;
    }
}
