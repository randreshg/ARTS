#!/usr/bin/env bash
# Fails when runtime source under <root>/libs calls the process C allocator.
#
# Runtime memory comes from the arts_malloc family; an interposed or
# exhausted C allocator must not be reachable from it.  A call is one of the
# names below followed by "(" and not preceded by an identifier character,
# "." or "->", so arts_malloc, mi_free and ops->free do not match.  Comment
# lines are skipped.  Every allowed site must still be present, so a stale
# entry fails too.
set -u
root=${1:?usage: check_no_libc_alloc.sh <source root>}
cd "$root" || exit 2

names='malloc|calloc|realloc|reallocarray|free|strdup|strndup|getline|getdelim|posix_memalign|aligned_alloc|memalign|valloc|asprintf|vasprintf'
call="(^|[^A-Za-z0-9_.>])(${names})[[:space:]]*\\("

# The allocator backend itself.
allow_files=(
  libs/src/core/utils/malloc.c
)

# "<path>|<line with surrounding blanks trimmed>": sites whose memory a
# foreign owner allocates or frees.
allow_lines=(
  # The registered pool's backend without mimalloc.
  'libs/src/core/memory/regpool.c|if (posix_memalign(&p, a, size) != 0) {'
  'libs/src/core/memory/regpool.c|void arts_regpool_free(void *p) { free(p); }'
  # fi_info hints: fi_freeinfo releases them with free.
  'libs/src/core/transport/net.c|hints->fabric_attr->prov_name = strdup(provider);'
  'libs/src/core/transport/net.c|hints->fabric_attr->prov_name = strdup("tcp");'
  'libs/src/core/transport/net.c|hints->domain_attr->name = strdup(fabric_domain);'
  'libs/src/core/transport/net.c|(struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));'
  'libs/src/core/transport/net.c|free(hints->fabric_attr->prov_name);'
  'libs/src/core/transport/net.c|cxi_hints->fabric_attr->prov_name = strdup("cxi");'
)

declare -A allowed=()
for e in "${allow_lines[@]}"; do allowed["$e"]=1; done
declare -A file_ok=()
for f in "${allow_files[@]}"; do file_ok["$f"]=1; done

bad=0
seen=0
while IFS= read -r hit; do
  f=${hit%%:*}
  rest=${hit#*:}
  n=${rest%%:*}
  line=${rest#*:}
  [[ -n ${file_ok[$f]+x} ]] && continue
  [[ $line =~ ^[[:space:]]*(\*|//|/\*) ]] && continue
  t=${line#"${line%%[![:space:]]*}"}
  t=${t%"${t##*[![:space:]]}"}
  if [[ -n ${allowed["$f|$t"]+x} ]]; then
    seen=$((seen + 1))
    continue
  fi
  echo "  $f:$n: $t"
  bad=1
done < <(grep -rnE --include='*.c' --include='*.h' --include='*.cu' \
           --include='*.cuh' --include='*.cpp' --include='*.hpp' "$call" libs | sort)

if [[ $bad -ne 0 ]]; then
  echo "FAIL runtime_no_libc_alloc: runtime source calls the C allocator" \
       "(use arts_malloc/arts_calloc/arts_realloc/arts_free)"
  exit 1
fi
if [[ $seen -ne ${#allow_lines[@]} ]]; then
  echo "FAIL runtime_no_libc_alloc: ${seen} of ${#allow_lines[@]} allowed" \
       "sites found; an allowed line changed or moved"
  exit 1
fi
echo "PASS runtime_no_libc_alloc: ${seen} allowed sites"
