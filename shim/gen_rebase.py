#!/usr/bin/env python3
"""Generate shim/vk_rebase.c: guest pointers below 4 GiB -> host, at the shim's entry.

Under a guest address base (the 64-bit "low window" Wine needs; FEX's 32-bit
guests go through the 32-bit thunks, which convert every pointer themselves,
so the layer is off for them) a guest address x below 4 GiB lives at host base + x, and host
base + x is ALSO a valid guest address for the same memory. Guest code hands
Vulkan structures full of such pointers to the library thunk, which passes
them to this shim unchanged; MoltenVK would dereference them as host
addresses -- in the macOS __PAGEZERO. (MEASURED: DXVK's vkCreateInstance
faulted at 0x31eca8; a lazy register fix-up in the fault handler then made
MoltenVK walk off an array whose end had been computed from the unfixed
pointer.)

For every entry point, the generated wrapper walks the arguments the way the
Vulkan registry describes them -- pointer parameters, the structures and
arrays they point to, their pNext chains (by sType), arrays of strings --
and rewrites every pointer below 4 GiB to base + pointer, IN PLACE, logging
each change; it calls the next layer (the hand-written overrides in wsi.c /
features.c, or MoltenVK) and then puts every rewritten field back. With no
base announced the wrappers only forward.

Not rewritten: pUserData (an application cookie, not necessarily a pointer),
unions, function pointers, and structures the included headers do not define.

Usage: gen_rebase.py vk.xml entrypoints.txt manual.txt out.c
  manual.txt: entry points implemented by hand (wsi.c, features.c) under the
  name lxrt_inner_<name>.
"""
import re
import sys
import xml.etree.ElementTree as ET

vkxml, ep_path, manual_path, out_path = sys.argv[1:5]
reg = ET.parse(vkxml).getroot()
entry = [l.strip() for l in open(ep_path) if l.strip() and not l.startswith("#")]
manual = [l.strip() for l in open(manual_path) if l.strip() and not l.startswith("#")]

# ---------------------------------------------------------------- what the headers define
# Types and commands of extensions whose header is not included (other
# platforms, provisional/beta, Vulkan SC) do not exist for the compiler.
PLATFORMS_OK = {"xlib", "xcb", "metal"}
excluded_types, excluded_cmds = set(), set()
for ext in reg.find("extensions"):
    plat = ext.get("platform")
    bad = (plat and plat not in PLATFORMS_OK) or ext.get("provisional") == "true" \
        or ext.get("supported") in ("disabled",) or "vulkan" not in (ext.get("supported") or "vulkan").split(",")
    if not bad:
        continue
    for req in ext.findall("require"):
        for t in req.findall("type"):
            excluded_types.add(t.get("name"))
        for c in req.findall("command"):
            excluded_cmds.add(c.get("name"))
# ...but a type another (included) extension or core also requires stays.
included_types = set()
for feat in reg.findall("feature"):
    if "vulkan" not in (feat.get("api") or "vulkan").split(","):
        continue
    for req in feat.findall("require"):
        if req.get("api") and "vulkan" not in req.get("api").split(","):
            continue
        for t in req.findall("type"):
            included_types.add(t.get("name"))
for ext in reg.find("extensions"):
    plat = ext.get("platform")
    bad = (plat and plat not in PLATFORMS_OK) or ext.get("provisional") == "true" \
        or ext.get("supported") in ("disabled",) or "vulkan" not in (ext.get("supported") or "vulkan").split(",")
    if bad:
        continue
    for req in ext.findall("require"):
        if req.get("api") and "vulkan" not in req.get("api").split(","):
            continue
        for t in req.findall("type"):
            included_types.add(t.get("name"))
excluded_types -= included_types


def vulkan_api(el):
    api = el.get("api")
    return api is None or "vulkan" in api.split(",")


def member_info(m):
    """(type, name, pointer depth, fixed array?, len attr, text)"""
    t = m.find("type").text
    n = m.find("name").text
    text = "".join(m.itertext())
    depth = text.split(n)[0].count("*")
    fixed = "[" in text.split(n, 1)[1] if n in text else False
    return t, n, depth, fixed, m.get("len"), text


structs = {}      # name -> [member tuples]
stypes = {}       # name -> VK_STRUCTURE_TYPE_...
aliases = {}
for t in reg.find("types"):
    if t.get("category") not in ("struct",) or not vulkan_api(t):
        continue
    name = t.get("name")
    if t.get("alias"):
        aliases[name] = t.get("alias")
        continue
    mems = []
    for m in t.findall("member"):
        if not vulkan_api(m):
            continue
        mems.append(member_info(m))
        if m.find("name").text == "sType" and m.get("values"):
            stypes[name] = m.get("values")
    structs[name] = mems


def defined(tname):
    return tname in structs and tname in included_types


def has_ptrs(tname, seen=None):
    """Does a struct (or anything it contains by value) hold pointers?"""
    seen = seen or set()
    if tname in seen or not defined(tname):
        return False
    seen.add(tname)
    for (t, n, depth, fixed, ln, _) in structs[tname]:
        if depth:
            return True
        if t in structs and has_ptrs(t, seen):
            return True
    return False


def count_expr(ln, owner):
    """C expression for an array length, or None if it is not a plain field."""
    if not ln:
        return "1"
    first = ln.split(",")[0]
    if first == "null-terminated" or first.startswith("latexmath") or re.search(r"[^A-Za-z0-9_]", first):
        return None
    return owner + first


out = []
w = out.append
w("""// GENERATED by shim/gen_rebase.py from vk.xml -- do not edit.
// See the generator for what this does and why.
#include <stdint.h>
#include <stddef.h>
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_window_t;
typedef uint32_t xcb_visualid_t;
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_xlib.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_metal.h>
#include "lxrt_host.h"

extern void *realloc(void *, size_t);
extern void free(void *);
extern char *getenv(const char *);
extern void *dlsym(void *, const char *);
extern int dprintf(int, const char *, ...);

// LXRT_VK_DEBUG=1: report every entry point that returns an error.
static int g_dbg = -1;
static VkResult rb_ret(const char *name, VkResult r)
{
    if (g_dbg < 0) { const char *e = getenv("LXRT_VK_DEBUG"); g_dbg = e && *e == '1'; }
    if (g_dbg && r < 0) dprintf(2, "[shim] %s -> %d\\n", name, (int)r);
    return r;
}

static uint64_t g_base;
static int g_base_known;
static uint64_t rb_base(void)
{
    if (!__atomic_load_n(&g_base_known, __ATOMIC_ACQUIRE)) {
        g_base = (uint64_t)lxrt_syscall2(LXRT_NR_GUEST_BASE_GET, 0, 0);
        if ((int64_t)g_base < 0) g_base = 0;
        // A 32-bit guest goes through FEX's 32-bit thunks, which already turn
        // every guest pointer into a host one (patches/fex-thunks-guestbase32
        // .patch); a value below 4 GiB reaching here is not a guest address.
        uint64_t (*bits)(void) = (uint64_t (*)(void))dlsym(0, "fex_lxrt_guest_bits");
        if (bits && bits() == 32) g_base = 0;
        __atomic_store_n(&g_base_known, 1, __ATOMIC_RELEASE);
    }
    return g_base;
}

typedef struct { void **field; void *old; } rb_ent;
typedef struct { unsigned n, cap; rb_ent *e; rb_ent local[128]; uint64_t base; } rb_log;

static void rb_init(rb_log *l) { l->n = 0; l->cap = 128; l->e = l->local; l->base = g_base; }
static void rb_undo(rb_log *l)
{
    while (l->n) { l->n--; *l->e[l->n].field = l->e[l->n].old; }
    if (l->e != l->local) free(l->e);
}
// Rewrite one pointer field in place (logged); returns the usable pointer.
static void *rb_fix(rb_log *l, void *const *pfield)
{
    void **f = (void **)pfield;
    uintptr_t v = (uintptr_t)*f;
    if (!v || v >= 0x100000000ull) return *f;
    if (l->n == l->cap) {
        unsigned nc = l->cap * 2;
        rb_ent *ne = realloc(l->e == l->local ? 0 : l->e, nc * sizeof *ne);
        if (!ne) return (void *)(v + l->base);        // cannot log: leave it rebased
        if (l->e == l->local)
            for (unsigned i = 0; i < l->n; i++) ne[i] = l->local[i];
        l->e = ne; l->cap = nc;
    }
    l->e[l->n].field = f; l->e[l->n].old = *f; l->n++;
    *f = (void *)(v + l->base);
    return *f;
}
// A pointer held in a local (a parameter): no log needed.
static inline void *rb_val(rb_log *l, const void *p)
{
    uintptr_t v = (uintptr_t)p;
    return (v && v < 0x100000000ull) ? (void *)(v + l->base) : (void *)p;
}

static void rb_chain(rb_log *l, const void *p);
""")

# forward declarations
gen_structs = [s for s in structs if defined(s) and has_ptrs(s)]
for s in gen_structs:
    w(f"static void rb_{s}(rb_log *l, const {s} *s);")
w("")


def emit_member_fix(owner, t, n, depth, fixed, ln, indent, ctx):
    """Code rewriting one member (owner is 's->'). ctx: struct name."""
    lines = []
    ind = " " * indent
    if n == "pUserData" or t.startswith("PFN_"):
        return lines
    if fixed:
        if depth == 0 and t in structs and defined(t) and has_ptrs(t):
            m = re.search(r"\[([^\]]+)\]", "".join(ln or ""))
        # fixed arrays of structs with pointers do not occur in practice
        return lines
    if depth == 0:
        if t in structs and defined(t) and has_ptrs(t):
            lines.append(f"{ind}rb_{t}(l, &{owner}{n});")
        return lines
    if n == "pNext":
        lines.append(f"{ind}rb_chain(l, rb_fix(l, (void *const *)&{owner}{n}));")
        return lines
    lines.append(f"{ind}{{ void *p_ = rb_fix(l, (void *const *)&{owner}{n});")
    cnt = count_expr(ln, owner)
    if depth == 1 and t in structs and defined(t) and has_ptrs(t) and cnt:
        lines.append(f"{ind}  if (p_) for (uint64_t i_ = 0; i_ < (uint64_t)({cnt}); i_++) rb_{t}(l, &((const {t} *)p_)[i_]); }}")
    elif depth == 2 and cnt:
        # array of pointers (strings, structure pointers)
        inner = ""
        if t in structs and defined(t) and has_ptrs(t):
            inner = f" rb_{t}(l, (const {t} *)q_);"
        lines.append(f"{ind}  if (p_) for (uint64_t i_ = 0; i_ < (uint64_t)({cnt}); i_++) {{ void *q_ = rb_fix(l, &((void *const *)p_)[i_]); (void)q_;{inner} }} }}")
    else:
        lines.append(f"{ind}  (void)p_; }}")
    return lines


for s in gen_structs:
    w(f"static void rb_{s}(rb_log *l, const {s} *s)")
    w("{")
    w("    if (!s) return;")
    for (t, n, depth, fixed, ln, text) in structs[s]:
        for line in emit_member_fix("s->", t, n, depth, fixed, ln, 4, s):
            w(line)
    w("}")
    w("")

# pNext chains, by sType
w("static void rb_chain(rb_log *l, const void *p)")
w("{")
w("    for (int guard = 0; p && guard < 64; guard++) {")
w("        const VkBaseInStructure *b = (const VkBaseInStructure *)p;")
w("        switch ((int)b->sType) {")
seen_st = set()
for s, st in stypes.items():
    if not defined(s) or st in seen_st:
        continue
    seen_st.add(st)
    if has_ptrs(s):
        w(f"        case {st}: rb_{s}(l, (const {s} *)p); return;   // rewrites its own pNext")
w("        default: break;")
w("        }")
w("        // A structure without pointers of its own: still follow the chain.")
w("        p = rb_fix(l, (void *const *)&((VkBaseInStructure *)p)->pNext);")
w("    }")
w("}")
w("")

# ---------------------------------------------------------------- commands
cmds = {}
cmd_alias = {}
for c in reg.find("commands"):
    if not vulkan_api(c):
        continue
    if c.get("alias"):
        cmd_alias[c.get("name")] = c.get("alias")
        continue
    proto = c.find("proto")
    ret = proto.find("type").text
    name = proto.find("name").text
    params = [member_info(p) for p in c.findall("param") if vulkan_api(p)]
    cmds[name] = (ret, params, "".join(proto.itertext()).rsplit(name, 1)[0].strip())


def resolve(n):
    seen = 0
    while n in cmd_alias and seen < 8:
        n = cmd_alias[n]
        seen += 1
    return n


wanted = []
for n in list(dict.fromkeys(entry + manual)):
    base = resolve(n)
    if base not in cmds or n in excluded_cmds or base in excluded_cmds:
        continue
    wanted.append((n, base))

table = []
for (n, base) in wanted:
    ret, params, rettext = cmds[base]
    decls = ", ".join(text.strip() for (_, _, _, _, _, text) in params)
    names = ", ".join(pn for (_, pn, _, _, _, _) in params)
    inner = f"lxrt_inner_{n}" if n in manual else f"lxrt_mvk_{n}"
    w(f"{rettext} {inner}({decls});")
    w(f"__attribute__((visibility(\"default\"))) {rettext} {n}({decls})")
    w("{")
    has_ptr_param = any(depth for (_, _, depth, _, _, _) in params)
    if not has_ptr_param:
        if ret == "VkResult":
            w(f"    return rb_ret(\"{n}\", {inner}({names}));")
        else:
            w(f"    {'return ' if ret != 'void' else ''}{inner}({names});")
        w("}")
        table.append(n)
        continue
    w("    if (!rb_base()) {")
    if ret == "VkResult":
        w(f"        return rb_ret(\"{n}\", {inner}({names}));")
    else:
        w(f"        {'return ' if ret != 'void' else ''}{inner}({names});")
    if ret == "void":
        w("        return;")
    w("    }")
    w("    rb_log l; rb_init(&l);")
    # pointer params: rebase the local, then walk what it points to
    pnames = {pn for (_, pn, _, _, _, _) in params}
    for (t, pn, depth, fixed, ln, text) in params:
        if not depth or pn == "pUserData":
            continue
        cast = text.strip()[: text.strip().rfind(pn)].strip()
        w(f"    {pn} = ({cast})rb_val(&l, {pn});")
    for (t, pn, depth, fixed, ln, text) in params:
        if not depth or pn == "pUserData":
            continue
        cnt = None
        if not ln:
            cnt = "1"
        else:
            first = ln.split(",")[0]
            if first in pnames:
                # length given by another parameter: a value, or *pointer
                ptype = next(p for p in params if p[1] == first)
                cnt = f"(({first}) ? *({first}) : 0)" if ptype[2] else first
            elif re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*->[A-Za-z_][A-Za-z0-9_]*", first):
                cnt = f"({first.split('->')[0]} ? {first} : 0)"
        if depth == 1 and t in structs and defined(t) and has_ptrs(t) and cnt:
            w(f"    if ({pn}) for (uint64_t i_ = 0; i_ < (uint64_t)({cnt}); i_++) rb_{t}(&l, &{pn}[i_]);")
        elif depth == 2 and cnt and ln:
            inner_walk = f" rb_{t}(&l, (const {t} *)q_);" if (t in structs and defined(t) and has_ptrs(t)) else ""
            w(f"    if ({pn}) for (uint64_t i_ = 0; i_ < (uint64_t)({cnt}); i_++) {{ void *q_ = rb_fix(&l, (void *const *)&((void *const *){pn})[i_]); (void)q_;{inner_walk} }}")
    if ret == "void":
        w(f"    {inner}({names});")
        w("    rb_undo(&l);")
    else:
        w(f"    {ret} r_ = {inner}({names});")
        w("    rb_undo(&l);")
        retexpr = f'rb_ret("{n}", r_)' if ret == "VkResult" else "r_"
        w(f"    return {retexpr};")
    w("}")
    w("")
    table.append(n)

w("// Name -> this file's wrapper, for vkGetInstanceProcAddr / vkGetDeviceProcAddr.")
w("static const struct { const char *n; void *f; } k_rebase_table[] = {")
for n in table:
    w(f"    {{ \"{n}\", (void *){n} }},")
w("};")
w("""
static int rb_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void *lxrt_rebase_proc(const char *name)
{
    if (!name) return 0;
    name = (const char *)rb_val(&(rb_log){ .base = rb_base() }, name);
    for (unsigned i = 0; i < sizeof k_rebase_table / sizeof k_rebase_table[0]; i++)
        if (rb_eq(name, k_rebase_table[i].n))
            return k_rebase_table[i].f;
    return 0;
}
""")
open(out_path, "w").write("\n".join(out) + "\n")
print(f"gen_rebase: {len(gen_structs)} structures, {len(table)} entry points", file=sys.stderr)
# The names that now have a wrapper: gen.py must emit their MoltenVK thunk
# hidden (lxrt_mvk_<name>) instead of exporting it.
open(out_path.replace(".c", ".names"), "w").write("\n".join(table) + "\n")
