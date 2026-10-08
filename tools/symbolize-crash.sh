#!/usr/bin/env bash
# Symbolize a crashpad minidump produced by shield (docs/crash-reporting.md).
#
# Usage: tools/symbolize-crash.sh <unstripped-binary> <minidump.dmp> [sym-dir]
#
# <unstripped-binary> should be the exact binary that crashed (build with
# RelWithDebInfo for file+line frames; Release still gives function frames
# via the symbol table). [sym-dir] defaults to a fresh temp directory.
#
# Requires dump_syms and minidump-stackwalk on PATH:
#   cargo install --locked dump_syms minidump-stackwalk
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <unstripped-binary> <minidump.dmp> [sym-dir]" >&2
    exit 1
fi

binary=$1
dump=$2
sym_dir=${3:-$(mktemp -d)}
mkdir -p "$sym_dir"

module=$(basename "$binary")
module_sym="$sym_dir/$module.sym"
dump_syms "$binary" > "$module_sym"

# minidump-stackwalk's SimpleSymbolSupplier only serves the breakpad
# symbol-server layout <module>/<debug-id>/<module>.sym — rearrange
# accordingly (id + name come from the MODULE line dump_syms wrote).
read -r _ _ _ debug_id sym_name < "$module_sym"
install -Dm644 "$module_sym" "$sym_dir/$sym_name/$debug_id/$sym_name.sym"

exec minidump-stackwalk "$dump" --symbols-path "$sym_dir"
