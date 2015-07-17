/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include <link.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

// x86 has a vdso, but there's nothing useful to us in it.
#if defined(__aarch64__) || defined(__x86_64__)

#if defined(__aarch64__)
#define VDSO_CLOCK_GETTIME_SYMBOL "__kernel_clock_gettime"
#define VDSO_GETTIMEOFDAY_SYMBOL  "__kernel_gettimeofday"
#elif defined(__x86_64__)
#define VDSO_CLOCK_GETTIME_SYMBOL "__vdso_clock_gettime"
#define VDSO_GETTIMEOFDAY_SYMBOL  "__vdso_gettimeofday"
#endif

#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <time.h>

#include "private/bionic_prctl.h"
#include "private/libc_logging.h"

extern "C" int __clock_gettime(int, timespec*);
extern "C" int __gettimeofday(timeval*, struct timezone*);

struct vdso_entry {
  const char* name;
  void* fn;
};

enum {
  VDSO_CLOCK_GETTIME = 0,
  VDSO_GETTIMEOFDAY,
  VDSO_END
};

static const vdso_entry vdso_entries_template[] = {
  [VDSO_CLOCK_GETTIME] = { VDSO_CLOCK_GETTIME_SYMBOL, reinterpret_cast<void*>(__clock_gettime) },
  [VDSO_GETTIMEOFDAY] = { VDSO_GETTIMEOFDAY_SYMBOL, reinterpret_cast<void*>(__gettimeofday) },
};

static vdso_entry* vdso_entries;

int clock_gettime(int clock_id, timespec* tp) {
  int (*vdso_clock_gettime)(int, timespec*) =
      reinterpret_cast<int (*)(int, timespec*)>(vdso_entries[VDSO_CLOCK_GETTIME].fn);
  return vdso_clock_gettime(clock_id, tp);
}

int gettimeofday(timeval* tv, struct timezone* tz) {
  int (*vdso_gettimeofday)(timeval*, struct timezone*) =
      reinterpret_cast<int (*)(timeval*, struct timezone*)>(vdso_entries[VDSO_GETTIMEOFDAY].fn);
  return vdso_gettimeofday(tv, tz);
}

static void __libc_init_vdso_entries() {
  // Set up the defaults in case we don't have a vdso or can't find everything we're looking for.
  memcpy(vdso_entries, vdso_entries_template, sizeof(vdso_entries_template));

  // Do we have a vdso?
  uintptr_t vdso_ehdr_addr = getauxval(AT_SYSINFO_EHDR);
  ElfW(Ehdr)* vdso_ehdr = reinterpret_cast<ElfW(Ehdr)*>(vdso_ehdr_addr);
  if (vdso_ehdr == nullptr) {
    return;
  }

  // How many symbols does it have?
  size_t symbol_count = 0;
  ElfW(Shdr)* vdso_shdr = reinterpret_cast<ElfW(Shdr)*>(vdso_ehdr_addr + vdso_ehdr->e_shoff);
  for (size_t i = 0; i < vdso_ehdr->e_shnum; ++i) {
    if (vdso_shdr[i].sh_type == SHT_DYNSYM) {
      symbol_count = vdso_shdr[i].sh_size / sizeof(ElfW(Sym));
    }
  }
  if (symbol_count == 0) {
    return;
  }

  // Where's the dynamic table?
  ElfW(Addr) vdso_addr = 0;
  ElfW(Dyn)* vdso_dyn = nullptr;
  ElfW(Phdr)* vdso_phdr = reinterpret_cast<ElfW(Phdr)*>(vdso_ehdr_addr + vdso_ehdr->e_phoff);
  for (size_t i = 0; i < vdso_ehdr->e_phnum; ++i) {
    if (vdso_phdr[i].p_type == PT_DYNAMIC) {
      vdso_dyn = reinterpret_cast<ElfW(Dyn)*>(vdso_ehdr_addr + vdso_phdr[i].p_offset);
    } else if (vdso_phdr[i].p_type == PT_LOAD) {
      vdso_addr = vdso_ehdr_addr + vdso_phdr[i].p_offset - vdso_phdr[i].p_vaddr;
    }
  }
  if (vdso_addr == 0 || vdso_dyn == nullptr) {
    return;
  }

  // Where are the string and symbol tables?
  const char* strtab = nullptr;
  ElfW(Sym)* symtab = nullptr;
  for (ElfW(Dyn)* d = vdso_dyn; d->d_tag != DT_NULL; ++d) {
    if (d->d_tag == DT_STRTAB) {
      strtab = reinterpret_cast<const char*>(vdso_addr + d->d_un.d_ptr);
    } else if (d->d_tag == DT_SYMTAB) {
      symtab = reinterpret_cast<ElfW(Sym)*>(vdso_addr + d->d_un.d_ptr);
    }
  }
  if (strtab == nullptr || symtab == nullptr) {
    return;
  }

  // Are there any symbols we want?
  for (size_t i = 0; i < symbol_count; ++i) {
    for (size_t j = 0; j < VDSO_END; ++j) {
      if (strcmp(vdso_entries[j].name, strtab + symtab[i].st_name) == 0) {
        vdso_entries[j].fn = reinterpret_cast<void*>(vdso_addr + symtab[i].st_value);
      }
    }
  }
}

void __libc_init_vdso() {
  static_assert(PAGE_SIZE >= sizeof(vdso_entries_template), "vdso_entries_template too large");
  vdso_entries = reinterpret_cast<vdso_entry*>(mmap(nullptr, sizeof(vdso_entries_template), PROT_READ|PROT_WRITE,
                                                    MAP_ANONYMOUS|MAP_PRIVATE, -1, 0));
  if (vdso_entries == MAP_FAILED) {
    __libc_fatal("failed to allocate vdso function pointer table: %s", strerror(errno));
  }
  __libc_init_vdso_entries();
  if (mprotect(vdso_entries, sizeof(vdso_entries_template), PROT_READ) == -1) {
    __libc_fatal("failed to mprotect PROT_READ vdso function pointer table: %s", strerror(errno));
  }
  prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, vdso_entries, sizeof(vdso_entries_template), "vdso function pointer table");
}

#else

void __libc_init_vdso() {
}

#endif
