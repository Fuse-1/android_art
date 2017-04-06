/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <memory>

#include "android-base/stringprintf.h"

#include "dex_file.h"
#include "dex_ir.h"
#include "dex_ir_builder.h"
#include "pagemap/pagemap.h"
#include "runtime.h"
#include "vdex_file.h"

namespace art {

using android::base::StringPrintf;

static constexpr size_t kLineLength = 32;

static bool g_show_key = false;
static bool g_verbose = false;
static bool g_show_statistics = false;

struct DexSectionInfo {
 public:
  std::string name;
  char letter;
};

static const std::map<uint16_t, DexSectionInfo> kDexSectionInfoMap = {
  { DexFile::kDexTypeHeaderItem, { "Header", 'H' } },
  { DexFile::kDexTypeStringIdItem, { "StringId", 'S' } },
  { DexFile::kDexTypeTypeIdItem, { "TypeId", 'T' } },
  { DexFile::kDexTypeProtoIdItem, { "ProtoId", 'P' } },
  { DexFile::kDexTypeFieldIdItem, { "FieldId", 'F' } },
  { DexFile::kDexTypeMethodIdItem, { "MethodId", 'M' } },
  { DexFile::kDexTypeClassDefItem, { "ClassDef", 'C' } },
  { DexFile::kDexTypeCallSiteIdItem, { "CallSiteId", 'z' } },
  { DexFile::kDexTypeMethodHandleItem, { "MethodHandle", 'Z' } },
  { DexFile::kDexTypeMapList, { "TypeMap", 'L' } },
  { DexFile::kDexTypeTypeList, { "TypeList", 't' } },
  { DexFile::kDexTypeAnnotationSetRefList, { "AnnotationSetReferenceItem", '1' } },
  { DexFile::kDexTypeAnnotationSetItem, { "AnnotationSetItem", '2' } },
  { DexFile::kDexTypeClassDataItem, { "ClassData", 'c' } },
  { DexFile::kDexTypeCodeItem, { "CodeItem", 'X' } },
  { DexFile::kDexTypeStringDataItem, { "StringData", 's' } },
  { DexFile::kDexTypeDebugInfoItem, { "DebugInfo", 'D' } },
  { DexFile::kDexTypeAnnotationItem, { "AnnotationItem", '3' } },
  { DexFile::kDexTypeEncodedArrayItem, { "EncodedArrayItem", 'E' } },
  { DexFile::kDexTypeAnnotationsDirectoryItem, { "AnnotationsDirectoryItem", '4' } }
};

class PageCount {
 public:
  PageCount() {
    for (auto it = kDexSectionInfoMap.begin(); it != kDexSectionInfoMap.end(); ++it) {
      map_[it->first] = 0;
    }
  }
  void Increment(uint16_t type) {
    map_[type]++;
  }
  size_t Get(uint16_t type) const {
    return map_.at(type);
  }
 private:
  std::map<uint16_t, size_t> map_;
  DISALLOW_COPY_AND_ASSIGN(PageCount);
};

static void PrintLetterKey() {
  std::cout << "letter section_type" << std::endl;
  for (const auto& p : kDexSectionInfoMap) {
    const DexSectionInfo& section_info = p.second;
    std::cout << section_info.letter << "      " << section_info.name.c_str() << std::endl;
  }
}

static char PageTypeChar(uint16_t type) {
  if (kDexSectionInfoMap.find(type) == kDexSectionInfoMap.end()) {
    return '-';
  }
  return kDexSectionInfoMap.find(type)->second.letter;
}

static uint16_t FindSectionTypeForPage(size_t page,
                                       const std::vector<dex_ir::DexFileSection>& sections) {
  for (const auto& section : sections) {
    size_t first_page_of_section = section.offset / kPageSize;
    // Only consider non-empty sections.
    if (section.size == 0) {
      continue;
    }
    // Attribute the page to the highest-offset section that starts before the page.
    if (first_page_of_section <= page) {
      return section.type;
    }
  }
  // If there's no non-zero sized section with an offset below offset we're looking for, it
  // must be the header.
  return DexFile::kDexTypeHeaderItem;
}

static void ProcessPageMap(uint64_t* pagemap,
                           size_t start,
                           size_t end,
                           const std::vector<dex_ir::DexFileSection>& sections,
                           PageCount* page_counts) {
  for (size_t page = start; page < end; ++page) {
    char type_char = '.';
    if (PM_PAGEMAP_PRESENT(pagemap[page])) {
      uint16_t type = FindSectionTypeForPage(page, sections);
      page_counts->Increment(type);
      type_char = PageTypeChar(type);
    }
    if (g_verbose) {
      std::cout << type_char;
      if ((page - start) % kLineLength == kLineLength - 1) {
        std::cout << std::endl;
      }
    }
  }
  if (g_verbose) {
    if ((end - start) % kLineLength != 0) {
      std::cout << std::endl;
    }
  }
}

static void DisplayDexStatistics(size_t start,
                                 size_t end,
                                 const PageCount& resident_pages,
                                 const std::vector<dex_ir::DexFileSection>& sections) {
  // Compute the total possible sizes for sections.
  PageCount mapped_pages;
  DCHECK_GE(end, start);
  size_t total_mapped_pages = end - start;
  if (total_mapped_pages == 0) {
    return;
  }
  for (size_t page = start; page < end; ++page) {
    mapped_pages.Increment(FindSectionTypeForPage(page, sections));
  }
  size_t total_resident_pages = 0;
  // Compute the width of the section header column in the table (for fixed formatting).
  int section_header_width = 0;
  for (const auto& section_info : kDexSectionInfoMap) {
    section_header_width = std::max(section_header_width,
                                    static_cast<int>(section_info.second.name.length()));
  }
  // The width needed to print a file page offset (32-bit).
  static constexpr int kPageCountWidth =
      static_cast<int>(std::numeric_limits<uint32_t>::digits10);
  // Display the sections.
  static constexpr char kSectionHeader[] = "Section name";
  std::cout << StringPrintf("%-*s %*s %*s %% of   %% of",
                            section_header_width,
                            kSectionHeader,
                            kPageCountWidth,
                            "resident",
                            kPageCountWidth,
                            "total"
                            )
            << std::endl;
  std::cout << StringPrintf("%-*s %*s %*s sect.  total",
                            section_header_width,
                            "",
                            kPageCountWidth,
                            "pages",
                            kPageCountWidth,
                            "pages")
            << std::endl;
  for (size_t i = sections.size(); i > 0; --i) {
    const dex_ir::DexFileSection& section = sections[i - 1];
    const uint16_t type = section.type;
    const DexSectionInfo& section_info = kDexSectionInfoMap.find(type)->second;
    size_t pages_resident = resident_pages.Get(type);
    double percent_resident = 0;
    if (mapped_pages.Get(type) > 0) {
      percent_resident = 100.0 * pages_resident / mapped_pages.Get(type);
    }
    // 6.2 is sufficient to print 0-100% with two decimal places of accuracy.
    std::cout << StringPrintf("%-*s %*zd %*zd %6.2f %6.2f",
                              section_header_width,
                              section_info.name.c_str(),
                              kPageCountWidth,
                              pages_resident,
                              kPageCountWidth,
                              mapped_pages.Get(type),
                              percent_resident,
                              100.0 * pages_resident / total_mapped_pages)
              << std::endl;
    total_resident_pages += pages_resident;
  }
  std::cout << StringPrintf("%-*s %*zd %*zd        %6.2f",
                            section_header_width,
                            "GRAND TOTAL",
                            kPageCountWidth,
                            total_resident_pages,
                            kPageCountWidth,
                            total_mapped_pages,
                            100.0 * total_resident_pages / total_mapped_pages)
            << std::endl
            << std::endl;
}

static void ProcessOneDexMapping(uint64_t* pagemap,
                                 uint64_t map_start,
                                 const DexFile* dex_file,
                                 uint64_t vdex_start) {
  uint64_t dex_file_start = reinterpret_cast<uint64_t>(dex_file->Begin());
  size_t dex_file_size = dex_file->Size();
  if (dex_file_start < vdex_start) {
    std::cerr << "Dex file start offset for "
              << dex_file->GetLocation().c_str()
              << " is incorrect: map start "
              << StringPrintf("%zx > dex start %zx\n", map_start, dex_file_start)
              << std::endl;
    return;
  }
  uint64_t start = (dex_file_start - vdex_start) / kPageSize;
  uint64_t end = RoundUp(start + dex_file_size, kPageSize) / kPageSize;
  std::cout << "DEX "
            << dex_file->GetLocation().c_str()
            << StringPrintf(": %zx-%zx",
                            map_start + start * kPageSize,
                            map_start + end * kPageSize)
            << std::endl;
  // Build a list of the dex file section types, sorted from highest offset to lowest.
  std::vector<dex_ir::DexFileSection> sections;
  {
    std::unique_ptr<dex_ir::Header> header(dex_ir::DexIrBuilder(*dex_file));
    sections = dex_ir::GetSortedDexFileSections(header.get(),
                                                dex_ir::SortDirection::kSortDescending);
  }
  PageCount section_resident_pages;
  ProcessPageMap(pagemap, start, end, sections, &section_resident_pages);
  if (g_show_statistics) {
    DisplayDexStatistics(start, end, section_resident_pages, sections);
  }
}

static bool DisplayMappingIfFromVdexFile(pm_map_t* map) {
  // Confirm that the map is from a vdex file.
  static const char* suffixes[] = { ".vdex" };
  std::string vdex_name;
  bool found = false;
  for (size_t j = 0; j < sizeof(suffixes) / sizeof(suffixes[0]); ++j) {
    if (strstr(pm_map_name(map), suffixes[j]) != nullptr) {
      vdex_name = pm_map_name(map);
      found = true;
      break;
    }
  }
  if (!found) {
    return true;
  }
  // Extract all the dex files from the vdex file.
  std::string error_msg;
  std::unique_ptr<VdexFile> vdex(VdexFile::Open(vdex_name,
                                                false /*writeable*/,
                                                false /*low_4gb*/,
                                                &error_msg /*out*/));
  if (vdex == nullptr) {
    std::cerr << "Could not open vdex file "
              << vdex_name.c_str()
              << ": error "
              << error_msg.c_str()
              << std::endl;
    return false;
  }

  std::vector<std::unique_ptr<const DexFile>> dex_files;
  if (!vdex->OpenAllDexFiles(&dex_files, &error_msg)) {
    std::cerr << "Dex files could not be opened for "
              << vdex_name.c_str()
              << ": error "
              << error_msg.c_str()
              << std::endl;
  }
  // Open the page mapping (one uint64_t per page) for the entire vdex mapping.
  uint64_t* pagemap;
  size_t len;
  if (pm_map_pagemap(map, &pagemap, &len) != 0) {
    std::cerr << "Error creating pagemap." << std::endl;
    return false;
  }
  // Process the dex files.
  std::cout << "MAPPING "
            << pm_map_name(map)
            << StringPrintf(": %zx-%zx", pm_map_start(map), pm_map_end(map))
            << std::endl;
  for (const auto& dex_file : dex_files) {
    ProcessOneDexMapping(pagemap,
                         pm_map_start(map),
                         dex_file.get(),
                         reinterpret_cast<uint64_t>(vdex->Begin()));
  }
  free(pagemap);
  return true;
}


static void Usage(const char* cmd) {
  std::cerr << "Usage: " << cmd << " [-k] [-s] [-v] pid" << std::endl
            << "    -k Shows a key to verbose display characters." << std::endl
            << "    -s Shows section statistics for individual dex files." << std::endl
            << "    -v Verbosely displays resident pages for dex files." << std::endl;
}

static int DexDiagMain(int argc, char* argv[]) {
  if (argc < 2) {
    Usage(argv[0]);
    return EXIT_FAILURE;
  }

  // TODO: add option to track usage by class name, etc.
  for (int i = 1; i < argc - 1; ++i) {
    if (strcmp(argv[i], "-k") == 0) {
      g_show_key = true;
    } else if (strcmp(argv[i], "-s") == 0) {
      g_show_statistics = true;
    } else if (strcmp(argv[i], "-v") == 0) {
      g_verbose = true;
    } else {
      Usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  // Art specific set up.
  InitLogging(argv, Runtime::Aborter);
  MemMap::Init();

  pid_t pid;
  char* endptr;
  pid = (pid_t)strtol(argv[argc - 1], &endptr, 10);
  if (*endptr != '\0' || kill(pid, 0) != 0) {
    std::cerr << StringPrintf("Invalid PID \"%s\".\n", argv[argc - 1]) << std::endl;
    return EXIT_FAILURE;
  }

  // get libpagemap kernel information.
  pm_kernel_t* ker;
  if (pm_kernel_create(&ker) != 0) {
    std::cerr << "Error creating kernel interface -- does this kernel have pagemap?" << std::endl;
    return EXIT_FAILURE;
  }

  // get libpagemap process information.
  pm_process_t* proc;
  if (pm_process_create(ker, pid, &proc) != 0) {
    std::cerr << "Error creating process interface -- does process "
              << pid
              << " really exist?"
              << std::endl;
    return EXIT_FAILURE;
  }

  // Get the set of mappings by the specified process.
  pm_map_t** maps;
  size_t num_maps;
  if (pm_process_maps(proc, &maps, &num_maps) != 0) {
    std::cerr << "Error listing maps." << std::endl;
    return EXIT_FAILURE;
  }

  // Process the mappings that are due to DEX files.
  for (size_t i = 0; i < num_maps; ++i) {
    if (!DisplayMappingIfFromVdexFile(maps[i])) {
      return EXIT_FAILURE;
    }
  }

  if (g_show_key) {
    PrintLetterKey();
  }
  return 0;
}

}  // namespace art

int main(int argc, char* argv[]) {
  return art::DexDiagMain(argc, argv);
}
