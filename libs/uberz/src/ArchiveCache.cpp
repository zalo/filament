/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <uberz/ArchiveCache.h>

#include <utils/memalign.h>

#include <zstd.h>

using namespace utils;

// Set this to a certain spec index to find out why it was deemed unsuitable.
// To find the spec index of interest, try invoking uberz with the verbose flag.
#define DEBUG_SPEC_INDEX -1
#define DEBUG_SUITABILILITY(BODY)                    \
    if constexpr(DEBUG_SPEC_INDEX > 0) {             \
        if (i == DEBUG_SPEC_INDEX) {                 \
            slog.d << "Spec " << DEBUG_SPEC_INDEX    \
                << " is unsuitable due to " << BODY  \
                << io::endl;                         \
        }                                            \
    }

namespace filament::uberz {

static bool strIsEqual(const CString& a, const char* b) {
    return strncmp(a.c_str(), b, a.size()) == 0;
}

void ArchiveCache::load(const void* archiveData, uint64_t archiveByteCount) {
    assert_invariant(mArchive == nullptr && "Do not call load() twice");
    size_t decompSize = ZSTD_getFrameContentSize(archiveData, archiveByteCount);
    if (decompSize == ZSTD_CONTENTSIZE_UNKNOWN || decompSize == ZSTD_CONTENTSIZE_ERROR) {
        PANIC_POSTCONDITION("Decompression error.");
    }
    uint64_t* basePointer = (uint64_t*) utils::aligned_alloc(decompSize, 8);
    ZSTD_decompress(basePointer, decompSize, archiveData, archiveByteCount);
    mArchive = (ReadableArchive*) basePointer;
    convertOffsetsToPointers(mArchive);
    mMaterials = FixedCapacityVector<Material*>(mArchive->specsCount, nullptr);
}

// This loops though all ubershaders and returns the first one that meets the given requirements.
Material* ArchiveCache::getMaterial(const ArchiveRequirements& reqs) {
    assert_invariant(mArchive && "Please call load() before requesting any materials.");

    for (uint64_t i = 0; i < mArchive->specsCount; ++i) {
        const ArchiveSpec& spec = mArchive->specs[i];
        if (spec.blendingMode != INVALID_BLENDING && spec.blendingMode != reqs.blendingMode) {
            DEBUG_SUITABILILITY("blend mode mismatch.");
            continue;
        }
        if (spec.shadingModel != INVALID_SHADING_MODEL && spec.shadingModel != reqs.shadingModel) {
            DEBUG_SUITABILILITY("material model.");
            continue;
        }
        bool specIsSuitable = true;

        // For each feature required by the mesh, this ubershader is suitable only if it includes a
        // feature flag for it and the feature flag is either OPTIONAL or REQUIRED.
        for (const auto& req : reqs.features) {
            const CString& meshRequirement = req.first;
            if (req.second == false) {
                continue;
            }
            bool found = false;
            for (uint64_t j = 0; j < spec.flagsCount && !found; ++j) {
                const ArchiveFlag& flag = spec.flags[j];
                if (strIsEqual(meshRequirement, flag.name)) {
                    if (flag.value != ArchiveFeature::UNSUPPORTED) {
                        found = true;
                    }
                    break;
                }
            }
            if (!found) {
                DEBUG_SUITABILILITY(meshRequirement.c_str());
                specIsSuitable = false;
                break;
            }
        }

        // If this ubershader requires a certain feature to be enabled in the glTF, but the glTF
        // mesh doesn't have it, then this ubershader is not suitable. This occurs very rarely, so
        // it intentionally comes after the other suitability check.
        for (uint64_t j = 0; j < spec.flagsCount && specIsSuitable; ++j) {
            ArchiveFlag& flag = spec.flags[j];
            if (UTILS_UNLIKELY(flag.value == ArchiveFeature::REQUIRED)) {
                // This allocates a new CString just to make a robin_map lookup, but this is rare
                // because almost none of our feature flags are REQUIRED.
                auto iter = reqs.features.find(CString(flag.name));
                if (iter == reqs.features.end() || iter.value() == false) {
                    DEBUG_SUITABILILITY(flag.name);
                    specIsSuitable = false;
                }
            }
        }

        if (specIsSuitable) {
            if (mMaterials[i] == nullptr) {
                mMaterials[i] = Material::Builder()
                    .package(spec.package, spec.packageByteCount)
                    .build(mEngine);
            }
            return mMaterials[i];
        }
    }
    return nullptr;
}

Material* ArchiveCache::getDefaultMaterial() {
    assert_invariant(mArchive && "Please call load() before requesting any materials.");
    assert_invariant(!mMaterials.empty() && "Archive must have at least one material.");
    if (mMaterials[0] == nullptr) {
        mMaterials[0] = Material::Builder()
            .package(mArchive->specs[0].package, mArchive->specs[0].packageByteCount)
            .build(mEngine);
    }
    return mMaterials[0];
}

void ArchiveCache::destroyMaterials() {
    for (auto mat : mMaterials) mEngine.destroy(mat);
    mMaterials.clear();
}

ArchiveCache::~ArchiveCache() {
    assert_invariant(mMaterials.size() == 0 &&
        "Please call destroyMaterials explicitly to ensure correct destruction order");
    free(mArchive);
}

} // namespace filament::uberz

#if !defined(NDEBUG)

using namespace filament::uberz;
using namespace filament;

inline
const char* toString(Shading shadingModel) noexcept {
    switch (shadingModel) {
        case Shading::UNLIT: return "unlit";
        case Shading::LIT: return "lit";
        case Shading::SUBSURFACE: return "subsurface";
        case Shading::CLOTH: return "cloth";
        case Shading::SPECULAR_GLOSSINESS: return "specularGlossiness";
    }
}

inline
const char* toString(BlendingMode blendingMode) noexcept {
    switch (blendingMode) {
        case BlendingMode::OPAQUE: return "opaque";
        case BlendingMode::TRANSPARENT: return "transparent";
        case BlendingMode::ADD: return "add";
        case BlendingMode::MASKED: return "masked";
        case BlendingMode::FADE: return "fade";
        case BlendingMode::MULTIPLY: return "multiply";
        case BlendingMode::SCREEN: return "screen";
    }
}

io::ostream& operator<<(io::ostream& out, const ArchiveRequirements& reqs) {
    out << "    ShadingModel = " << toString(reqs.shadingModel) << '\n'
        << "    BlendingMode = " << toString(reqs.blendingMode) << '\n';
    for (const auto& pair : reqs.features) {
        out << "    " << pair.first.c_str() << " = " << (pair.second ? "true" : "false") << '\n';
    }
    return out;
}

#endif
