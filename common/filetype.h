#pragma once

#include "common.h"
#include "../elf/elf.h"

namespace mold {

enum class FileType {
  UNKNOWN,
  EMPTY,
  ELF_OBJ,
  ELF_DSO,
  MACH_OBJ,
  MACH_EXE,
  MACH_DYLIB,
  MACH_BUNDLE,
  MACH_UNIVERSAL,
  AR,
  THIN_AR,
  TAPI,
  TEXT,
  GCC_LTO_OBJ,
  LLVM_BITCODE,
};

template <typename MappedFile>
bool is_text_file(MappedFile *mf) {
  u8 *data = mf->data;
  return mf->size >= 4 && isprint(data[0]) && isprint(data[1]) &&
         isprint(data[2]) && isprint(data[3]);
}

inline std::optional<std::string_view>
get_elf_data(std::string_view data, u64 offset, u64 size) {
  if (offset > (u64)data.size() || size > (u64)data.size() - offset)
    return {};
  return data.substr(offset, size);
}

template <typename T>
inline std::optional<std::span<T>>
get_elf_span(std::string_view data, u64 offset, u64 size) {
  if (size % sizeof(T))
    return {};
  if (std::optional<std::string_view> view = get_elf_data(data, offset, size))
    return std::span<T>{(T *)view->data(), view->size() / sizeof(T)};
  return {};
}

inline std::optional<std::string_view>
get_elf_string(std::string_view strtab, u64 offset) {
  if (offset > (u64)strtab.size())
    return {};

  std::string_view str = strtab.substr(offset);
  size_t len = str.find('\0');
  if (len == std::string_view::npos)
    return {};
  return str.substr(0, len);
}

template <typename E>
inline std::optional<std::span<elf::ElfShdr<E>>>
get_elf_shdrs(std::string_view data) {
  using namespace mold::elf;

  if (data.size() < sizeof(ElfEhdr<E>))
    return {};

  ElfEhdr<E> &ehdr = *(ElfEhdr<E> *)data.data();
  u64 shoff = ehdr.e_shoff;

  if (shoff > (u64)data.size())
    return {};

  u64 remain = data.size() - shoff;
  if ((ehdr.e_shnum == 0 || ehdr.e_shstrndx == SHN_XINDEX) &&
      remain < sizeof(ElfShdr<E>))
    return {};

  ElfShdr<E> *sh_begin = (ElfShdr<E> *)(data.data() + shoff);
  u64 num_sections = ehdr.e_shnum ? ehdr.e_shnum : sh_begin->sh_size;

  if (num_sections > remain / sizeof(ElfShdr<E>))
    return {};
  return std::span<ElfShdr<E>>{sh_begin, (size_t)num_sections};
}

template <typename E, typename Context, typename MappedFile>
inline bool is_gcc_lto_obj(Context &ctx, MappedFile *mf) {
  using namespace mold::elf;

  std::string_view data = mf->get_contents();
  if (data.size() < sizeof(ElfEhdr<E>))
    return false;

  ElfEhdr<E> &ehdr = *(ElfEhdr<E> *)data.data();
  std::optional<std::span<ElfShdr<E>>> shdrs = get_elf_shdrs<E>(data);
  if (!shdrs)
    return false;

  // e_shstrndx is a 16-bit field. If .shstrtab's section index is
  // too large, the actual number is stored to sh_link field.
  u64 shstrtab_idx = (ehdr.e_shstrndx == SHN_XINDEX)
    ? (*shdrs)[0].sh_link : ehdr.e_shstrndx;
  if (shstrtab_idx >= shdrs->size())
    return false;

  std::optional<std::string_view> shstrtab =
    get_elf_data(data, (*shdrs)[shstrtab_idx].sh_offset,
                 (*shdrs)[shstrtab_idx].sh_size);
  if (!shstrtab)
    return false;

  for (ElfShdr<E> &sec : *shdrs) {
    // GCC FAT LTO objects contain both regular ELF sections and GCC-
    // specific LTO sections, so that they can be linked as LTO objects if
    // the LTO linker plugin is available and falls back as regular
    // objects otherwise. GCC FAT LTO object can be identified by the
    // presence of `.gcc.lto_.symtab` section.
    if (!ctx.arg.plugin.empty()) {
      std::optional<std::string_view> name = get_elf_string(*shstrtab, sec.sh_name);
      if (!name)
        return false;
      if (name->starts_with(".gnu.lto_.symtab."))
        return true;
    }

    if (sec.sh_type != SHT_SYMTAB)
      continue;

    // GCC non-FAT LTO object contains only sections symbols followed by
    // a common symbol whose name is `__gnu_lto_slim` (or `__gnu_lto_v1`
    // for older GCC releases).
    std::optional<std::span<ElfSym<E>>> elf_syms =
      get_elf_span<ElfSym<E>>(data, sec.sh_offset, sec.sh_size);
    if (!elf_syms)
      return false;

    auto skip = [](u8 type) {
      return type == STT_NOTYPE || type == STT_FILE || type == STT_SECTION;
    };

    i64 i = 1;
    while (i < elf_syms->size() && skip((*elf_syms)[i].st_type))
      i++;

    if (i < elf_syms->size() && (*elf_syms)[i].st_shndx == SHN_COMMON) {
      if (sec.sh_link >= shdrs->size())
        return false;

      std::optional<std::string_view> strtab =
        get_elf_data(data, (*shdrs)[sec.sh_link].sh_offset,
                     (*shdrs)[sec.sh_link].sh_size);
      if (!strtab)
        return false;

      std::optional<std::string_view> name =
        get_elf_string(*strtab, (*elf_syms)[i].st_name);
      if (!name)
        return false;
      if (name->starts_with("__gnu_lto_"))
        return true;
    }
    break;
  }

  return false;
}

template <typename Context, typename MappedFile>
FileType get_file_type(Context &ctx, MappedFile *mf) {
  using namespace elf;

  std::string_view data = mf->get_contents();

  if (data.empty())
    return FileType::EMPTY;

  if (data.starts_with("\177ELF")) {
    if (data.size() < sizeof(ElfEhdr<I386>))
      Fatal(ctx) << mf->name << ": file too small";

    u8 byte_order = ((ElfEhdr<I386> *)data.data())->e_ident[EI_DATA];

    if (byte_order == ELFDATA2LSB) {
      auto &ehdr = *(ElfEhdr<I386> *)data.data();

      if (ehdr.e_type == ET_REL) {
        if (ehdr.e_ident[EI_CLASS] == ELFCLASS32) {
          if (is_gcc_lto_obj<I386>(ctx, mf))
            return FileType::GCC_LTO_OBJ;
        } else {
          if (is_gcc_lto_obj<X86_64>(ctx, mf))
            return FileType::GCC_LTO_OBJ;
        }
        return FileType::ELF_OBJ;
      }

      if (ehdr.e_type == ET_DYN)
        return FileType::ELF_DSO;
    } else {
      auto &ehdr = *(ElfEhdr<M68K> *)data.data();

      if (ehdr.e_type == ET_REL) {
        if (ehdr.e_ident[EI_CLASS] == ELFCLASS32) {
          if (is_gcc_lto_obj<M68K>(ctx, mf))
            return FileType::GCC_LTO_OBJ;
        } else {
          if (is_gcc_lto_obj<SPARC64>(ctx, mf))
            return FileType::GCC_LTO_OBJ;
        }
        return FileType::ELF_OBJ;
      }

      if (ehdr.e_type == ET_DYN)
        return FileType::ELF_DSO;
    }
    return FileType::UNKNOWN;
  }

  if (data.starts_with("\xcf\xfa\xed\xfe")) {
    switch (*(ul32 *)(data.data() + 12)) {
    case 1: // MH_OBJECT
      return FileType::MACH_OBJ;
    case 2: // MH_EXECUTE
      return FileType::MACH_EXE;
    case 6: // MH_DYLIB
      return FileType::MACH_DYLIB;
    case 8: // MH_BUNDLE
      return FileType::MACH_BUNDLE;
    }
    return FileType::UNKNOWN;
  }

  if (data.starts_with("!<arch>\n"))
    return FileType::AR;
  if (data.starts_with("!<thin>\n"))
    return FileType::THIN_AR;
  if (data.starts_with("--- !tapi-tbd"))
    return FileType::TAPI;
  if (data.starts_with("\xca\xfe\xba\xbe"))
    return FileType::MACH_UNIVERSAL;
  if (is_text_file(mf))
    return FileType::TEXT;
  if (data.starts_with("\xde\xc0\x17\x0b"))
    return FileType::LLVM_BITCODE;
  if (data.starts_with("BC\xc0\xde"))
    return FileType::LLVM_BITCODE;
  return FileType::UNKNOWN;
}

inline std::string filetype_to_string(FileType type) {
  switch (type) {
  case FileType::UNKNOWN: return "UNKNOWN";
  case FileType::EMPTY: return "EMPTY";
  case FileType::ELF_OBJ: return "ELF_OBJ";
  case FileType::ELF_DSO: return "ELF_DSO";
  case FileType::MACH_EXE: return "MACH_EXE";
  case FileType::MACH_OBJ: return "MACH_OBJ";
  case FileType::MACH_DYLIB: return "MACH_DYLIB";
  case FileType::MACH_BUNDLE: return "MACH_BUNDLE";
  case FileType::MACH_UNIVERSAL: return "MACH_UNIVERSAL";
  case FileType::AR: return "AR";
  case FileType::THIN_AR: return "THIN_AR";
  case FileType::TAPI: return "TAPI";
  case FileType::TEXT: return "TEXT";
  case FileType::GCC_LTO_OBJ: return "GCC_LTO_OBJ";
  case FileType::LLVM_BITCODE: return "LLVM_BITCODE";
  }
  return "UNKNOWN";
}

inline std::ostream &operator<<(std::ostream &out, FileType type) {
  out << filetype_to_string(type);
  return out;
}

} // namespace mold
