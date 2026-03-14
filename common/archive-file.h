// This file contains functions to read an archive file (.a file).
// An archive file is just a bundle of object files. It's similar to
// tar or zip, but the contents are not compressed.
//
// An archive file is either "regular" or "thin". A regular archive
// contains object files directly, while a thin archive contains only
// pathnames. In the latter case, actual file contents have to be read
// from given pathnames. A regular archive is sometimes called "fat"
// archive as opposed to "thin".
//
// If an archive file is given to the linker, the linker pulls out
// object files that are needed to resolve undefined symbols. So,
// bunding object files as an archive and giving that archive to the
// linker has a different meaning than directly giving the same set of
// object files to the linker. The former links only needed object
// files, while the latter links all the given object files.
//
// Therefore, if you link libc.a for example, not all the libc
// functions are linked to your binary. Instead, only object files
// that provides functions and variables used in your program get
// linked. To make this efficient, static library functions are
// usually separated to each object file in an archive file. You can
// see the contents of libc.a by running `ar t
// /usr/lib/x86_64-linux-gnu/libc.a`.

#pragma once

#include "common.h"
#include "filetype.h"

namespace mold {

inline std::optional<u64> parse_ar_num(std::string_view str) {
  while (!str.empty() && str.back() == ' ')
    str.remove_suffix(1);

  if (str.empty())
    return {};

  u64 val = 0;
  for (u8 c : str) {
    if (c < '0' || c > '9')
      return {};
    if (val > (UINT64_MAX - (c - '0')) / 10)
      return {};
    val = val * 10 + (c - '0');
  }
  return val;
}

struct ArHdr {
  char ar_name[16];
  char ar_date[12];
  char ar_uid[6];
  char ar_gid[6];
  char ar_mode[8];
  char ar_size[10];
  char ar_fmag[2];

  bool starts_with(std::string_view s) const {
    return std::string_view(ar_name, s.size()) == s;
  }

  bool is_strtab() const {
    return starts_with("// ");
  }

  bool is_symtab() const {
    return starts_with("/ ") || starts_with("/SYM64/ ");
  }

  bool has_valid_fmag() const {
    return ar_fmag[0] == '`' && ar_fmag[1] == '\n';
  }

  std::optional<u64> get_size() const {
    return parse_ar_num({ar_size, sizeof(ar_size)});
  }

  std::optional<u64> get_bsd_name_len() const {
    if (!starts_with("#1/"))
      return {};
    return parse_ar_num({ar_name + 3, sizeof(ar_name) - 3});
  }

  std::optional<u64> get_sysv_name_offset() const {
    if (!starts_with("/"))
      return {};
    return parse_ar_num({ar_name + 1, sizeof(ar_name) - 1});
  }

  std::optional<std::string>
  read_name(std::string_view strtab, std::string_view body, u64 &name_size) const {
    name_size = 0;

    // BSD-style long filename
    if (starts_with("#1/")) {
      std::optional<u64> namelen = get_bsd_name_len();
      if (!namelen || *namelen > body.size())
        return {};

      std::string name(body.substr(0, *namelen));
      name_size = *namelen;

      if (size_t pos = name.find('\0'); pos != std::string::npos)
        name = name.substr(0, pos);
      return name;
    }

    // SysV-style long filename
    if (starts_with("/")) {
      std::optional<u64> offset = get_sysv_name_offset();
      if (!offset || *offset >= strtab.size())
        return {};

      std::string_view name = strtab.substr(*offset);
      size_t pos = name.find("/\n");
      if (pos == std::string_view::npos)
        return {};
      return std::string(name.substr(0, pos));
    }

    // Short fileanme
    if (const char *end = (char *)memchr(ar_name, '/', sizeof(ar_name)))
      return std::string(ar_name, end);
    return std::string(ar_name, sizeof(ar_name));
  }
};

inline bool is_darwin_symtab_name(std::string_view name) {
  return name == "__.SYMDEF" || name == "__.SYMDEF SORTED" ||
         name == "__.SYMDEF_64" || name == "__.SYMDEF_64 SORTED";
}

inline bool is_darwin_symtab_64(std::string_view name) {
  return name == "__.SYMDEF_64" || name == "__.SYMDEF_64 SORTED";
}

struct ArchiveSymbolInfo {
  std::string_view name;
  i64 member_idx = 0;
};

enum class ArchiveIndexStatus {
  OK,
  NO_DARWIN_SYMTAB,
  EMPTY_DARWIN_SYMTAB,
};

inline std::string_view archive_index_status_name(ArchiveIndexStatus status) {
  switch (status) {
  case ArchiveIndexStatus::OK:
    return "ok";
  case ArchiveIndexStatus::NO_DARWIN_SYMTAB:
    return "no_darwin_symtab";
  case ArchiveIndexStatus::EMPTY_DARWIN_SYMTAB:
    return "empty_darwin_symtab";
  }
  unreachable();
}

template <typename Context, typename MappedFile>
struct ArchiveMemberInfo {
  std::string name;
  std::string path;
  u64 header_offset = 0;
  u64 data_offset = 0;
  u64 data_size = 0;
  bool is_thin = false;
};

template <typename Context, typename MappedFile>
struct ArchiveFileInfo {
  MappedFile *mf = nullptr;
  std::vector<ArchiveMemberInfo<Context, MappedFile>> members;
  std::vector<ArchiveSymbolInfo> symbols;

  MappedFile *get_member(Context &ctx, i64 idx) const {
    const ArchiveMemberInfo<Context, MappedFile> &member = members[idx];

    if (member.is_thin) {
      MappedFile *child = MappedFile::must_open(ctx, member.path);
      child->thin_parent = mf;
      return child;
    }

    return mf->slice(ctx, member.name, member.data_offset, member.data_size);
  }
};

template <typename Context, typename MappedFile>
struct ArchiveReadResult {
  ArchiveIndexStatus status = ArchiveIndexStatus::OK;
  std::optional<ArchiveFileInfo<Context, MappedFile>> info;
};

template <typename Context, typename MappedFile>
static void parse_darwin_archive_symtab(
    Context &ctx,
    const ArchiveFileInfo<Context, MappedFile> &info,
    std::string_view name,
    std::string_view data,
    std::vector<ArchiveSymbolInfo> &symbols) {
  auto corrupted = [&] { Fatal(ctx) << info.mf->name << ": corrupted archive"; };

  std::unordered_map<u64, i64> member_map;
  member_map.reserve(info.members.size());
  for (i64 i = 0; i < info.members.size(); i++)
    member_map.emplace(info.members[i].header_offset, i);

  auto read_u32 = [&](std::string_view buf) {
    if (buf.size() < sizeof(u32))
      corrupted();
    u32 val;
    memcpy(&val, buf.data(), sizeof(val));
    return val;
  };

  auto read_u64 = [&](std::string_view buf) {
    if (buf.size() < sizeof(u64))
      corrupted();
    u64 val;
    memcpy(&val, buf.data(), sizeof(val));
    return val;
  };

  i64 ent_size = is_darwin_symtab_64(name) ? 16 : 8;
  u64 ranlib_bytes = is_darwin_symtab_64(name) ? read_u64(data) : read_u32(data);
  data.remove_prefix(is_darwin_symtab_64(name) ? sizeof(u64) : sizeof(u32));

  if (ranlib_bytes % ent_size || ranlib_bytes > data.size())
    corrupted();

  std::string_view ranlib_data = data.substr(0, ranlib_bytes);
  data.remove_prefix(ranlib_bytes);

  u64 strtab_bytes = is_darwin_symtab_64(name) ? read_u64(data) : read_u32(data);
  data.remove_prefix(is_darwin_symtab_64(name) ? sizeof(u64) : sizeof(u32));

  if (strtab_bytes > data.size())
    corrupted();

  std::string_view strtab = data.substr(0, strtab_bytes);
  symbols.reserve(ranlib_bytes / ent_size);

  for (i64 i = 0; i < ranlib_bytes / ent_size; i++) {
    u64 strx = is_darwin_symtab_64(name) ? read_u64(ranlib_data) : read_u32(ranlib_data);
    u64 off = is_darwin_symtab_64(name) ? read_u64(ranlib_data.substr(8))
                                        : read_u32(ranlib_data.substr(4));
    ranlib_data.remove_prefix(ent_size);

    if (strx >= strtab.size())
      corrupted();

    std::string_view symname = strtab.substr(strx);
    size_t len = symname.find('\0');
    if (len == std::string_view::npos)
      corrupted();

    auto it = member_map.find(off);
    if (it == member_map.end())
      corrupted();

    symbols.push_back({symname.substr(0, len), it->second});
  }
}

template <typename Context, typename MappedFile>
ArchiveReadResult<Context, MappedFile>
read_archive_file(Context &ctx, MappedFile *mf) {
  u8 *begin = mf->data;
  u8 *data = begin + 8;
  u8 *end = begin + mf->size;
  std::string_view strtab;
  std::string symtab_name;
  std::string_view symtab_data;
  std::vector<ArchiveMemberInfo<Context, MappedFile>> members;
  std::vector<ArchiveSymbolInfo> symbols;
  auto corrupted = [&] { Fatal(ctx) << mf->name << ": corrupted archive"; };
  bool is_thin = get_file_type(ctx, mf) == FileType::THIN_AR;

  while (data < end) {
    if ((data - begin) % 2)
      data++;
    if (data == end)
      break;
    if (end - data < sizeof(ArHdr))
      corrupted();

    u64 header_offset = data - begin;
    ArHdr &hdr = *(ArHdr *)data;
    if (!hdr.has_valid_fmag())
      corrupted();

    u8 *body = data + sizeof(hdr);
    std::optional<u64> size = hdr.get_size();
    if (!size || (u64)(end - body) < *size)
      corrupted();
    data = body + *size;

    if (hdr.is_strtab()) {
      strtab = {(char *)body, (size_t)*size};
      continue;
    }

    if (hdr.is_symtab())
      continue;

    u64 name_size = 0;
    std::optional<std::string> name =
      hdr.read_name(strtab, {(char *)body, (size_t)*size}, name_size);
    if (!name || name_size > *size)
      corrupted();

    if (is_darwin_symtab_name(*name)) {
      symtab_name = *name;
      symtab_data = {(char *)body + name_size, (size_t)(*size - name_size)};
      continue;
    }

    ArchiveMemberInfo<Context, MappedFile> &member = members.emplace_back();
    member.name = *name;
    member.header_offset = header_offset;
    member.is_thin = is_thin;

    if (is_thin) {
      member.path = name->starts_with('/') ?
        *name : (filepath(mf->name).parent_path() / *name).string();
    } else {
      member.data_offset = body - begin + name_size;
      member.data_size = *size - name_size;
    }
  }

  if (!symtab_name.empty()) {
    ArchiveFileInfo<Context, MappedFile> info;
    info.mf = mf;
    info.members = members;
    parse_darwin_archive_symtab(ctx, info, symtab_name, symtab_data, symbols);
  }

  if (symtab_name.empty())
    return {.status = ArchiveIndexStatus::NO_DARWIN_SYMTAB};

  return {
    .status = symbols.empty() ? ArchiveIndexStatus::EMPTY_DARWIN_SYMTAB
                              : ArchiveIndexStatus::OK,
    .info = ArchiveFileInfo<Context, MappedFile>{
      .mf = mf,
      .members = std::move(members),
      .symbols = std::move(symbols),
    },
  };
}

template <typename Context, typename MappedFile>
std::vector<MappedFile *>
read_thin_archive_members(Context &ctx, MappedFile *mf) {
  u8 *begin = mf->data;
  u8 *data = begin + 8;
  u8 *end = begin + mf->size;
  std::vector<MappedFile *> vec;
  std::string_view strtab;
  auto corrupted = [&] { Fatal(ctx) << mf->name << ": corrupted archive"; };

  while (data < end) {
    // Each header is aligned to a 2 byte boundary.
    if ((data - begin) % 2)
      data++;
    if (data == end)
      break;
    if (end - data < sizeof(ArHdr))
      corrupted();

    ArHdr &hdr = *(ArHdr *)data;
    if (!hdr.has_valid_fmag())
      corrupted();

    u8 *body = data + sizeof(hdr);
    std::optional<u64> size = hdr.get_size();
    if (!size || (u64)(end - body) < *size)
      corrupted();

    // Read a string table.
    if (hdr.is_strtab()) {
      strtab = {(char *)body, (size_t)*size};
      data = body + *size;
      continue;
    }

    // Skip a symbol table.
    if (hdr.is_symtab()) {
      data = body + *size;
      continue;
    }

    if (!hdr.starts_with("#1/") && !hdr.starts_with("/"))
      Fatal(ctx) << mf->name << ": filename is not stored as a long filename";

    u64 name_size;
    std::optional<std::string> name =
      hdr.read_name(strtab, {(char *)body, (size_t)*size}, name_size);
    if (!name)
      corrupted();

    // Skip if symbol table
    if (is_darwin_symtab_name(*name)) {
      data = body + *size;
      continue;
    }

    std::string path = name->starts_with('/') ?
      *name : (filepath(mf->name).parent_path() / *name).string();
    vec.push_back(MappedFile::must_open(ctx, path));
    vec.back()->thin_parent = mf;
  }
  return vec;
}

template <typename Context, typename MappedFile>
std::vector<MappedFile *> read_fat_archive_members(Context &ctx, MappedFile *mf) {
  u8 *begin = mf->data;
  u8 *data = begin + 8;
  u8 *end = begin + mf->size;
  std::vector<MappedFile *> vec;
  std::string_view strtab;
  auto corrupted = [&] { Fatal(ctx) << mf->name << ": corrupted archive"; };

  while (data < end) {
    if ((data - begin) % 2)
      data++;
    if (data == end)
      break;
    if (end - data < sizeof(ArHdr))
      corrupted();

    ArHdr &hdr = *(ArHdr *)data;
    if (!hdr.has_valid_fmag())
      corrupted();

    u8 *body = data + sizeof(hdr);
    std::optional<u64> size = hdr.get_size();
    if (!size || (u64)(end - body) < *size)
      corrupted();
    data = body + *size;

    // Read if string table
    if (hdr.is_strtab()) {
      strtab = {(char *)body, (size_t)*size};
      continue;
    }

    // Skip if symbol table
    if (hdr.is_symtab())
      continue;

    // Read the name field
    u64 name_size;
    std::optional<std::string> name =
      hdr.read_name(strtab, {(char *)body, (size_t)*size}, name_size);
    if (!name || name_size > *size)
      corrupted();

    // Skip if symbol table
    if (is_darwin_symtab_name(*name))
      continue;

    vec.push_back(mf->slice(ctx, *name, body - begin + name_size, *size - name_size));
  }
  return vec;
}

template <typename Context, typename MappedFile>
std::vector<MappedFile *> read_archive_members(Context &ctx, MappedFile *mf) {
  switch (get_file_type(ctx, mf)) {
  case FileType::AR:
    return read_fat_archive_members(ctx, mf);
  case FileType::THIN_AR:
    return read_thin_archive_members(ctx, mf);
  default:
    unreachable();
  }
}

} // namespace mold
