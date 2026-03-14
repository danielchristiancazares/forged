import sys

phases = [
  "read_input_files",
  "resolve_symbols",
  "create_internal_file",
  "handle_exported_symbols_list",
  "handle_unexported_symbols_list",
  "dead_strip",
  "remove_unreferenced_subsections",
  "claim_unresolved_symbols",
  "finish_parsing_archive_members",
  "create_synthetic_chunks",
  "uniquify_literals",
  "uniquify_literal_pointers",
  "merge_mergeable_sections",
  "scan_relocations",
  "assign_offsets",
  "copy_sections_to_output_file",
  "compute_uuid",
  "search_library",
  "find_framework",
  "index_archive",
  "all"
]

lines = sys.stdin.read().splitlines()
for line in lines:
    for p in phases:
        if line.endswith(p):
            print(line)
