/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef TNT_FILAFLAT_BLOBDICTIONARY_H
#define TNT_FILAFLAT_BLOBDICTIONARY_H

#include <cstdint>

#include <stddef.h>

#include <vector>

#include <utils/FixedCapacityVector.h>

namespace filaflat {

// Flat list of blobs that can be referenced by index.
class BlobDictionary {
public:
    BlobDictionary() = default;
    ~BlobDictionary() = default;

    using ShaderContent = utils::FixedCapacityVector<uint8_t>;

    inline void addBlob(const char* blob, size_t len) noexcept {
        mBlobs.emplace_back(len);
        memcpy(mBlobs.back().data(), blob, len);
    }

    inline void addBlob(ShaderContent&& blob) noexcept {
        mBlobs.push_back(std::move(blob));
    }

    inline bool isEmpty() const noexcept {
        return mBlobs.empty();
    }

    inline void reserve(size_t size) {
        mBlobs.reserve(size);
    }

    inline const ShaderContent& getBlob(size_t index) const noexcept {
        return mBlobs[index];
    }

    inline size_t size() const noexcept {
        return mBlobs.size();
    }

private:
    std::vector<ShaderContent> mBlobs;
};

} // namespace filaflat

#endif // TNT_FILAFLAT_BLOBDICTIONARY_H
