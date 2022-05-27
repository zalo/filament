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

#include <uberz/ReadableArchive.h>

#include <utils/debug.h>

using namespace filament;
using namespace utils;

namespace filament::uberz {

static_assert(sizeof(ReadableArchive) == 4 + 4 + 8 + 8);
static_assert(sizeof(ArchiveSpec) == 4 + 4 + 8 + 8 + 8 + 8);
static_assert(sizeof(ArchiveFlag) == 8 + 8);

void convertOffsetsToPointers(ReadableArchive* archive) {
    assert_invariant(archive->specsOffset % 8 == 0);
    uint64_t* basePointer = (uint64_t*) archive;
    archive->specs = (ArchiveSpec*) (basePointer + archive->specsOffset / 8);
    for (uint64_t i = 0; i < archive->specsCount; ++i) {
        ArchiveSpec& spec = archive->specs[i];
        assert_invariant(spec.flagsOffset % 8 == 0);
        spec.flags = (ArchiveFlag*) (basePointer + (spec.flagsOffset / 8));
        spec.package = ((uint8_t*) basePointer) + spec.packageOffset;
        for (uint64_t j = 0; j < spec.flagsCount; ++j) {
            ArchiveFlag& flag = spec.flags[j];
            flag.name = ((const char*) basePointer) + flag.nameOffset;
        }
    }
}

} // namespace filament::uberz
