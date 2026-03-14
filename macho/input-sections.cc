#include "mold.h"

namespace mold::macho {

template <typename T>
static void sort_relocations_by_offset(std::span<T> vec) {
  if (vec.size() < 2)
    return;

  auto less = [](const T &a, const T &b) { return a.offset < b.offset; };
  if (std::is_sorted(vec.begin(), vec.end(), less))
    return;

  bool reverse_sorted = true;
  for (i64 i = 1; i < vec.size(); i++) {
    if (vec[i - 1].offset < vec[i].offset) {
      reverse_sorted = false;
      break;
    }
  }

  if (reverse_sorted) {
    std::reverse(vec.begin(), vec.end());

    // Preserve original order for relocations that share the same offset,
    // such as SUBTRACTOR/UNSIGNED pairs.
    for (i64 i = 0; i < vec.size();) {
      i64 j = i + 1;
      while (j < vec.size() && vec[i].offset == vec[j].offset)
        j++;
      std::reverse(vec.begin() + i, vec.begin() + j);
      i = j;
    }
    return;
  }

  std::stable_sort(vec.begin(), vec.end(), less);
}

template <typename E>
OutputSection<E> &get_output_section(Context<E> &ctx, const MachSection<E> &hdr) {
  static std::unordered_set<std::string_view> data_const_set = {
    "__got", "__auth_got", "__auth_ptr", "__nl_symbol_ptr", "__const",
    "__cfstring", "__mod_init_func", "__mod_term_func", "__objc_classlist",
    "__objc_nlclslist", "__objc_catlist", "__objc_nlcatlist", "__objc_protolist",
  };

  std::string_view seg = hdr.get_segname();
  std::string_view sect = hdr.get_sectname();

  if (seg == "__DATA" && data_const_set.contains(sect)) {
    seg = "__DATA_CONST";
  } else if (seg == "__TEXT" && sect == "__StaticInit") {
    sect = "__text";
  }

  return *OutputSection<E>::get_instance(ctx, seg, sect);
}

template <typename E>
InputSection<E>::InputSection(Context<E> &ctx, ObjectFile<E> &file,
                              const MachSection<E> &hdr, u32 secidx)
  : file(file), hdr(hdr), secidx(secidx), osec(get_output_section(ctx, hdr)) {
  if (hdr.type != S_ZEROFILL && &file != ctx.internal_obj)
    contents = file.mf->get_contents().substr(hdr.offset, hdr.size);
}

template <typename E>
void InputSection<E>::parse_relocations(Context<E> &ctx) {
  if (hdr.nreloc == 0) {
    for (Subsection<E> *subsec : get_subsections()) {
      subsec->rel_offset = 0;
      subsec->nrels = 0;
    }
    return;
  }

  Timer timer(ctx, "parse_relocations_section");

  {
    Timer t(ctx, "read_relocations_section", &timer);
    i64 start = file.rels_pool.size();
    read_relocations(ctx, file, hdr, file.rels_pool);
    rels = std::span<Relocation<E>>(file.rels_pool).subspan(start);
  }

  if (rels.size() > 1) {
    Timer t(ctx, "sort_relocations_section", &timer);
    sort_relocations_by_offset(rels);
  }

  std::span<Subsection<E> *> subsecs = get_subsections();

  if (subsecs.size() == 1) {
    Subsection<E> *subsec = subsecs[0];
    ASSERT(subsec->isec == this);
    subsec->rel_offset = 0;
    subsec->nrels = rels.size();

    u32 input_offset = subsec->input_addr - hdr.addr;
    if (input_offset)
      for (Relocation<E> &r : rels)
        r.offset -= input_offset;
  } else {
    Timer t(ctx, "assign_relocations_section", &timer);

    i64 i = 0;
    for (Subsection<E> *subsec : subsecs) {
      ASSERT(subsec->isec == this);
      subsec->rel_offset = i;

      u32 input_offset = subsec->input_addr - hdr.addr;
      while (i < rels.size() && rels[i].offset < input_offset + subsec->input_size) {
        rels[i].offset -= input_offset;
        i++;
      }
      subsec->nrels = i - subsec->rel_offset;
    }
  }
}

using E = MOLD_TARGET;

template class InputSection<E>;

} // namespace mold::macho
