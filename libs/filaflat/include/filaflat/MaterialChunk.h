/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef TNT_FILAMAT_MATERIAL_CHUNK_H
#define TNT_FILAMAT_MATERIAL_CHUNK_H

#include <filament/MaterialChunkType.h>

#include <filaflat/Unflattener.h>

#include <private/filament/Variant.h>

#include <tsl/robin_map.h>

#include <vector> // TODO: remove
#include <string> // TODO: remove

namespace filaflat {

class BlobDictionary;
class ChunkContainer;
class ShaderBuilder;

class MaterialChunk {
public:
    explicit MaterialChunk(ChunkContainer const& container);
    ~MaterialChunk() noexcept;

    // call this once after container.parse() has been called
    bool initialize(filamat::ChunkType materialTag);

    // call this as many times as needed
    bool getShader(ShaderBuilder& shaderBuilder,
            BlobDictionary const& dictionary,
            uint8_t shaderModel, filament::Variant variant, uint8_t stage);

    struct TextShaderInfo {
        uint8_t model;
        filament::Variant variant;
        uint8_t stage;
        uint32_t offset;
        std::vector<uint16_t> lineIndices;
        std::string decodedShaderText;
        uint32_t stringLength;
    };

    // Writes up to "count" records into the given pointer, or returns the available number.
    size_t enumerateTextShaders(TextShaderInfo* records, size_t count,
            BlobDictionary const& lines);

private:
    ChunkContainer const& mContainer;
    filamat::ChunkType mMaterialTag = filamat::ChunkType::Unknown;
    Unflattener mUnflattener{nullptr, nullptr};
    const uint8_t* mBase = nullptr;
    tsl::robin_map<uint32_t, uint32_t> mOffsets;

    bool getTextShader(Unflattener unflattener,
            BlobDictionary const& dictionary, ShaderBuilder& shaderBuilder,
            uint8_t shaderModel, filament::Variant variant, uint8_t ps);

    bool getSpirvShader(
            BlobDictionary const& dictionary, ShaderBuilder& shaderBuilder,
            uint8_t shaderModel, filament::Variant variant, uint8_t stage);
};

} // namespace filamat

#endif // TNT_FILAMAT_MATERIAL_CHUNK_H
