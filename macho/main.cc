#include "mold.h"
#include "../common/archive-file.h"
#include "../common/output-file.h"
#include "../common/sha.h"

#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>
#include <tbb/concurrent_vector.h>
#include <tbb/global_control.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_sort.h>

#ifndef _WIN32
# include <sys/mman.h>
# include <sys/time.h>
#endif

namespace mold::macho {

static std::pair<std::string_view, std::string_view>
split_string(std::string_view str, char sep) {
  size_t pos = str.find(sep);
  if (pos == str.npos)
    return {str, ""};
  return {str.substr(0, pos), str.substr(pos + 1)};
}

template <typename E>
static bool has_lto_obj(Context<E> &ctx) {
  for (ObjectFile<E> *file : ctx.objs)
    if (file->lto_module && file->is_alive)
      return true;
  return false;
}

template <typename T, typename Pred>
static i64 parallel_erase_if(std::vector<T> &vec, Pred pred) {
  static constexpr i64 GRAIN_SIZE = 4096;

  if (vec.size() < GRAIN_SIZE)
    return std::erase_if(vec, pred);

  i64 num_blocks = (vec.size() + GRAIN_SIZE - 1) / GRAIN_SIZE;
  std::vector<i64> counts(num_blocks);

  tbb::parallel_for((i64)0, num_blocks, [&](i64 block_idx) {
    i64 begin = block_idx * GRAIN_SIZE;
    i64 end = std::min<i64>(begin + GRAIN_SIZE, vec.size());
    i64 count = 0;

    for (i64 i = begin; i < end; i++)
      if (!pred(vec[i]))
        count++;
    counts[block_idx] = count;
  });

  std::vector<i64> offsets(num_blocks + 1);
  for (i64 i = 0; i < num_blocks; i++)
    offsets[i + 1] = offsets[i] + counts[i];

  std::vector<T> compacted(offsets.back());
  tbb::parallel_for((i64)0, num_blocks, [&](i64 block_idx) {
    i64 begin = block_idx * GRAIN_SIZE;
    i64 end = std::min<i64>(begin + GRAIN_SIZE, vec.size());
    i64 out = offsets[block_idx];

    for (i64 i = begin; i < end; i++)
      if (!pred(vec[i]))
        compacted[out++] = std::move(vec[i]);
  });

  i64 removed = vec.size() - compacted.size();
  vec = std::move(compacted);
  return removed;
}

template <typename E>
static void resolve_symbols(Context<E> &ctx) {
  Timer t(ctx, "resolve_symbols");

  std::vector<InputFile<E> *> files;
  append(files, ctx.objs);
  append(files, ctx.dylibs);

  tbb::parallel_for_each(files, [&](InputFile<E> *file) {
    file->resolve_symbols(ctx);
  });

  auto mark_live_objects = [&] {
    // We want to keep symbols that may be referenced indirectly.
    for (std::string_view name : ctx.arg.u) {
      Symbol<E> *sym = get_symbol(ctx, name);
      maybe_extract_archive_member(ctx, *sym);
      if (InputFile<E> *file = sym->file)
        file->is_alive = true;
    }

    maybe_extract_archive_member(ctx, *ctx.arg.entry);
    if (InputFile<E> *file = ctx.arg.entry->file)
      file->is_alive = true;

    if (InputFile<E> *file = ctx._objc_msgSend->file; file && file->is_dylib)
      file->is_alive = true;

    if (!ctx.arg.fixup_chains)
      if (InputFile<E> *file = ctx.dyld_stub_binder->file; file && file->is_dylib)
        file->is_alive = true;

    std::vector<ObjectFile<E> *> live_objs;
    for (ObjectFile<E> *file : ctx.objs)
      if (file->is_alive)
        live_objs.push_back(file);

    for (i64 i = 0; i < live_objs.size(); i++) {
      live_objs[i]->mark_live_objects(ctx, [&](ObjectFile<E> *file) {
        live_objs.push_back(file);
      });
    }
  };

  // Choose archive members before running LTO so archive IR obeys regular
  // archive extraction semantics. Rerun reachability after LTO because the
  // synthesized object may reference additional archive members.
  mark_live_objects();

  if (has_lto_obj(ctx)) {
    do_lto(ctx);
    mark_live_objects();
  }

  files.clear();
  append(files, ctx.objs);
  append(files, ctx.dylibs);

  // Remove symbols of eliminated files.
  tbb::parallel_for_each(files, [&](InputFile<E> *file) {
    if (!file->is_alive)
      file->clear_symbols();
  });

  // Redo symbol resolution because extracting object files from archives
  // may raise the priority of symbols defined by the object file.
  tbb::parallel_for_each(files, [&](InputFile<E> *file) {
    if (file->is_alive)
      file->resolve_symbols(ctx);
  });

  std::erase_if(ctx.objs, [](InputFile<E> *file) { return !file->is_alive; });
  std::erase_if(ctx.dylibs, [](InputFile<E> *file) { return !file->is_alive; });

  for (i64 i = 1; DylibFile<E> *file : ctx.dylibs)
    if (file->dylib_idx != BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE)
      file->dylib_idx = i++;
}

template <typename E>
static void handle_exported_symbols_list(Context<E> &ctx) {
  Timer t(ctx, "handle_exported_symbols_list");
  if (ctx.arg.exported_symbols_list.empty())
    return;

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (Symbol<E> *sym : file->syms)
      if (sym && sym->file == file)
        if (sym->visibility != SCOPE_LOCAL)
          sym->visibility = ctx.arg.exported_symbols_list.find(sym->name)
            ? SCOPE_GLOBAL : SCOPE_MODULE;
  });
}

template <typename E>
static void handle_unexported_symbols_list(Context<E> &ctx) {
  Timer t(ctx, "handle_unexported_symbols_list");
  if (ctx.arg.unexported_symbols_list.empty())
    return;

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (Symbol<E> *sym : file->syms)
      if (sym && sym->file == file)
        if (sym->visibility == SCOPE_GLOBAL &&
            ctx.arg.unexported_symbols_list.find(sym->name))
          sym->visibility = SCOPE_MODULE;
  });
}

template <typename E>
static void compute_import_export(Context<E> &ctx) {
  // Compute is_imported and is_exported values
  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (Symbol<E> *sym : file->syms) {
      if (!sym || sym->visibility != SCOPE_GLOBAL)
        continue;

      // If we are creating a dylib, all global symbols are exported by default.
      if (sym->file == file && ctx.output_type != MH_EXECUTE) {
        std::scoped_lock lock(sym->mu);
        sym->is_exported = true;
        if (ctx.arg.flat_namespace)
          sym->is_imported = true;
        continue;
      }

      // If we are using a dylib symbol, we need to import it.
      if (sym->file && sym->file->is_dylib) {
        std::scoped_lock lock(sym->mu);
        sym->is_imported = true;
      }
    }
  });

  // We want to export symbols referenced by dylibs.
  tbb::parallel_for_each(ctx.dylibs, [&](DylibFile<E> *file) {
    for (Symbol<E> *sym : file->syms) {
      if (sym && sym->file && !sym->file->is_dylib &&
          sym->visibility == SCOPE_GLOBAL) {
        std::scoped_lock lock(sym->mu);
        sym->is_exported = true;
      }
    }
  });

  // Some symbols are referenced only by linker-synthesized sections.
  // We need to handle such symbols too.
  auto import = [&](Symbol<E> &sym) {
    if (!sym.file)
      Error(ctx) << "undefined symbol: " << sym;

    if (sym.file->is_dylib) {
      sym.is_imported = true;
      sym.flags |= NEEDS_GOT;
    }
  };

  if (ctx.objc_stubs)
    import(*ctx._objc_msgSend);
  if (ctx.stub_helper)
    import(*ctx.dyld_stub_binder);
}

template <typename E>
static void create_internal_file(Context<E> &ctx) {
  Timer t(ctx, "create_internal_file");

  ObjectFile<E> *obj = new ObjectFile<E>;
  obj->is_alive = true;
  obj->mach_syms = obj->mach_syms2;
  ctx.obj_pool.emplace_back(obj);
  ctx.objs.push_back(obj);
  ctx.internal_obj = obj;

  auto add = [&](Symbol<E> *sym) {
    sym->file = obj;
    obj->syms.push_back(sym);
  };

  add(ctx.__dyld_private);
  add(ctx.___dso_handle);

  switch (ctx.output_type) {
  case MH_EXECUTE: {
    add(ctx.__mh_execute_header);
    ctx.__mh_execute_header->visibility = SCOPE_GLOBAL;
    ctx.__mh_execute_header->is_exported = true;
    ctx.__mh_execute_header->value = ctx.arg.pagezero_size;
    break;
  }
  case MH_DYLIB:
    add(ctx.__mh_dylib_header);
    break;
  case MH_BUNDLE:
    add(ctx.__mh_bundle_header);
    break;
  default:
    unreachable();
  }

  // Add start stop symbols.
  std::set<std::string_view> start_stop_symbols;
  std::mutex mu;

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    std::set<std::string_view> set;
    for (Symbol<E> *sym : file->syms)
      if (!sym->file)
        if (sym->name.starts_with("segment$start$") ||
            sym->name.starts_with("segment$end$") ||
            sym->name.starts_with("section$start$") ||
            sym->name.starts_with("section$end$"))
          set.insert(sym->name);

    std::scoped_lock lock(mu);
    start_stop_symbols.merge(set);
  });

  for (std::string_view name : start_stop_symbols)
    add(get_symbol(ctx, name));
}

// Some pieces of code or data such as C++ inline functions or
// instantiated templates can be defined by multiple input files.
// We want to deduplicate them so that only one copy of them is
// included into the output file.
//
// In Mach-O, such code or data are simply defined as weak symbols to
// allow multiple definitions. In this function, we eliminate
// subsections that are referenced by weak symbols that are overridden
// by other files' instances.
template <typename E>
static void remove_unreferenced_subsections(Context<E> &ctx) {
  Timer t(ctx, "remove_unreferenced_subsections");

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    if (file == ctx.internal_obj)
      return;

    if (u32 flags = ((MachHeader *)file->mf->data)->flags;
        !(flags & MH_SUBSECTIONS_VIA_SYMBOLS))
      return;

    for (i64 i = 0; i < file->mach_syms.size(); i++) {
      MachSym<E> &msym = file->mach_syms[i];
      Symbol<E> &sym = *file->syms[i];
      if (sym.file != file && (msym.type == N_SECT) &&
          (msym.desc & N_WEAK_DEF) && !(msym.desc & N_ALT_ENTRY)) {
        // Some weak definitions live in always-split sections such as __cstring
        // and therefore don't have a direct sym_to_subsec entry.
        Subsection<E> *subsec = file->sym_to_subsec[i];
        if (!subsec)
          subsec = file->find_subsection(ctx, msym.value);
        if (subsec)
          subsec->is_alive = false;
      }
    }

    std::erase_if(file->subsections, [](Subsection<E> *subsec) {
      return !subsec->is_alive;
    });
  });
}

template <typename E>
static bool compare_segments(const std::unique_ptr<OutputSegment<E>> &a,
                             const std::unique_ptr<OutputSegment<E>> &b) {
  // We want to sort output segments in the following order:
  // __TEXT, __DATA_CONST, __DATA, <other segments>, __LINKEDIT
  auto get_rank = [](std::string_view name) {
    if (name == "__TEXT")
      return 0;
    if (name == "__DATA_CONST")
      return 1;
    if (name == "__DATA")
      return 2;
    if (name == "__LINKEDIT")
      return 4;
    return 3;
  };

  std::string_view x = a->cmd.get_segname();
  std::string_view y = b->cmd.get_segname();
  return std::tuple{get_rank(x), x} < std::tuple{get_rank(y), y};
}

template <typename E>
static bool compare_chunks(const Chunk<E> *a, const Chunk<E> *b) {
  assert(a->hdr.get_segname() == b->hdr.get_segname());

  auto is_bss = [](const Chunk<E> *x) {
    return x->hdr.type == S_ZEROFILL || x->hdr.type == S_THREAD_LOCAL_ZEROFILL;
  };

  if (is_bss(a) != is_bss(b))
    return !is_bss(a);

  static const std::string_view rank[] = {
    // __TEXT
    "__mach_header",
    "__stubs",
    "__text",
    "__stub_helper",
    "__gcc_except_tab",
    "__cstring",
    "__eh_frame",
    "__unwind_info",
    // __DATA_CONST
    "__got",
    "__const",
    // __DATA
    "__mod_init_func",
    "__la_symbol_ptr",
    "__thread_ptrs",
    "__data",
    "__objc_imageinfo",
    "__thread_vars",
    "__thread_ptr",
    "__thread_data",
    "__thread_bss",
    "__common",
    "__bss",
    // __LINKEDIT
    "__rebase",
    "__binding",
    "__weak_binding",
    "__lazy_binding",
    "__chainfixups",
    "__export",
    "__func_starts",
    "__data_in_code",
    "__symbol_table",
    "__ind_sym_tab",
    "__string_table",
    "__code_signature",
  };

  auto get_rank = [](std::string_view name) {
    i64 i = 0;
    for (; i < sizeof(rank) / sizeof(rank[0]); i++)
      if (name == rank[i])
        return i;
    return i;
  };

  std::string_view x = a->hdr.get_sectname();
  std::string_view y = b->hdr.get_sectname();
  return std::tuple{get_rank(x), x} < std::tuple{get_rank(y), y};
}

template <typename E>
static Chunk<E> *find_section(Context<E> &ctx, std::string_view segname,
                              std::string_view sectname) {
  for (Chunk<E> *chunk : ctx.chunks)
    if (chunk->hdr.match(segname, sectname))
      return chunk;
  return nullptr;
}

template <typename E>
static void claim_unresolved_symbols(Context<E> &ctx) {
  Timer t(ctx, "claim_unresolved_symbols");

  // Handle -U
  for (std::string_view name : ctx.arg.U) {
    Symbol<E> *sym = get_symbol(ctx, name);
    if (!sym->file) {
      sym->file = ctx.internal_obj;
      sym->visibility = SCOPE_GLOBAL;
      sym->is_imported = true;
      sym->is_common = false;
      sym->is_weak = true;
      sym->is_tlv = false;
      sym->subsec = nullptr;
      sym->value = 0;
      ctx.internal_obj->syms.push_back(sym);
    }
  }

  // Find all undefined symbols
  std::vector<std::vector<Symbol<E> *>> msgsend_syms(ctx.objs.size());
  std::vector<std::vector<Symbol<E> *>> other_syms(ctx.objs.size());

  auto is_null = [](InputFile<E> **ptr) {
    return __atomic_load_n(ptr, __ATOMIC_RELAXED) == nullptr;
  };

  auto compare_exchange = [](InputFile<E> **ptr, InputFile<E> *desired) {
    InputFile<E> *expected = nullptr;
    return __atomic_compare_exchange_n(ptr, &expected, desired, false,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  };

  tbb::parallel_for((i64)0, (i64)ctx.objs.size(), [&](i64 i) {
    for (Symbol<E> *sym : ctx.objs[i]->syms) {
      InputFile<E> *expected = nullptr;
      if (is_null(&sym->file) &&
          (sym->name.starts_with("_objc_msgSend$") || !ctx.arg.undefined_error) &&
          compare_exchange(&sym->file, ctx.internal_obj)) {
        if (sym->name.starts_with("_objc_msgSend$")) {
          msgsend_syms[i].push_back(sym);
        } else {
          other_syms[i].push_back(sym);
          sym->is_imported = true;
        }
      }
    }
  });

  auto less = [](Symbol<E> *a, Symbol<E> *b) { return a->name < b->name; };

  // _objc_msgSend is the underlying function of the Objective-C's dynamic
  // message dispatching. The function takes an receiver object and an
  // interned method name string as the first and the second arguments,
  // respectively, along with the arguments for the method.
  //
  // Since Xcode 14, Apple's clang no longer directly generate machine
  // code to call _objc_msgSend but instead generate machine code call to
  // _objc_msgSend$foo where foo is the receiver's class name. It is now
  // the linker's responsibility to detect a call to such symbol and
  // generate stub machine code to call _objc_msgSend with appropriate
  // arguments.
  //
  // The stub code is created in the `__objc_stubs` section.
  //
  // Apple did it as a size optimization. Since the code stub is now
  // shared among functions that call the same class with the same
  // message, it can reduce the output file size.
  std::vector<Symbol<E> *> msgsend_syms2 = flatten(msgsend_syms);
  tbb::parallel_sort(msgsend_syms2.begin(), msgsend_syms2.end(), less);

  if (!msgsend_syms2.empty()) {
    ctx.objc_stubs.reset(new ObjcStubsSection<E>(ctx));
    ctx.internal_obj->rels_pool.reserve(msgsend_syms2.size());
    for (Symbol<E> *sym : msgsend_syms2)
      ctx.internal_obj->add_msgsend_symbol(ctx, *sym);
  }

  // Promote remaining undefined symbols to dynamic symbols if
  // -undefined [ warning | suppress | dynamic_lookup ] was given.
  std::vector<Symbol<E> *> other_syms2 = flatten(other_syms);
  tbb::parallel_sort(other_syms2.begin(), other_syms2.end(), less);
  append(ctx.internal_obj->syms, other_syms2);
}

template <typename E>
static void finish_parsing_archive_members(Context<E> &ctx) {
  Timer t(ctx, "finish_parsing_archive_members");
  static Counter counter("num_archive_members_finish_parsed");

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    if (file->needs_finish_parse()) {
      counter++;
      file->finish_parse(ctx);
    }
  });
}

template <typename E>
static void create_synthetic_chunks(Context<E> &ctx) {
  Timer t(ctx, "create_synthetic_chunks");

  // Create symbol import tables.
  if (ctx.arg.fixup_chains) {
    ctx.chained_fixups.reset(new ChainedFixupsSection<E>(ctx));
  } else {
    ctx.rebase.reset(new RebaseSection<E>(ctx));
    ctx.bind.reset(new BindSection<E>(ctx));
    ctx.stub_helper.reset(new StubHelperSection<E>(ctx));
    ctx.lazy_symbol_ptr.reset(new LazySymbolPtrSection<E>(ctx));
    ctx.lazy_bind.reset(new LazyBindSection<E>(ctx));
  }

  // Create a __DATA,__objc_imageinfo section.
  ctx.image_info = ObjcImageInfoSection<E>::create(ctx);

  // Create a __LINKEDIT,__func_starts section.
  if (ctx.arg.function_starts)
    ctx.function_starts.reset(new FunctionStartsSection(ctx));

  // Create a __LINKEDIT,__data_in_code section.
  if (ctx.arg.data_in_code_info)
    ctx.data_in_code.reset(new DataInCodeSection(ctx));

  // Create a __TEXT,__init_offsets section.
  if (ctx.arg.init_offsets)
    ctx.init_offsets.reset(new InitOffsetsSection(ctx));

  // Handle -sectcreate
  for (SectCreateOption arg : ctx.arg.sectcreate) {
    MappedFile<Context<E>> *mf =
      MappedFile<Context<E>>::must_open(ctx, std::string(arg.filename));
    new SectCreateSection<E>(ctx, arg.segname, arg.sectname, mf->get_contents());
  }

  // We add subsections specified by -order_file to output sections.
  for (std::string_view name : ctx.arg.order_file)
    if (Symbol<E> *sym = get_symbol(ctx, name); sym->file)
      if (Subsection<E> *subsec = sym->subsec)
        if (!subsec->added_to_osec)
          subsec->isec->osec.add_subsec(subsec);

  // Add remaining subsections to output sections.
  for (ObjectFile<E> *file : ctx.objs)
    for (Subsection<E> *subsec : file->subsections)
      if (!subsec->added_to_osec)
        subsec->isec->osec.add_subsec(subsec);

  // Add output sections to segments.
  for (Chunk<E> *chunk : ctx.chunks) {
    if (chunk != ctx.data)
      if (OutputSection<E> *osec = chunk->to_osec())
        if (osec->members.empty())
          continue;

    chunk->seg->chunks.push_back(chunk);
  }

  // Handle -add_empty_section
  for (AddEmptySectionOption &opt : ctx.arg.add_empty_section) {
    if (!find_section(ctx, opt.segname, opt.sectname)) {
      OutputSegment<E> *seg = OutputSegment<E>::get_instance(ctx, opt.segname);
      Chunk<E> *sec = new SectCreateSection<E>(ctx, opt.segname, opt.sectname, {});
      seg->chunks.push_back(sec);
    }
  }

  // Even though redundant, section headers have its containing segments name
  // in Mach-O.
  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
    for (Chunk<E> *chunk : seg->chunks)
      if (!chunk->is_hidden)
        chunk->hdr.set_segname(seg->cmd.segname);

  // Sort segments and output sections.
  sort(ctx.segments, compare_segments<E>);

  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
    sort(seg->chunks, compare_chunks<E>);
}

// Merge S_CSTRING_LITERALS or S_{4,8,16}BYTE_LITERALS subsections by contents.
template <typename E>
static void uniquify_literals(Context<E> &ctx, OutputSection<E> &osec) {
  Timer t(ctx, "uniquify_literals " + std::string(osec.hdr.get_sectname()));

  struct Entry {
    Entry(Subsection<E> *subsec) : owner(subsec) {}
    Entry(const Entry &other) = default;

    Atomic<Subsection<E> *> owner = nullptr;
    Atomic<u8> p2align = 0;
  };

  struct SubsecRef {
    Subsection<E> *subsec = nullptr;
    u64 hash = 0;
    Entry *ent = nullptr;
  };

  std::vector<SubsecRef> vec(osec.members.size());

  // Estimate the number of unique strings.
  tbb::enumerable_thread_specific<HyperLogLog> estimators;

  tbb::parallel_for((i64)0, (i64)osec.members.size(), [&](i64 i) {
    Subsection<E> *subsec = osec.members[i];
    u64 h = hash_string(subsec->get_contents());
    vec[i].subsec = subsec;
    vec[i].hash = h;
    estimators.local().insert(h);
  });

  HyperLogLog estimator;
  for (HyperLogLog &e : estimators)
    estimator.merge(e);

  // Create a hash map large enough to hold all strings.
  ConcurrentMap<Entry> map(estimator.get_cardinality() * 3 / 2);

  // Insert all strings into the hash table.
  tbb::parallel_for_each(vec, [&](SubsecRef &ref) {
    if (!ref.subsec)
      return;

    std::string_view key = ref.subsec->get_contents();
    ref.ent = map.insert(key, ref.hash, {ref.subsec}).first;

    Subsection<E> *existing = ref.ent->owner;
    while (existing->isec->file.priority < ref.subsec->isec->file.priority &&
           !ref.ent->owner.compare_exchange_weak(existing, ref.subsec,
                                                 std::memory_order_relaxed));

    update_maximum(ref.ent->p2align, ref.subsec->p2align.load());
  });

  // Decide who will become the owner for each subsection.
  tbb::parallel_for_each(vec, [&](SubsecRef &ref) {
    if (ref.subsec && ref.subsec != ref.ent->owner) {
      ref.subsec->replacer = ref.ent->owner;
      ref.subsec->is_replaced = true;
    }
  });

  static Counter counter("num_merged_strings");
  counter += parallel_erase_if(osec.members, [](Subsection<E> *subsec) {
    return subsec->is_replaced;
  });
}

// Merge S_LITERAL_POINTERS subsections such as __DATA,__objc_selrefs
// by contents.
//
// Each S_LITERAL_POINTERS subsection is 8 bytes long and contains a
// single relocation record. We merge two subsections if they contain
// the same relcoation.
template <typename E>
static void uniquify_literal_pointers(Context<E> &ctx, OutputSection<E> &osec) {
  Timer t(ctx, "uniquify_literal_pointers");

  struct Entry {
    Entry(i64 idx) : owner_idx(idx) {}
    Entry(const Entry &other) = default;

    Atomic<i64> owner_idx = -1;
  };

  struct SubsecRef {
    i64 idx = -1;
    Subsection<E> *subsec = nullptr;
    Subsection<E> *target = nullptr;
    u64 hash = 0;
    Entry *ent = nullptr;
  };

  auto get_target = [](Relocation<E> &r) -> Subsection<E> * {
    Subsection<E> *subsec = r.sym() ? r.sym()->subsec : r.subsec();
    if (!subsec)
      return nullptr;
    return subsec->is_replaced ? subsec->replacer : subsec;
  };

  std::vector<SubsecRef> refs(osec.members.size());
  tbb::parallel_for((i64)0, (i64)osec.members.size(), [&](i64 i) {
    Subsection<E> *subsec = osec.members[i];
    assert(subsec->input_size == sizeof(Word<E>));

    refs[i].idx = i;
    refs[i].subsec = subsec;

    std::span<Relocation<E>> rels = subsec->get_rels();
    if (rels.size() != 1)
      return;

    refs[i].target = get_target(rels[0]);
    if (!refs[i].target)
      return;

    std::string_view key((char *)&refs[i].target, sizeof(refs[i].target));
    refs[i].hash = hash_string(key);
  });

  ConcurrentMap<Entry> map(osec.members.size() * 3 / 2);

  tbb::parallel_for_each(refs, [&](SubsecRef &ref) {
    if (!ref.target)
      return;

    std::string_view key((char *)&ref.target, sizeof(ref.target));
    ref.ent = map.insert(key, ref.hash, {ref.idx}).first;
    update_minimum(ref.ent->owner_idx, ref.idx);
  });

  tbb::parallel_for_each(refs, [&](SubsecRef &ref) {
    if (!ref.ent)
      return;

    Subsection<E> *owner = refs[ref.ent->owner_idx].subsec;
    if (ref.subsec != owner) {
      ref.subsec->replacer = owner;
      ref.subsec->is_replaced = true;
    }
  });

  static Counter counter("num_merged_literal_pointers");
  counter += parallel_erase_if(osec.members, [](Subsection<E> *subsec) {
    return subsec->is_replaced;
  });
}

template <typename E>
static void merge_mergeable_sections(Context<E> &ctx) {
  Timer t(ctx, "merge_mergeable_sections");

  for (Chunk<E> *chunk : ctx.chunks) {
    if (OutputSection<E> *osec = chunk->to_osec()) {
      switch (chunk->hdr.type) {
      case S_CSTRING_LITERALS:
      case S_4BYTE_LITERALS:
      case S_8BYTE_LITERALS:
      case S_16BYTE_LITERALS:
        uniquify_literals(ctx, *osec);
        break;
      }
    }
  }

  for (Chunk<E> *chunk : ctx.chunks)
    if (OutputSection<E> *osec = chunk->to_osec())
      if (chunk->hdr.type == S_LITERAL_POINTERS)
        uniquify_literal_pointers(ctx, *osec);

  // Rewrite relocations and symbols.
  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (std::unique_ptr<InputSection<E>> &isec : file->sections)
      if (isec)
        for (Relocation<E> &r : isec->rels)
          if (r.subsec() && r.subsec()->is_replaced)
            r.target = r.subsec()->replacer;
  });

  std::vector<InputFile<E> *> files;
  append(files, ctx.objs);
  append(files, ctx.dylibs);

  tbb::parallel_for_each(files, [&](InputFile<E> *file) {
    for (Symbol<E> *sym : file->syms)
      if (sym && sym->file == file && sym->subsec && sym->subsec->is_replaced)
        sym->subsec = sym->subsec->replacer;
  });

  // Remove deduplicated subsections from each file's subsection vector.
  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    std::erase_if(file->subsections, [](Subsection<E> *subsec) {
      return subsec->is_replaced;
    });
  });
}

template <typename E>
static void scan_relocations(Context<E> &ctx) {
  Timer t(ctx, "scan_relocations");

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (Subsection<E> *subsec : file->subsections) {
      subsec->scan_relocations(ctx);
      for (UnwindRecord<E> &rec : subsec->get_unwind_records())
        if (rec.personality)
          rec.personality->flags |= NEEDS_GOT;
    }

    for (std::unique_ptr<CieRecord<E>> &cie : file->cies)
      if (cie->personality)
        cie->personality->flags |= NEEDS_GOT;
  });

  std::vector<InputFile<E> *> files;
  append(files, ctx.objs);
  append(files, ctx.dylibs);

  std::vector<std::vector<Symbol<E> *>> vec(files.size());

  tbb::parallel_for((i64)0, (i64)files.size(), [&](i64 i) {
    for (Symbol<E> *sym : files[i]->syms)
      if (sym && sym->file == files[i] && sym->flags)
        vec[i].push_back(sym);
  });

  for (std::span<Symbol<E> *> syms : vec) {
    for (Symbol<E> *sym : syms) {
      if (sym->flags & NEEDS_GOT)
        ctx.got.add(ctx, sym);

      if (sym->flags & NEEDS_STUB) {
        if (ctx.arg.bind_at_load || ctx.arg.fixup_chains)
          ctx.got.add(ctx, sym);
        ctx.stubs.add(ctx, sym);
      }

      if (sym->flags & NEEDS_THREAD_PTR)
        ctx.thread_ptrs.add(ctx, sym);

      sym->flags = 0;
    }
  }
}

template <typename E>
static i64 assign_offsets(Context<E> &ctx) {
  Timer t(ctx, "assign_offsets");

  i64 sect_idx = 1;
  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
    for (Chunk<E> *chunk : seg->chunks)
      if (!chunk->is_hidden)
        chunk->sect_idx = sect_idx++;

  i64 fileoff = 0;
  i64 vmaddr = ctx.arg.pagezero_size;

  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments) {
    seg->set_offset(ctx, fileoff, vmaddr);
    fileoff += seg->cmd.filesize;
    vmaddr += seg->cmd.vmsize;
  }
  return fileoff;
}

// An address of a symbol of type S_THREAD_LOCAL_VARIABLES is computed
// as a relative address to the beginning of the first thread-local
// section. This function finds the beginning address.
template <typename E>
static u64 get_tls_begin(Context<E> &ctx) {
  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
    for (Chunk<E> *chunk : seg->chunks)
      if (chunk->hdr.type == S_THREAD_LOCAL_REGULAR ||
          chunk->hdr.type == S_THREAD_LOCAL_ZEROFILL)
        return chunk->hdr.addr;
  return 0;
}

template <typename E>
static void fix_synthetic_symbol_values(Context<E> &ctx) {
  ctx.__dyld_private->value = ctx.data->hdr.addr;
  ctx.__mh_dylib_header->value = ctx.data->hdr.addr;
  ctx.__mh_bundle_header->value = ctx.data->hdr.addr;
  ctx.___dso_handle->value = ctx.data->hdr.addr;

  auto find_segment = [&](std::string_view name) -> SegmentCommand<E> * {
    for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
      if (seg->cmd.get_segname() == name)
        return &seg->cmd;
    return nullptr;
  };

  auto find_section = [&](std::string_view name) -> MachSection<E> * {
    size_t pos = name.find('$');
    if (pos == name.npos)
      return nullptr;

    std::string_view segname = name.substr(0, pos);
    std::string_view sectname = name.substr(pos + 1);

    for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
      for (Chunk<E> *chunk : seg->chunks)
        if (chunk->hdr.match(segname, sectname))
          return &chunk->hdr;
    return nullptr;
  };

  i64 objc_stubs_offset = 0;

  for (Symbol<E> *sym : ctx.internal_obj->syms) {
    std::string_view name = sym->name;

    if (remove_prefix(name, "segment$start$")) {
      sym->value = ctx.text->hdr.addr;
      if (SegmentCommand<E> *cmd = find_segment(name))
        sym->value = cmd->vmaddr;
      continue;
    }

    if (remove_prefix(name, "segment$end$")) {
      sym->value = ctx.text->hdr.addr;
      if (SegmentCommand<E> *cmd = find_segment(name))
        sym->value = cmd->vmaddr + cmd->vmsize;
      continue;
    }

    if (remove_prefix(name, "section$start$")) {
      sym->value = ctx.text->hdr.addr;
      if (MachSection<E> *hdr = find_section(name))
        sym->value = hdr->addr;
      continue;
    }

    if (remove_prefix(name, "section$end$")) {
      sym->value = ctx.text->hdr.addr;
      if (MachSection<E> *hdr = find_section(name))
        sym->value = hdr->addr + hdr->size;
      continue;
    }

    if (name.starts_with("_objc_msgSend$")) {
      sym->value = ctx.objc_stubs->hdr.addr + objc_stubs_offset;
      objc_stubs_offset += ObjcStubsSection<E>::ENTRY_SIZE;
      continue;
    }
  }
}

template <typename E>
static void fill_text_segment_gaps(Context<E> &ctx, OutputSegment<E> &seg) {
  if constexpr (!is_x86<E>)
    return;

  if (seg.cmd.get_segname() != "__TEXT")
    return;

  i64 seg_begin = seg.cmd.fileoff;
  i64 seg_end = seg.cmd.fileoff + seg.cmd.filesize;

  tbb::parallel_for((i64)0, (i64)seg.chunks.size() + 1, [&](i64 i) {
    i64 begin = seg_begin;
    if (i > 0)
      begin = seg.chunks[i - 1]->hdr.offset + seg.chunks[i - 1]->hdr.size;

    i64 end = seg_end;
    if (i < seg.chunks.size())
      end = seg.chunks[i]->hdr.offset;

    if (begin < end)
      memset(ctx.buf + begin, 0x90, end - begin);
  });
}

template <typename E>
static void copy_sections_to_output_file(Context<E> &ctx) {
  Timer t(ctx, "copy_sections_to_output_file");

  tbb::parallel_for_each(ctx.segments,
                         [&](std::unique_ptr<OutputSegment<E>> &seg) {
    Timer t2(ctx, std::string(seg->cmd.get_segname()), &t);

    // Keep x86 text padding disassembler-friendly without rewriting the
    // entire segment up front.
    fill_text_segment_gaps(ctx, *seg);

    tbb::parallel_for_each(seg->chunks, [&](Chunk<E> *sec) {
      if (sec->hdr.type != S_ZEROFILL) {
        Timer t3(ctx, std::string(sec->hdr.get_sectname()), &t2);
        sec->copy_buf(ctx);
      }
    });
  });
}

template <typename E>
static void compute_uuid(Context<E> &ctx) {
  Timer t(ctx, "compute_uuid");

  // Compute a markle tree of height two.
  i64 filesize = ctx.output_file->filesize;
  i64 shard_size = 4096 * 1024;
  i64 num_shards = align_to(filesize, shard_size) / shard_size;
  std::vector<u8> shards(num_shards * SHA256_SIZE);

  tbb::parallel_for((i64)0, num_shards, [&](i64 i) {
    u8 *begin = ctx.buf + shard_size * i;
    u8 *end = (i == num_shards - 1) ? ctx.buf + filesize : begin + shard_size;
    sha256_hash(begin, end - begin, shards.data() + i * SHA256_SIZE);
  });

  u8 buf[SHA256_SIZE];
  sha256_hash(shards.data(), shards.size(), buf);
  memcpy(ctx.uuid, buf, 16);
  ctx.mach_hdr.copy_buf(ctx);
}

template <typename E>
static MappedFile<Context<E>> *
strip_universal_header(Context<E> &ctx, MappedFile<Context<E>> *mf) {
  FatHeader &hdr = *(FatHeader *)mf->data;
  assert(hdr.magic == FAT_MAGIC);

  FatArch *arch = (FatArch *)(mf->data + sizeof(hdr));
  for (i64 i = 0; i < hdr.nfat_arch; i++)
    if (arch[i].cputype == E::cputype)
      return mf->slice(ctx, mf->name, arch[i].offset, arch[i].size);
  Fatal(ctx) << mf->name << ": fat file contains no matching file";
}

template <typename E>
static void read_file(Context<E> &ctx, MappedFile<Context<E>> *mf);

template <typename E>
static void collect_hoisted_dylibs(Context<E> &ctx) {
  std::unordered_set<std::string_view> seen;
  for (DylibFile<E> *file : ctx.dylibs)
    if (!file->install_name.empty())
      seen.insert(file->install_name);

  for (i64 i = 0; i < ctx.dylibs.size(); i++)
    for (DylibFile<E> *file : ctx.dylibs[i]->hoisted_libs)
      if (seen.insert(file->install_name).second)
        ctx.dylibs.push_back(file);
}

template <typename E>
static void resolve_new_files(Context<E> &ctx, i64 obj_begin, i64 dylib_begin) {
  ctx.tg.wait();
  collect_hoisted_dylibs(ctx);

  for (i64 i = dylib_begin; i < ctx.dylibs.size(); i++)
    ctx.dylibs[i]->resolve_symbols(ctx);

  for (i64 i = obj_begin; i < ctx.objs.size(); i++)
    ctx.objs[i]->resolve_symbols(ctx);
}

template <typename E>
static MappedFile<Context<E>> *
search_library(Context<E> &ctx, std::string_view name,
               std::initializer_list<std::string_view> suffixes) {
  Timer t(ctx, "search_library");

  for (const std::string &dir : ctx.arg.library_paths) {
    std::string path = dir + "/lib";
    path += name;

    i64 base = path.size();
    for (std::string_view suffix : suffixes) {
      path.resize(base);
      path += suffix;
      if (MappedFile<Context<E>> *mf = open_cached_input(ctx, path, true))
        return mf;
    }
  }
  return nullptr;
}

template <typename E>
static MappedFile<Context<E>> *find_library(Context<E> &ctx, std::string name) {
  if (ctx.arg.search_paths_first)
    return search_library(ctx, name, {".tbd", ".dylib", ".a"});

  if (MappedFile<Context<E>> *mf =
        search_library(ctx, name, {".tbd", ".dylib"}))
    return mf;
  return search_library(ctx, name, {".a"});
}

template <typename E>
static MappedFile<Context<E>> *find_framework(Context<E> &ctx, std::string name) {
  Timer t(ctx, "find_framework");
  std::string suffix;
  std::tie(name, suffix) = split_string(name, ',');

  for (const std::string &root : ctx.arg.framework_paths) {
    std::string path =
      resolve_framework_realpath(ctx, root + "/" + name + ".framework/" + name);

    if (!suffix.empty())
      if (MappedFile<Context<E>> *mf = open_cached_input(ctx, path + suffix))
        return mf;

    if (MappedFile<Context<E>> *mf = open_cached_input(ctx, path + ".tbd"))
      return mf;

    if (MappedFile<Context<E>> *mf = open_cached_input(ctx, path))
      return mf;
  }
  return nullptr;
}

template <typename E>
static bool read_library(Context<E> &ctx, std::string name, bool required = true) {
  if (ctx.seen_libraries.contains(name)) {
    if (ctx.reader.needed)
      if (auto it = ctx.lib_dylibs.find(name); it != ctx.lib_dylibs.end())
        it->second->is_alive = true;
    return true;
  }

  MappedFile<Context<E>> *mf = find_library(ctx, name);
  if (!mf) {
    if (required)
      Fatal(ctx) << "library not found: -l" << name;
    return false;
  }

  ctx.seen_libraries.insert(name);
  i64 num_dylibs = ctx.dylibs.size();
  read_file(ctx, mf);

  if (ctx.dylibs.size() != num_dylibs)
    ctx.lib_dylibs.emplace(name, ctx.dylibs.back());
  return true;
}

template <typename E>
static bool read_framework(Context<E> &ctx, std::string name,
                           bool required = true) {
  if (ctx.seen_frameworks.contains(name)) {
    if (ctx.reader.needed)
      if (auto it = ctx.framework_dylibs.find(name);
          it != ctx.framework_dylibs.end())
        it->second->is_alive = true;
    return true;
  }

  MappedFile<Context<E>> *mf = find_framework(ctx, name);
  if (!mf) {
    if (required)
      Fatal(ctx) << "-framework not found: " << name;
    return false;
  }

  ctx.seen_frameworks.insert(name);
  i64 num_dylibs = ctx.dylibs.size();
  read_file(ctx, mf);

  if (ctx.dylibs.size() != num_dylibs)
    ctx.framework_dylibs.emplace(name, ctx.dylibs.back());
  return true;
}

template <typename E>
static void read_bundle(Context<E> &ctx, std::string name) {
  MappedFile<Context<E>> *mf = open_cached_input(ctx, name);
  if (!mf)
    Fatal(ctx) << "-bundle_loader: cannot open " << name;
  read_file(ctx, mf);
}

template <typename E>
static void process_linker_options(Context<E> &ctx, ObjectFile<E> &file) {
  std::vector<std::string> opts = file.get_linker_options(ctx);
  ReaderContext orig = ctx.reader;

  auto warn_bad_linker_option = [&](std::string_view opt) {
    Warn(ctx) << file << ": ignoring unsupported LC_LINKER_OPTION command: " << opt;
  };

  auto warn_malformed_linker_option = [&](std::string_view opt) {
    Warn(ctx) << file << ": ignoring malformed LC_LINKER_OPTION command: " << opt;
  };

  for (i64 j = 0; j < opts.size();) {
    ctx.reader = orig;
    ctx.reader.implicit = true;

    std::string_view opt = opts[j];

    if (opt == "-framework" || opt == "-needed_framework" ||
        opt == "-needed-framework") {
      if (j + 1 >= opts.size()) {
        warn_malformed_linker_option(opt);
        j++;
        continue;
      }

      if (opt != "-framework")
        ctx.reader.needed = true;

      read_framework(ctx, opts[j + 1], false);
      j += 2;
      continue;
    }

    if (opt == "-needed-l" || opt == "-l") {
      if (j + 1 >= opts.size()) {
        warn_malformed_linker_option(opt);
        j++;
        continue;
      }

      if (opt == "-needed-l")
        ctx.reader.needed = true;

      read_library(ctx, opts[j + 1], false);
      j += 2;
      continue;
    }

    if (opt.starts_with("-needed-l")) {
      if (opt.size() == std::string_view("-needed-l").size()) {
        warn_malformed_linker_option(opt);
        j++;
        continue;
      }

      ctx.reader.needed = true;
      read_library(ctx, std::string(opt.substr(std::string_view("-needed-l").size())),
                   false);
      j++;
      continue;
    }

    if (opt.starts_with("-l")) {
      if (opt.size() == std::string_view("-l").size()) {
        warn_malformed_linker_option(opt);
        j++;
        continue;
      }

      read_library(ctx, std::string(opt.substr(2)), false);
      j++;
      continue;
    }

    warn_bad_linker_option(opt);
    j++;
  }

  ctx.reader = orig;
}

template <typename E>
static ObjectFile<E> *load_archive_member(Context<E> &ctx,
                                          ArchiveMember<E> &member) {
  if (member.is_loaded)
    return member.file;

  static Counter counter("num_archive_members_extracted");
  member.is_loaded = true;

  MappedFile<Context<E>> *mf;
  if (member.is_thin) {
    mf = MappedFile<Context<E>>::must_open(ctx, member.path);
    mf->thin_parent = member.archive->mf;
  } else {
    mf = member.archive->mf->slice(ctx, member.name, member.data_offset,
                                   member.data_size);
  }

  if (get_file_type(ctx, mf) == FileType::MACH_UNIVERSAL)
    mf = strip_universal_header(ctx, mf);

  FileType type = get_file_type(ctx, mf);
  if (type != FileType::MACH_OBJ && type != FileType::LLVM_BITCODE)
    return nullptr;

  ReaderContext saved = ctx.reader;
  ctx.reader = member.archive->reader;
  ObjectFile<E> *file = ObjectFile<E>::create(ctx, mf, member.archive->mf->name);
  ctx.reader = saved;

  counter++;
  file->priority = member.priority;
  member.file = file;
  ctx.objs.push_back(file);
  file->parse(ctx);
  return file;
}

template <typename E>
static void record_archive_fallback(Context<E> &ctx, MappedFile<Context<E>> *mf,
                                    ArchiveIndexStatus status) {
  static Counter counter("num_archives_eager_fallback");
  static Counter no_symtab("num_archives_eager_fallback_no_darwin_symtab");
  static Counter empty_symtab("num_archives_eager_fallback_empty_darwin_symtab");

  counter++;

  switch (status) {
  case ArchiveIndexStatus::OK:
    unreachable();
  case ArchiveIndexStatus::NO_DARWIN_SYMTAB:
    no_symtab++;
    break;
  case ArchiveIndexStatus::EMPTY_DARWIN_SYMTAB:
    empty_symtab++;
    break;
  }

  ctx.archive_fallbacks.push_back(std::string(archive_index_status_name(status)) +
                                  "\t" + mf->name);
}

template <typename E>
static ArchiveIndexStatus add_archive(Context<E> &ctx, MappedFile<Context<E>> *mf) {
  Timer t(ctx, "index_archive");
  ArchiveReadResult<Context<E>, MappedFile<Context<E>>> result =
    read_archive_file<Context<E>, MappedFile<Context<E>>>(ctx, mf);
  if (!result.info)
    return result.status;
  ArchiveFileInfo<Context<E>, MappedFile<Context<E>>> &info = *result.info;

  static Counter archives("num_archives_indexed");
  static Counter empty_symtab("num_archives_indexed_empty_darwin_symtab");
  static Counter members("num_archive_members_indexed");
  static Counter symbols("num_archive_symbols_indexed");
  archives++;
  if (result.status == ArchiveIndexStatus::EMPTY_DARWIN_SYMTAB) {
    empty_symtab++;
    // A Darwin archive with an empty TOC can't satisfy symbol-driven extraction
    // in the lazy path, so avoid building per-member bookkeeping for it.
    return ArchiveIndexStatus::OK;
  }
  members += (int)info.members.size();
  symbols += (int)info.symbols.size();

  std::unique_ptr<ArchiveFile<E>> archive = std::make_unique<ArchiveFile<E>>();
  archive->mf = mf;
  archive->reader = ctx.reader;
  archive->members.reserve(info.members.size());

  for (auto &src : info.members) {
    ArchiveMember<E> &dst = archive->members.emplace_back();
    dst.archive = archive.get();
    dst.name = std::move(src.name);
    dst.path = std::move(src.path);
    dst.data_offset = src.data_offset;
    dst.data_size = src.data_size;
    dst.priority = ctx.next_object_priority++;
    dst.is_thin = src.is_thin;
  }

  ArchiveFile<E> *archive_ptr = archive.get();
  for (ArchiveSymbolInfo ent : info.symbols) {
    ASSERT(ent.member_idx >= 0);
    ASSERT((size_t)ent.member_idx < archive->members.size());
    ctx.archive_symbol_map[ent.name].push_back(&archive->members[ent.member_idx]);
  }

  ctx.archive_pool.emplace_back(std::move(archive));
  ctx.archives.push_back(archive_ptr);
  return ArchiveIndexStatus::OK;
}

template <typename E>
static void read_file(Context<E> &ctx, MappedFile<Context<E>> *mf) {
  if (get_file_type(ctx, mf) == FileType::MACH_UNIVERSAL)
    mf = strip_universal_header(ctx, mf);

  switch (get_file_type(ctx, mf)) {
  case FileType::TAPI:
  case FileType::MACH_DYLIB:
  case FileType::MACH_EXE: {
    DylibFile<E> *file = DylibFile<E>::create(ctx, mf);
    ctx.tg.run([file, &ctx] { file->parse(ctx); });
    ctx.dylibs.push_back(file);
    break;
  }
  case FileType::MACH_OBJ:
  case FileType::LLVM_BITCODE: {
    ObjectFile<E> *file = ObjectFile<E>::create(ctx, mf, "");
    file->priority = ctx.next_object_priority++;
    ctx.tg.run([file, &ctx] { file->parse(ctx); });
    ctx.objs.push_back(file);
    break;
  }
  case FileType::AR:
  case FileType::THIN_AR:
    if (!ctx.arg.ObjC && !ctx.reader.all_load) {
      ArchiveIndexStatus status = add_archive(ctx, mf);
      if (status == ArchiveIndexStatus::OK)
        break;
      record_archive_fallback(ctx, mf, status);
    }

    for (MappedFile<Context<E>> *child : read_archive_members(ctx, mf)) {
      FileType type = get_file_type(ctx, child);
      if (type == FileType::MACH_OBJ || type == FileType::LLVM_BITCODE) {
        ObjectFile<E> *file = ObjectFile<E>::create(ctx, child, mf->name);
        file->priority = ctx.next_object_priority++;
        ctx.tg.run([file, &ctx] { file->parse(ctx); });
        ctx.objs.push_back(file);
      }
    }
    break;
  default:
    Fatal(ctx) << mf->name << ": unknown file type";
    break;
  }
}

template <typename E>
static std::vector<std::string>
read_filelist(Context<E> &ctx, std::string arg) {
  std::string path;
  std::string dir;

  if (size_t pos = arg.find(','); pos != arg.npos) {
    path = arg.substr(0, pos);
    dir = arg.substr(pos + 1) + "/";
  } else {
    path = arg;
  }

  MappedFile<Context<E>> *mf = MappedFile<Context<E>>::open(ctx, path);
  if (!mf)
    Fatal(ctx) << "-filepath: cannot open " << path;

  std::vector<std::string> vec;
  for (std::string_view str = mf->get_contents(); !str.empty();) {
    std::string_view path;
    std::tie(path, str) = split_string(str, '\n');
    vec.push_back(path_clean(dir + std::string(path)));
  }
  return vec;
}

template <typename E>
static void read_input_files(Context<E> &ctx, std::span<std::string> args) {
  Timer t(ctx, "read_input_files");

  while (!args.empty()) {
    const std::string &opt = args[0];
    args = args.subspan(1);

    if (!opt.starts_with('-')) {
      read_file(ctx, MappedFile<Context<E>>::must_open(ctx, opt));
      continue;
    }

    if (opt == "-all_load") {
      ctx.reader.all_load = true;
      continue;
    }

    if (opt == "-noall_load") {
      ctx.reader.all_load = false;
      continue;
    }

    if (args.empty())
      Fatal(ctx) << opt << ": missing argument";

    const std::string &arg = args[0];
    args = args.subspan(1);
    ReaderContext orig = ctx.reader;

    if (opt == "-filelist") {
      for (std::string &path : read_filelist(ctx, arg)) {
        MappedFile<Context<E>> *mf = MappedFile<Context<E>>::open(ctx, path);
        if (!mf)
          Fatal(ctx) << "-filelist " << arg << ": cannot open file: " << path;
        read_file(ctx, mf);
      }
    } else if (opt == "-force_load") {
      ctx.reader.all_load = true;
      read_file(ctx, MappedFile<Context<E>>::must_open(ctx, arg));
    } else if (opt == "-framework") {
      read_framework(ctx, arg);
    } else if (opt == "-needed_framework" || opt == "-needed-framework") {
      ctx.reader.needed = true;
      read_framework(ctx, arg);
    } else if (opt == "-weak_framework") {
      ctx.reader.weak = true;
      read_framework(ctx, arg);
    } else if (opt == "-l") {
      read_library(ctx, arg);
    } else if (opt == "-needed-l") {
      ctx.reader.needed = true;
      read_library(ctx, arg);
    } else if (opt == "-hidden-l") {
      ctx.reader.hidden = true;
      read_library(ctx, arg);
    } else if (opt == "-weak-l") {
      ctx.reader.weak = true;
      read_library(ctx, arg);
    } else if (opt == "-reexport-l") {
      ctx.reader.reexport = true;
      read_library(ctx, arg);
    } else if (opt == "-reexport_library") {
      ctx.reader.reexport = true;
      read_file(ctx, MappedFile<Context<E>>::must_open(ctx, arg));
    } else {
      unreachable();
    }

    ctx.reader = orig;
  }

  // With -bundle_loader, we can import symbols from a main executable.
  if (!ctx.arg.bundle_loader.empty())
    read_bundle(ctx, ctx.arg.bundle_loader);

  // An object file can contain linker directives to load other object
  // files or libraries, so process them if any. We accept the common
  // autolink forms and warn for the rest instead of hard-failing.
  for (i64 i = 0; i < ctx.objs.size(); i++) {
    process_linker_options(ctx, *ctx.objs[i]);
  }

  ctx.tg.wait();
  collect_hoisted_dylibs(ctx);

  if (ctx.objs.empty())
    Fatal(ctx) << "no input files";

  for (i64 i = 1; DylibFile<E> *file : ctx.dylibs)
    if (file->dylib_idx != BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE)
      file->dylib_idx = i++;
}

template <typename E>
void maybe_extract_archive_member(Context<E> &ctx, Symbol<E> &sym) {
  auto should_extract = [&](const Symbol<E> &sym) {
    if (!sym.file)
      return true;
    if (sym.file->is_dylib)
      return true;
    if (sym.is_common)
      return true;
    return !sym.file->is_alive && sym.is_weak;
  };

  auto it = ctx.archive_symbol_map.find(sym.name);
  if (it == ctx.archive_symbol_map.end())
    return;

  while (should_extract(sym)) {
    bool loaded = false;

    for (ArchiveMember<E> *member : it->second) {
      if (member->is_loaded)
        continue;

      i64 obj_begin = ctx.objs.size();
      i64 dylib_begin = ctx.dylibs.size();
      ObjectFile<E> *file = load_archive_member(ctx, *member);
      if (!file)
        continue;

      process_linker_options(ctx, *file);
      resolve_new_files(ctx, obj_begin, dylib_begin);
      loaded = true;
      break;
    }

    if (!loaded)
      break;
  }
}

template <typename E>
void print_dependencies(Context<E> &ctx) {
  SyncOut(ctx) <<
R"(# This is an output of the sold linker's --print-dependencies option.
#
# Each line consists of 4 fields, <file1>, <file2>, <symbol-type> and
# <symbol>, separated by tab characters. It indicates that <file1> depends
# on <file2> to use <symbol>. <symbol-type> is either "u" or "w" for
# regular undefined or weak undefined, respectively.
)";

  for (ObjectFile<E> *file : ctx.objs) {
    for (std::unique_ptr<InputSection<E>> &isec : file->sections) {
      if (!isec)
        continue;

      MachRel *mach_rels = (MachRel *)(file->mf->data + isec->hdr.reloff);
      std::unordered_set<Symbol<E> *> visited;

      for (i64 i = 0; i < isec->hdr.nreloc; i++) {
        MachRel &r = mach_rels[i];
        if (!r.is_extern)
          continue;

        MachSym<E> &msym = file->mach_syms[r.idx];
        Symbol<E> &sym = *file->syms[r.idx];

        if (msym.is_undef() && sym.file && sym.file != file && visited.insert(&sym).second)
          SyncOut(ctx) << *file << '\t' << *sym.file
                       << '\t' << ((msym.desc & N_WEAK_DEF) ? 'w' : 'u')
                       << '\t' << sym;
      }
    }
  }
}

template <typename E>
static void write_dependency_info(Context<E> &ctx) {
  static constexpr u8 LINKER_VERSION = 0;
  static constexpr u8 INPUT_FILE = 0x10;
  static constexpr u8 NOT_FOUND_FILE = 0x11;
  static constexpr u8 OUTPUT_FILE = 0x40;

  std::ofstream out;
  out.open(std::string(ctx.arg.dependency_info).c_str());
  if (!out.is_open())
    Fatal(ctx) << "cannot open " << ctx.arg.dependency_info
               << ": " << errno_string();

  out << LINKER_VERSION << mold_version << '\0';

  std::set<std::string_view> input_files;
  for (std::unique_ptr<MappedFile<Context<E>>> &mf : ctx.mf_pool)
    if (!mf->parent)
      input_files.insert(mf->name);

  for (std::string_view s : input_files)
    out << INPUT_FILE << s << '\0';

  for (std::string_view s : ctx.missing_files)
    out << NOT_FOUND_FILE << s << '\0';

  out << OUTPUT_FILE << ctx.arg.output << '\0';
  out.close();
}

template <typename E>
static void print_stats(Context<E> &ctx) {
  for (ObjectFile<E> *file : ctx.objs) {
    static Counter subsections("num_subsections");
    subsections += file->subsections.size();

    static Counter syms("num_syms");
    syms += file->syms.size();

    static Counter rels("num_rels");
    for (std::unique_ptr<InputSection<E>> &isec : file->sections)
      if (isec)
        rels += isec->rels.size();
  }

  static Counter num_objs("num_objs", ctx.objs.size());
  static Counter num_dylibs("num_dylibs", ctx.dylibs.size());

  Counter::print();

  for (const std::string &ent : ctx.archive_fallbacks)
    std::cout << "archive_fallback=" << ent << "\n";
}

template <typename E>
static int redo_main(int argc, char **argv, i64 arch) {
  switch (arch) {
  case CPU_TYPE_ARM64_32:
    return macho_main<ARM64_32>(argc, argv);
  case CPU_TYPE_X86_64:
    return macho_main<X86_64>(argc, argv);
  }
  unreachable();
}

template <typename E>
int macho_main(int argc, char **argv) {
  Context<E> ctx;

  for (i64 i = 0; i < argc; i++)
    ctx.cmdline_args.push_back(argv[i]);

  std::vector<std::string> file_args = parse_nonpositional_args(ctx);

  if constexpr (is_arm64<E>)
    if (ctx.arg.arch != E::cputype)
      return redo_main<E>(argc, argv, ctx.arg.arch);

  Timer t(ctx, "all");

  tbb::global_control tbb_cont(tbb::global_control::max_allowed_parallelism,
                               ctx.arg.thread_count);

  if (ctx.arg.adhoc_codesign)
    ctx.code_sig.reset(new CodeSignatureSection<E>(ctx));

  read_input_files(ctx, file_args);

  // `-ObjC` is an option to load all members of static archive
  // libraries that implement an Objective-C class or category.
  if (ctx.arg.ObjC)
    for (ObjectFile<E> *file : ctx.objs)
      if (!file->archive_name.empty() && file->is_objc_object(ctx))
        file->is_alive = true;

  resolve_symbols(ctx);

  if (ctx.output_type == MH_EXECUTE && !ctx.arg.entry->file)
    Error(ctx) << "undefined entry point symbol: " << *ctx.arg.entry;

  create_internal_file(ctx);

  // Handle -exported_symbol and -exported_symbols_list
  handle_exported_symbols_list(ctx);

  // Handle -unexported_symbol and -unexported_symbols_list
  handle_unexported_symbols_list(ctx);

  if (ctx.arg.trace) {
    for (ObjectFile<E> *file : ctx.objs)
      SyncOut(ctx) << *file;
    for (DylibFile<E> *file : ctx.dylibs)
      SyncOut(ctx) << *file;
  }

  for (ObjectFile<E> *file : ctx.objs)
    file->convert_common_symbols(ctx);

  claim_unresolved_symbols(ctx);
  finish_parsing_archive_members(ctx);

  if (ctx.arg.dead_strip)
    dead_strip(ctx);
  else
    remove_unreferenced_subsections(ctx);

  if (ctx.arg.print_dependencies)
    print_dependencies(ctx);

  create_synthetic_chunks(ctx);
  merge_mergeable_sections(ctx);

  for (ObjectFile<E> *file : ctx.objs)
    file->check_duplicate_symbols(ctx);

  for (SectAlignOption &opt : ctx.arg.sectalign)
    if (Chunk<E> *chunk = find_section(ctx, opt.segname, opt.sectname))
      chunk->hdr.p2align = opt.p2align;

  bool has_pagezero_seg = ctx.arg.pagezero_size;
  for (i64 i = 0; i < ctx.segments.size(); i++)
    ctx.segments[i]->seg_idx = (has_pagezero_seg ? i + 1 : i);

  compute_import_export(ctx);

  scan_relocations(ctx);

  i64 output_size = assign_offsets(ctx);
  ctx.tls_begin = get_tls_begin(ctx);
  fix_synthetic_symbol_values(ctx);

  ctx.output_file =
    OutputFile<Context<E>>::open(ctx, ctx.arg.output, output_size, 0777);
  ctx.buf = ctx.output_file->buf;

  copy_sections_to_output_file(ctx);

  if (ctx.chained_fixups)
    ctx.chained_fixups->write_fixup_chains(ctx);

  if (ctx.code_sig)
    ctx.code_sig->write_signature(ctx);
  else if (ctx.arg.uuid == UUID_HASH)
    compute_uuid(ctx);

  ctx.output_file->close(ctx);

  if (!ctx.arg.dependency_info.empty())
    write_dependency_info(ctx);

  ctx.checkpoint();
  t.stop();

  if (ctx.arg.perf)
    print_timer_records(ctx.timer_records);

  if (ctx.arg.stats)
    print_stats(ctx);

  if (!ctx.arg.map.empty())
    print_map(ctx);

  if (ctx.arg.quick_exit) {
    std::cout << std::flush;
    std::cerr << std::flush;
    _exit(0);
  }

  return 0;
}

using E = MOLD_TARGET;

#ifdef MOLD_ARM64

extern template int macho_main<ARM64_32>(int, char **);
extern template int macho_main<X86_64>(int, char **);

int main(int argc, char **argv) {
  return macho_main<ARM64>(argc, argv);
}

#else

template int macho_main<E>(int, char **);

#endif
}
