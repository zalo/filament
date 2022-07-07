/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <matdbg/ShaderReplacer.h>

#include <backend/DriverEnums.h>

#include <filamat/MaterialBuilder.h>

#include <filaflat/DictionaryReader.h>
#include <filaflat/MaterialChunk.h>

#include <utils/Log.h>

#include <map>
#include <sstream>

#include <GlslangToSpv.h>

#include "sca/builtinResource.h"
#include "sca/GLSLTools.h"

#include "eiff/ChunkContainer.h"
#include "eiff/DictionarySpirvChunk.h"

namespace filament::matdbg {

using namespace backend;
using namespace filaflat;
using namespace filamat;
using namespace glslang;
using namespace utils;

using std::ostream;
using std::stringstream;
using std::streampos;
using std::vector;

// Tiny database of shader text that can import / export MaterialTextChunk and DictionaryTextChunk.
class ShaderIndex {
public:
    ShaderIndex(ChunkType dictTag, ChunkType matTag, const filaflat::ChunkContainer& cc);

    void writeChunks(ostream& stream);

    // Replaces the specified shader text with new content.
    void replaceShader(backend::ShaderModel shaderModel, Variant variant,
            ShaderType stage, const char* source, size_t sourceLength);

    bool isEmpty() const { return mStringLines.size() == 0 && mShaderRecords.size() == 0; }

private:
    struct ShaderRecord {
        uint8_t model;
        Variant variant;
        uint8_t stage;
        uint32_t offset;
        vector<uint16_t> lineIndices;
        std::string decodedShaderText;
        uint32_t stringLength;
    };

    const ChunkType mDictTag;
    const ChunkType mMatTag;
    vector<ShaderRecord> mShaderRecords;
    vector<std::string> mStringLines;
};

// Tiny database of data blobs that can import / export MaterialSpirvChunk and DictionarySpirvChunk.
// The blobs are stored *after* they have been compressed by SMOL-V.
class BlobIndex {
public:
    BlobIndex(ChunkType dictTag, ChunkType matTag, const filaflat::ChunkContainer& cc);

    void writeChunks(ostream& stream);

    // Replaces the specified shader with new content.
    void replaceShader(backend::ShaderModel shaderModel, Variant variant,
            ShaderType stage, const char* source, size_t sourceLength);

    bool isEmpty() const { return mDataBlobs.size() == 0 && mShaderRecords.size() == 0; }

private:
    struct ShaderRecord {
        uint8_t model;
        Variant variant;
        uint8_t stage;
        uint32_t blobIndex;
    };

    const ChunkType mDictTag;
    const ChunkType mMatTag;
    vector<ShaderRecord> mShaderRecords;
    filaflat::BlobDictionary mDataBlobs;
};

ShaderReplacer::ShaderReplacer(Backend backend, const void* data, size_t size) :
        mBackend(backend), mOriginalPackage(data, size) {
    switch (backend) {
        case Backend::OPENGL:
            mMaterialTag = ChunkType::MaterialGlsl;
            mDictionaryTag = ChunkType::DictionaryText;
            break;
        case Backend::METAL:
            mMaterialTag = ChunkType::MaterialMetal;
            mDictionaryTag = ChunkType::DictionaryText;
            break;
        case Backend::VULKAN:
            mMaterialTag = ChunkType::MaterialSpirv;
            mDictionaryTag = ChunkType::DictionarySpirv;
            break;
        default:
            break;
    }
}

ShaderReplacer::~ShaderReplacer() {
    delete mEditedPackage;
}

bool ShaderReplacer::replaceShaderSource(ShaderModel shaderModel, Variant variant,
            ShaderType stage, const char* sourceString, size_t stringLength) {
    if (!mOriginalPackage.parse()) {
        return false;
    }

    filaflat::ChunkContainer const& cc = mOriginalPackage;
    if (!cc.hasChunk(mMaterialTag) || !cc.hasChunk(mDictionaryTag)) {
        return false;
    }

    if (mDictionaryTag == ChunkType::DictionarySpirv) {
        return replaceSpirv(shaderModel, variant, stage, sourceString, stringLength);
    }

    // Clone all chunks except Dictionary* and Material*.
    stringstream sstream(std::string((const char*) cc.getData(), cc.getSize()));
    stringstream tstream;

    {
        uint64_t type;
        uint32_t size;
        vector<uint8_t> content;
        while (sstream) {
            sstream.read((char*) &type, sizeof(type));
            sstream.read((char*) &size, sizeof(size));
            content.resize(size);
            sstream.read((char*) content.data(), size);
            if (ChunkType(type) == mDictionaryTag|| ChunkType(type) == mMaterialTag) {
                continue;
            }
            tstream.write((char*) &type, sizeof(type));
            tstream.write((char*) &size, sizeof(size));
            tstream.write((char*) content.data(), size);
        }
    }

    // Append the new chunks for Dictionary* and Material*.
    ShaderIndex shaderIndex(mDictionaryTag, mMaterialTag, cc);
    if (!shaderIndex.isEmpty()) {
        shaderIndex.replaceShader(shaderModel, variant, stage, sourceString, stringLength);
        shaderIndex.writeChunks(tstream);
    }

    // Copy the new package from the stringstream into a ChunkContainer.
    // The memory gets freed by DebugServer, which has ownership over the material package.
    const size_t size = tstream.str().size();
    uint8_t* data = new uint8_t[size];
    memcpy(data, tstream.str().data(), size);

    assert_invariant(mEditedPackage == nullptr);
    mEditedPackage = new filaflat::ChunkContainer(data, size);

    return true;
}

bool ShaderReplacer::replaceSpirv(ShaderModel shaderModel, Variant variant,
            ShaderType stage, const char* source, size_t sourceLength) {
    assert_invariant(mMaterialTag == ChunkType::MaterialSpirv);

    const EShLanguage shLang = stage == VERTEX ? EShLangVertex : EShLangFragment;

    std::string nullTerminated(source, sourceLength);
    source = nullTerminated.c_str();

    TShader tShader(shLang);
    tShader.setStrings(&source, 1);

    MaterialBuilder::TargetApi targetApi = targetApiFromBackend(mBackend);
    assert_invariant(targetApi == MaterialBuilder::TargetApi::VULKAN);

    const int langVersion = GLSLTools::glslangVersionFromShaderModel(shaderModel);
    const EShMessages msg = GLSLTools::glslangFlagsFromTargetApi(targetApi);
    const bool ok = tShader.parse(&DefaultTBuiltInResource, langVersion, false, msg);
    if (!ok) {
        slog.e << "ShaderReplacer parse:\n" << tShader.getInfoLog() << io::endl;
        return false;
    }

    TProgram program;
    program.addShader(&tShader);
    const bool linkOk = program.link(msg);
    if (!linkOk) {
        slog.e << "ShaderReplacer link:\n" << program.getInfoLog() << io::endl;
        return false;
    }

    // Unfortunately we need to use std::vector to interface with glslang.
    vector<unsigned int> spirv;

    SpvOptions options;
    options.generateDebugInfo = true;
    GlslangToSpv(*tShader.getIntermediate(), spirv, &options);

    source = (const char*) spirv.data();
    sourceLength = spirv.size() * 4;

    slog.i << "Success re-generating SPIR-V. (" << sourceLength << " bytes)" << io::endl;

    // Clone all chunks except Dictionary* and Material*.
    filaflat::ChunkContainer const& cc = mOriginalPackage;
    stringstream sstream(std::string((const char*) cc.getData(), cc.getSize()));
    stringstream tstream;

    {
        uint64_t type;
        uint32_t size;
        vector<uint8_t> content;
        while (sstream) {
            sstream.read((char*) &type, sizeof(type));
            sstream.read((char*) &size, sizeof(size));
            content.resize(size);
            sstream.read((char*) content.data(), size);
            if (ChunkType(type) == mDictionaryTag || ChunkType(type) == mMaterialTag) {
                continue;
            }
            tstream.write((char*) &type, sizeof(type));
            tstream.write((char*) &size, sizeof(size));
            tstream.write((char*) content.data(), size);
        }
    }

    // Append the new chunks for Dictionary* and Material*.
    BlobIndex shaderIndex(mDictionaryTag, mMaterialTag, cc);
    if (!shaderIndex.isEmpty()) {
        shaderIndex.replaceShader(shaderModel, variant, stage, source, sourceLength);
        shaderIndex.writeChunks(tstream);
    }

    // Copy the new package from the stringstream into a ChunkContainer.
    // The memory gets freed by DebugServer, which has ownership over the material package.
    const size_t size = tstream.str().size();
    uint8_t* data = new uint8_t[size];
    memcpy(data, tstream.str().data(), size);

    assert_invariant(mEditedPackage == nullptr);
    mEditedPackage = new filaflat::ChunkContainer(data, size);

    return true;
}

const uint8_t* ShaderReplacer::getEditedPackage() const {
    return  (const uint8_t*) mEditedPackage->getData();
}

size_t ShaderReplacer::getEditedSize() const {
    return mEditedPackage->getSize();
}

ShaderIndex::ShaderIndex(ChunkType dictTag, ChunkType matTag, const filaflat::ChunkContainer& cc) :
        mDictTag(dictTag), mMatTag(matTag) {
    uint32_t count = *((const uint32_t*) cc.getChunkStart(dictTag));
    mStringLines.resize(count);
    const uint8_t* ptr = cc.getChunkStart(dictTag) + 4;
    for (uint32_t i = 0; i < count; i++) {
        mStringLines[i] = std::string((const char*) ptr);
        ptr += mStringLines[i].length() + 1;
    }

    const uint8_t* chunkContent = cc.getChunkStart(matTag);
    const size_t size = cc.getChunkEnd(matTag) - chunkContent;
    stringstream stream(std::string((const char*) chunkContent, size));
    uint64_t recordCount;
    stream.read((char*) &recordCount, sizeof(recordCount));
    mShaderRecords.resize(recordCount);
    for (auto& record : mShaderRecords) {
        stream.read((char*) &record.model, sizeof(ShaderRecord::model));
        stream.read((char*) &record.variant, sizeof(ShaderRecord::variant));
        stream.read((char*) &record.stage, sizeof(ShaderRecord::stage));
        stream.read((char*) &record.offset, sizeof(ShaderRecord::offset));

        const auto previousPosition = stream.tellg();
        stream.seekg(record.offset);
        {
            stream.read((char*) &record.stringLength, sizeof(ShaderRecord::stringLength));

            uint32_t lineCount;
            stream.read((char*) &lineCount, sizeof(lineCount));

            record.lineIndices.resize(lineCount);
            stream.read((char*) record.lineIndices.data(), lineCount * sizeof(uint16_t));
        }
        stream.seekg(previousPosition);
    }
}

void ShaderIndex::writeChunks(ostream& stream) {
    // Perform a prepass to compute dict chunk size.
    uint32_t size = sizeof(uint32_t);
    for (const auto& stringLine : mStringLines) {
        size += stringLine.length() + 1;
    }

    // Serialize the dict chunk.
    uint64_t type = mDictTag;
    stream.write((char*) &type, sizeof(type));
    stream.write((char*) &size, sizeof(size));
    uint32_t count = mStringLines.size();
    stream.write((char*) &count, sizeof(count));
    for (const auto& stringLine : mStringLines) {
        stream.write(stringLine.c_str(), stringLine.length() + 1);
    }

    // Perform a prepass to compute mat chunk size.
    size = sizeof(uint64_t) + mShaderRecords.size() * (
        sizeof(ShaderRecord::model) + sizeof(ShaderRecord::variant) +
        sizeof(ShaderRecord::stage) + sizeof(ShaderRecord::offset));

    for (const auto& record : mShaderRecords) {
        size += sizeof(ShaderRecord::stringLength);
        size += sizeof(uint32_t);
        size += record.lineIndices.size() * sizeof(uint16_t);
    }

    // Serialize the mat chunk.
    type = mMatTag;
    stream.write((char*) &type, sizeof(type));
    stream.write((char*) &size, sizeof(size));
    uint64_t recordCount = mShaderRecords.size();
    stream.write((char*) &recordCount, sizeof(recordCount));
    for (const auto& record : mShaderRecords) {
        stream.write((char*) &record.model, sizeof(ShaderRecord::model));
        stream.write((char*) &record.variant, sizeof(ShaderRecord::variant));
        stream.write((char*) &record.stage, sizeof(ShaderRecord::stage));
        stream.write((char*) &record.offset, sizeof(ShaderRecord::offset));
    }
    for (const auto& record : mShaderRecords) {
        uint32_t lineCount = record.lineIndices.size();
        stream.write((char*) &record.stringLength, sizeof(ShaderRecord::stringLength));
        stream.write((char*) &lineCount, sizeof(lineCount));
        stream.write((char*) record.lineIndices.data(), lineCount * sizeof(uint16_t));
    }
}

void ShaderIndex::replaceShader(backend::ShaderModel shaderModel, Variant variant,
            backend::ShaderType stage, const char* source, size_t sourceLength) {
    // First, deref the indices to create monolithic strings for each shader.
    for (auto& record : mShaderRecords) {
        size_t len = 0;
        for (uint16_t index : record.lineIndices) {
            if (index >= mStringLines.size()) {
                slog.e << "Internal chunk decoding error." << io::endl;
                return;
            }
            len += mStringLines[index].size() + 1;
        }
        record.decodedShaderText.clear();
        record.decodedShaderText.reserve(len);
        for (uint16_t index : record.lineIndices) {
            record.decodedShaderText += mStringLines[index];
            record.decodedShaderText += '\n';
        }
    }

    // Replace the string of interest.
    const uint8_t model = (uint8_t) shaderModel;
    for (auto& record : mShaderRecords) {
        if (record.model == model && record.variant == variant && record.stage == stage) {
            record.decodedShaderText = std::string(source, sourceLength);
            break;
        }
    }

    // Finally, re-encode the shaders into indices as a form of compression.
    std::map<std::string_view, uint16_t> table;
    for (size_t i = 0; i < mStringLines.size(); i++) {
        table.emplace(mStringLines[i], uint16_t(i));
    }

    uint32_t offset = sizeof(uint64_t);
    for (const auto& record : mShaderRecords) {
        offset += sizeof(ShaderRecord::model);
        offset += sizeof(ShaderRecord::variant);
        offset += sizeof(ShaderRecord::stage);
        offset += sizeof(ShaderRecord::offset);
    }

    for (auto& record : mShaderRecords) {
        record.stringLength = record.decodedShaderText.length() + 1;
        record.lineIndices.clear();
        record.offset = offset;

        offset += sizeof(ShaderRecord::stringLength);
        offset += sizeof(uint32_t);

        const char* const start = record.decodedShaderText.c_str();
        const size_t length = record.decodedShaderText.length();
        for (size_t cur = 0; cur < length; cur++) {
            size_t pos = cur;
            size_t len = 0;
            while (start[cur] != '\n' && cur < length) {
                cur++;
                len++;
            }
            if (pos + len > length) {
                slog.e << "Internal chunk encoding error." << io::endl;
                return;
            }
            std::string_view newLine(start + pos, len);
            auto iter = table.find(newLine);
            if (iter == table.end()) {
                size_t index = mStringLines.size();
                if (index > UINT16_MAX) {
                    slog.e << "Chunk encoding error: too many unique codelines." << io::endl;
                    return;
                }
                record.lineIndices.push_back(index);
                table.emplace(newLine, index);
                mStringLines.emplace_back(newLine);
                continue;
            }
            record.lineIndices.push_back(iter->second);
        }
        offset += sizeof(uint16_t) * record.lineIndices.size();
    }
}

BlobIndex::BlobIndex(ChunkType dictTag, ChunkType matTag, const filaflat::ChunkContainer& cc) :
        mDictTag(dictTag), mMatTag(matTag) {
    // Decompress SMOL-V.
    DictionaryReader reader;
    reader.unflatten(cc, mDictTag, mDataBlobs);

    // Parse the metadata.
    const uint8_t* chunkContent = cc.getChunkStart(matTag);
    const size_t size = cc.getChunkEnd(matTag) - chunkContent;
    stringstream stream(std::string((const char*) chunkContent, size));
    uint64_t recordCount;
    stream.read((char*) &recordCount, sizeof(recordCount));
    mShaderRecords.resize(recordCount);
    for (auto& record : mShaderRecords) {
        stream.read((char*) &record.model, sizeof(ShaderRecord::model));
        stream.read((char*) &record.variant, sizeof(ShaderRecord::variant));
        stream.read((char*) &record.stage, sizeof(ShaderRecord::stage));
        stream.read((char*) &record.blobIndex, sizeof(ShaderRecord::blobIndex));
    }
}

void BlobIndex::writeChunks(ostream& stream) {
    // Consolidate equivalent blobs and rewrite the blob indices along the way.
    filamat::BlobDictionary blobs;
    for (auto& record : mShaderRecords) {
        const auto& src = mDataBlobs[record.blobIndex];
        assert(src.size() % 4 == 0);
        const uint32_t* ptr = (const uint32_t*) src.data();
        record.blobIndex = blobs.addBlob(vector<uint32_t>(ptr, ptr + src.size() / 4));
    }

    // Adjust start cursor of flatteners to match alignment of output stream.
    const size_t pad = stream.tellp() % 8;
    const auto initialize = [pad](Flattener& f) {
        for (size_t i = 0; i < pad; i++) {
            f.writeUint8(0);
        }
    };

    // Apply SMOL-V compression and write out the results.
    {
        filamat::ChunkContainer cc;
        cc.addChild<DictionarySpirvChunk>(std::move(blobs), false);

        Flattener prepass = Flattener::getDryRunner();
        initialize(prepass);

        const size_t dictChunkSize = cc.flatten(prepass);
        auto buffer = std::make_unique<uint8_t[]>(dictChunkSize);
        assert_invariant(intptr_t(buffer.get()) % 8 == 0);

        Flattener writer(buffer.get());
        initialize(writer);
        UTILS_UNUSED_IN_RELEASE const size_t written = cc.flatten(writer);

        assert_invariant(written == dictChunkSize);
        stream.write((char*)buffer.get() + pad, dictChunkSize - pad);
    }

    // Compute mat chunk size.
    const uint32_t size = sizeof(uint64_t) +  mShaderRecords.size() *
        (sizeof(ShaderRecord::model) + sizeof(ShaderRecord::variant) +
        sizeof(ShaderRecord::stage) + sizeof(ShaderRecord::blobIndex));

    // Serialize the chunk.
    uint64_t type = mMatTag;
    stream.write((char*) &type, sizeof(type));
    stream.write((char*) &size, sizeof(size));
    const uint64_t recordCount = mShaderRecords.size();
    stream.write((char*) &recordCount, sizeof(recordCount));
    for (const auto& record : mShaderRecords) {
        stream.write((char*) &record.model, sizeof(ShaderRecord::model));
        stream.write((char*) &record.variant, sizeof(ShaderRecord::variant));
        stream.write((char*) &record.stage, sizeof(ShaderRecord::stage));
        stream.write((char*) &record.blobIndex, sizeof(ShaderRecord::blobIndex));
    }
}

void BlobIndex::replaceShader(ShaderModel shaderModel, Variant variant,
            ShaderType stage, const char* source, size_t sourceLength) {
    const uint8_t model = (uint8_t) shaderModel;
    for (auto& record : mShaderRecords) {
        if (record.model == model && record.variant == variant && record.stage == stage) {
            auto& blob = mDataBlobs[record.blobIndex];
            blob.reserve(sourceLength);
            blob.resize(sourceLength);
            memcpy(blob.data(), source, sourceLength);
            return;
        }
    }
    slog.e << "Unable to replace shader." << io::endl;
}

} // namespace filament::matdbg
