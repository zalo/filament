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

#include <uberz/WritableArchive.h>
#include <uberz/ReadableArchive.h>

#include <zstd.h>

#include <utils/Log.h>

using namespace utils;

struct Keyword {
    const char* const txt;
    const size_t len;
    bool test(const char* cursor) const { return strncmp(cursor, txt, len) == 0; }
};

#define DEFINE_KEYWORD(a, b) static const Keyword a = { b, strlen(b) }

namespace Keywords {
DEFINE_KEYWORD(BlendingMode, "BlendingMode");
DEFINE_KEYWORD(ShadingModel, "ShadingModel");

DEFINE_KEYWORD(unsupported, "unsupported");
DEFINE_KEYWORD(optional, "optional");
DEFINE_KEYWORD(required, "required");

DEFINE_KEYWORD(opaque, "opaque");
DEFINE_KEYWORD(transparent, "transparent");
DEFINE_KEYWORD(add, "add");
DEFINE_KEYWORD(masked, "masked");
DEFINE_KEYWORD(fade, "fade");
DEFINE_KEYWORD(multiply, "multiply");
DEFINE_KEYWORD(screen, "screen");

DEFINE_KEYWORD(unlit, "unlit");
DEFINE_KEYWORD(lit, "lit");
DEFINE_KEYWORD(subsurface, "subsurface");
DEFINE_KEYWORD(cloth, "cloth");
DEFINE_KEYWORD(specularGlossiness, "specularGlossiness");
}

namespace filament::uberz {

static size_t readArchiveFeature(const char* cursor, ArchiveFeature* feature) {
    if (Keywords::unsupported.test(cursor)) {
        *feature = ArchiveFeature::UNSUPPORTED;
        return Keywords::unsupported.len;
    }
    if (Keywords::required.test(cursor)) {
        *feature = ArchiveFeature::REQUIRED;
        return Keywords::required.len;
    }
    if (Keywords::optional.test(cursor)) {
        *feature = ArchiveFeature::OPTIONAL;
        return Keywords::optional.len;
    }
    return 0;
}

static size_t readBlendingMode(const char* cursor, BlendingMode* blending) {
    if (Keywords::opaque.test(cursor)) {
        *blending = BlendingMode::OPAQUE;
        return Keywords::opaque.len;
    }
    if (Keywords::transparent.test(cursor)) {
        *blending = BlendingMode::TRANSPARENT;
        return Keywords::transparent.len;
    }
    if (Keywords::add.test(cursor)) {
        *blending = BlendingMode::ADD;
        return Keywords::add.len;
    }
    if (Keywords::masked.test(cursor)) {
        *blending = BlendingMode::MASKED;
        return Keywords::masked.len;
    }
    if (Keywords::fade.test(cursor)) {
        *blending = BlendingMode::FADE;
        return Keywords::fade.len;
    }
    if (Keywords::multiply.test(cursor)) {
        *blending = BlendingMode::MULTIPLY;
        return Keywords::multiply.len;
    }
    if (Keywords::screen.test(cursor)) {
        *blending = BlendingMode::SCREEN;
        return Keywords::screen.len;
    }
    return 0;
}

static size_t readShadingModel(const char* cursor, Shading* shading) {
    if (Keywords::unlit.test(cursor)) {
        *shading = Shading::UNLIT;
        return Keywords::unlit.len;
    }
    if (Keywords::lit.test(cursor)) {
        *shading = Shading::LIT;
        return Keywords::lit.len;
    }
    if (Keywords::subsurface.test(cursor)) {
        *shading = Shading::SUBSURFACE;
        return Keywords::subsurface.len;
    }
    if (Keywords::cloth.test(cursor)) {
        *shading = Shading::CLOTH;
        return Keywords::cloth.len;
    }
    if (Keywords::specularGlossiness.test(cursor)) {
        *shading = Shading::SPECULAR_GLOSSINESS;
        return Keywords::specularGlossiness.len;
    }
    return 0;
}

static bool isAlphaNumeric(char c) {
    return (c >= '0' && c <= '9') || c == '_'
        || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool isWhitespace(char c) {
    return c == ' ' || c == '\t';
}

static size_t readIdentifier(const char* cursor)  {
    size_t i = 0;
    while (isAlphaNumeric(cursor[i])) i++;
    return i;
}

static size_t readWhitespace(const char* cursor) {
    size_t i = 0;
    while (isWhitespace(cursor[i])) i++;
    return i;
}

void WritableArchive::addMaterial(const char* name, const uint8_t* package, size_t packageSize) {
    Material& material = mMaterials[++mMaterialIndex];

    material = {
        CString(name),
        FixedCapacityVector<uint8_t>(packageSize)
    };
    memcpy(material.package.data(), package, packageSize);

    // We use invalid values to denote "not set". e.g. if the spec file
    // does not set the blend mode, then it can be used for any blend mode.
    material.blendingMode = INVALID_BLENDING;
    material.shadingModel = INVALID_SHADING_MODEL;

    mLineNumber = 1;
}

void WritableArchive::addSpecLine(const char* line) {
    const char* cursor = line;

    assert_invariant(mMaterialIndex > -1);
    Material& material = mMaterials[mMaterialIndex];

    auto emitSyntaxError = [&](const char* msg) {
        const int column = 1 + cursor - line;
        PANIC_POSTCONDITION("%s.spec(%d,%d): %s", material.name.c_str(), mLineNumber, column, msg);
    };

    auto parseFeatureFlag = [&]() {
        size_t length = readIdentifier(cursor);
        if (length == 0) {
            emitSyntaxError("expected identifier");
        }
        CString id(cursor, length);
        cursor += length;
        cursor += readWhitespace(cursor);
        if (*cursor++ != '=') {
            emitSyntaxError("expected equal sign");
        }
        cursor += readWhitespace(cursor);
        length = readArchiveFeature(cursor, &material.flags[id]);
        if (length == 0) {
            emitSyntaxError("expected unsupported / optional / required");
        }
        cursor += length;
    };

    auto parseBlendingMode = [&]() {
        cursor += readWhitespace(cursor);
        if (*cursor++ != '=') {
            emitSyntaxError("expected equal sign");
        }
        cursor += readWhitespace(cursor);
        size_t length = readBlendingMode(cursor, &material.blendingMode);
        if (length == 0) {
            emitSyntaxError("expected lowercase blending mode enum");
        }
        cursor += length;
    };

    auto parseShadingModel = [&]() {
        cursor += readWhitespace(cursor);
        if (*cursor++ != '=') {
            emitSyntaxError("expected equal sign");
        }
        cursor += readWhitespace(cursor);
        size_t length = readShadingModel(cursor, &material.shadingModel);
        if (length == 0) {
            emitSyntaxError("expected lowercase shading enum");
        }
        cursor += length;
    };

    if (cursor[0] == 0 || cursor[0] == '#') {
        ++mLineNumber;
        return;
    }

    if (Keywords::BlendingMode.test(cursor)) {
        cursor += Keywords::BlendingMode.len;
        parseBlendingMode();
    } else if (Keywords::ShadingModel.test(cursor)) {
        cursor += Keywords::ShadingModel.len;
        parseShadingModel();
    } else {
        parseFeatureFlag();
    }

    if (cursor[0] != 0) {
        emitSyntaxError("unexpected trailing character");
    }

    ++mLineNumber;
}

FixedCapacityVector<uint8_t> WritableArchive::serialize() const {
    size_t byteCount = sizeof(ReadableArchive);
    for (const auto& mat : mMaterials) {
        byteCount += sizeof(ArchiveSpec);
    }
    size_t flaglistOffset = byteCount;
    for (const auto& mat : mMaterials) {
        for (const auto& pair : mat.flags) {
            byteCount += sizeof(ArchiveFlag);
        }
    }
    size_t nameOffset = byteCount;
    for (const auto& mat : mMaterials) {
        for (const auto& pair : mat.flags) {
            byteCount += pair.first.size() + 1;
        }
    }
    size_t filamatOffset = byteCount;
    for (const auto& mat : mMaterials) {
        byteCount += mat.package.size();
    }

    ReadableArchive archive;
    archive.magic = 'UBER';
    archive.version = 0;
    archive.specsCount = mMaterials.size();
    archive.specsOffset = sizeof(ReadableArchive);

    auto specs = FixedCapacityVector<ArchiveSpec>::with_capacity(mMaterials.size());
    size_t flagCount = 0;
    for (const auto& mat : mMaterials) {
        ArchiveSpec spec;
        spec.shadingModel = mat.shadingModel;
        spec.blendingMode = mat.blendingMode;
        spec.flagsCount = mat.flags.size();
        spec.flagsOffset = flaglistOffset + flagCount * sizeof(ArchiveFlag);
        spec.packageByteCount = mat.package.size();
        spec.packageOffset = filamatOffset;
        specs.push_back(spec);
        filamatOffset += mat.package.size();
        flagCount += mat.flags.size();
    }

    auto flags = FixedCapacityVector<ArchiveFlag>::with_capacity(flagCount);
    size_t charCount = 0;
    for (const auto& mat : mMaterials) {
        for (const auto& pair : mat.flags) {
            ArchiveFlag flag;
            flag.nameOffset = nameOffset + charCount;
            flag.value = pair.second;
            charCount += pair.first.size() + 1;
            flags.push_back(flag);
        }
    }

    std::string flagNames(charCount, ' ');
    char* flagNamesPtr = flagNames.data();
    for (const auto& mat : mMaterials) {
        for (const auto& pair : mat.flags) {
            memcpy(flagNamesPtr, pair.first.data(), pair.first.size() + 1);
            flagNamesPtr += pair.first.size() + 1;
        }
    }
    assert(flagNamesPtr - flagNames.data() == flagNames.size());

    FixedCapacityVector<uint8_t> outputBuf(byteCount);
    uint8_t* writeCursor = outputBuf.data();
    memcpy(writeCursor, &archive, sizeof(archive));
    writeCursor += sizeof(archive);
    memcpy(writeCursor, specs.data(), sizeof(ArchiveSpec) * specs.size());
    writeCursor += sizeof(ArchiveSpec) * specs.size();
    memcpy(writeCursor, flags.data(), sizeof(ArchiveFlag) * flags.size());
    writeCursor += sizeof(ArchiveFlag) * flags.size();
    memcpy(writeCursor, flagNames.data(), charCount);
    writeCursor += charCount;
    for (const auto& mat : mMaterials) {
        memcpy(writeCursor, mat.package.data(), mat.package.size());
        writeCursor += mat.package.size();
    }
    assert_invariant(writeCursor - outputBuf.data() == outputBuf.size());

    FixedCapacityVector<uint8_t> compressedBuf(ZSTD_compressBound(outputBuf.size()));
    size_t zstdResult = ZSTD_compress(compressedBuf.data(), compressedBuf.size(), outputBuf.data(),
            outputBuf.size(), ZSTD_maxCLevel());
    if (ZSTD_isError(zstdResult)) {
        PANIC_POSTCONDITION("Error during archive compression: %s", ZSTD_getErrorName(zstdResult));
    }

    compressedBuf.resize(zstdResult);

    // Some compilers might perform copy elision it we simply returned compressedBuf here.
    // However, by moving it into a brand new object, we can *guarantee* copy elision.
    return FixedCapacityVector<uint8_t>(std::move(compressedBuf));
}

} // namespace filament::uberz
