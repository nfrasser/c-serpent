"""
Codegen and diagnostic tests. These only need a C compiler.

Each case is a dict:

    name      test name, also the temp subdirectory
    src       source text, written to <name>.c and passed to c-serpent
    files     extra files to create alongside it, {relative path: text}
    args      arguments before the input file; @SRC@ expands to the input path,
              and if no input appears in args the source file is appended
    stdin     text piped to c-serpent's stdin
    rc        expected exit code, default 0
    out_has / out_lacks / err_has / err_lacks
              substrings that must (or must not) appear
"""

PRELUDE = "#include <stdint.h>\n"

CASES = [

    # ------------------------------------------------ basic wrapping

    dict(name="basic",
         src=PRELUDE + """
double mean_i32(int N, int32_t *a){ double s=0; for(int i=0;i<N;i++) s+=a[i]; return s/N; }
#ifdef CSERPENT
CSERPENT_MODULE(means)
CSERPENT_WRAPFN(mean_i32)
#endif
""",
         out_has=["PyObject * wrap_mean_i32",
                  '{"mean_i32", (PyCFunction) wrap_mean_i32',
                  "PyMODINIT_FUNC PyInit_means"]),

    dict(name="no_module_emits_wrappers_only",
         src="""
void f(void);
#ifdef CSERPENT
CSERPENT_WRAPFN(f)
#endif
""",
         out_has=["wrap_f"],
         out_lacks=["PyMODINIT_FUNC"]),

    dict(name="multiple_names",
         src="""
void alpha(void); void beta(void); void gamma_(void);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN(alpha, beta, gamma_)
#endif
""",
         out_has=["wrap_alpha", "wrap_beta", "wrap_gamma_"]),

    dict(name="rename_and_doc",
         src="""
int internal_thing(int x);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN(internal_thing, name = "nice", doc = "Does a thing.")
#endif
""",
         out_has=['{"nice", (PyCFunction) wrap_internal_thing, METH_VARARGS|METH_KEYWORDS, "Does a thing."}']),

    dict(name="errstr",
         src="""
const char *may_fail(int x);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN(may_fail, errstr = 1)
#endif
""",
         out_has=["if(rtn) {", "PyErr_SetString(PyExc_RuntimeError, rtn)"]),

    dict(name="errcheck_errarg",
         src="""
const char *check_err(int x);
int read_frame(int a, int b, int c);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN(read_frame, errcheck = check_err, errarg = 2)
#endif
""",
         out_has=["const char *_exn = check_err(b);"]),

    dict(name="generic_dispatch",
         src=PRELUDE + """
double meanf(int N, float *a); double meand(int N, double *a);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN_GENERIC(mean)
#endif
""",
         out_has=["wrap_meanf", "wrap_meand", "PyObject * wrap_mean (",
                  '{"mean", (PyCFunction) wrap_mean']),

    dict(name="generic_strips_underscore",
         src=PRELUDE + """
double sum_f(int N, float *a); double sum_d(int N, double *a);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN_GENERIC(sum_)
#endif
""",
         out_has=['{"sum", (PyCFunction) wrap_sum_']),

    dict(name="generic_keeps_underscore",
         src=PRELUDE + """
double sum_f(int N, float *a); double sum_d(int N, double *a);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN_GENERIC(sum_, strip_underscore = 0)
#endif
""",
         out_has=['{"sum_", (PyCFunction) wrap_sum_']),

    dict(name="manual_wrapper",
         src="""
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN_MANUAL(handwritten, doc = "Mine.")
#endif
""",
         out_has=['{"handwritten", (PyCFunction) wrap_handwritten, METH_VARARGS|METH_KEYWORDS, "Mine."}'],
         out_lacks=["PyObject * wrap_handwritten ("]),

    dict(name="wrapconst_tag_and_member",
         src="""
enum error_code { E_OK, E_BAD };
enum { ANON_A = 5, ANON_B };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPCONST(error_code, ANON_A)
#endif
""",
         out_has=['PyModule_AddIntConstant(m, "E_OK", E_OK)',
                  'PyModule_AddIntConstant(m, "E_BAD", E_BAD)',
                  'PyModule_AddIntConstant(m, "ANON_A", ANON_A)'],
         out_lacks=['"ANON_B"']),

    dict(name="wrapconst_deduplicates",
         src="""
enum e { A };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPCONST(e, A)
#endif
""",
         out_has=['PyModule_AddIntConstant(m, "A", A)']),

    dict(name="opaque",
         src="""
typedef struct Ctx Ctx;
int use_ctx(Ctx *c);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_OPAQUE(Ctx)
CSERPENT_WRAPFN(use_ctx)
#endif
""",
         out_has=["unsigned long long c_ull"]),

    dict(name="config_bytes_and_addresses",
         src=PRELUDE + """
void takes(int32_t *a);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_CONFIG(bytes = 1, addresses = 1)
CSERPENT_WRAPFN(takes)
#endif
""",
         out_has=["PyBytes_Check(a_obj)", "PyLong_Check(a_obj)"]),

    dict(name="per_function_overrides_config",
         src=PRELUDE + """
void a_fn(int32_t *a); void b_fn(int32_t *a);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_CONFIG(bytes = 1)
CSERPENT_WRAPFN(a_fn)
CSERPENT_WRAPFN(b_fn, bytes = 0)
#endif
""",
         out_has=["PyBytes_Check(a_obj)"]),

    dict(name="config_declarations_off",
         src="""
int f(int x);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_CONFIG(declarations = 0)
CSERPENT_WRAPFN(f)
#endif
""",
         out_lacks=["int f (int x);"]),

    dict(name="config_float16",
         src="""
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_CONFIG(float16 = 1)
#endif
""",
         out_has=["_Float16: NPY_FLOAT16"]),

    # ------------------------------------------------ structs

    dict(name="struct_basic",
         src=PRELUDE + """
struct Matrix { int32_t rows; double scale; };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(Matrix)
#endif
""",
         out_has=["cs_Matrix_Object", "cs_Matrix_get_rows", "cs_Matrix_set_rows",
                  "cs_Matrix_get_scale", "PyType_Ready(&cs_Matrix_Type)",
                  'PyModule_AddObject(m, "Matrix"']),

    dict(name="struct_typedef_anonymous",
         src=PRELUDE + """
typedef struct { int32_t x; int32_t y; } Point;
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(Point)
#endif
""",
         # the C spelling must be 'Point', never 'struct Point'
         out_has=["Point *p;", "cs_Point_get_x"],
         out_lacks=["struct Point"]),

    dict(name="struct_union",
         src=PRELUDE + """
union Tag { int32_t i; float f; };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(Tag)
#endif
""",
         out_has=["union Tag *p;", "cs_Tag_get_i", "cs_Tag_get_f"]),

    dict(name="struct_fields_option",
         src=PRELUDE + """
struct P { int32_t a; int32_t b; int32_t c; };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(P, fields = (a, c))
#endif
""",
         out_has=["cs_P_get_a", "cs_P_get_c"],
         out_lacks=["cs_P_get_b"]),

    dict(name="struct_exclude_option",
         src=PRELUDE + """
struct P { int32_t a; int32_t b; };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(P, exclude = (b))
#endif
""",
         out_has=["cs_P_get_a"],
         out_lacks=["cs_P_get_b"]),

    dict(name="struct_readonly_all",
         src=PRELUDE + """
struct P { int32_t a; int32_t b; };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(P, readonly = 1)
#endif
""",
         out_lacks=["cs_P_set_a", "cs_P_set_b"]),

    dict(name="struct_readonly_some",
         src=PRELUDE + """
struct P { int32_t a; int32_t b; };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(P, readonly = (a))
#endif
""",
         out_has=["cs_P_set_b"],
         out_lacks=["cs_P_set_a"]),

    dict(name="struct_pointer_members_read_only",
         src=PRELUDE + """
struct P { char *label; double *data; };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(P)
#endif
""",
         out_has=["cs_P_get_label", "cs_P_get_data", "PyUnicode_FromString"],
         out_lacks=["cs_P_set_label", "cs_P_set_data"]),

    dict(name="struct_nested_and_array_members",
         src=PRELUDE + """
typedef struct { int32_t x; } Inner;
struct Outer { Inner inner; double coeffs[4]; };
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(Inner)
CSERPENT_WRAPTYPE(Outer)
#endif
""",
         # nested member is a view onto the parent; array member is a numpy view
         out_has=["cs_Inner_from_ptr(&s->p->inner, o, 0)",
                  "PyArray_SimpleNewFromData",
                  "PyArray_SetBaseObject"]),

    dict(name="struct_as_parameter_and_return",
         src=PRELUDE + """
typedef struct { int32_t x; } Point;
Point  point_add(Point a, Point b);
int32_t point_norm(Point *p);
Point *point_new(void);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(Point)
CSERPENT_WRAPFN(point_add, point_norm, point_new)
#endif
""",
         out_has=['cs_Point_ptr(p_obj, "p", 1, &p)',       # pointer arg accepts None
                  'cs_Point_ptr(a_obj, "a", 0, &_tmp)',    # by-value arg does not
                  "cs_Point_new_owned()",                  # by-value return
                  "cs_Point_from_ptr(rtn, NULL, 1)"]),     # pointer return is C-owned

    dict(name="struct_invalidates",
         src=PRELUDE + """
typedef struct { int32_t x; } Point;
void point_free(Point *p);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPTYPE(Point)
CSERPENT_WRAPFN(point_free, invalidates = 1)
#endif
""",
         out_has=["((cs_Point_Object*)p_obj)->p = NULL;"]),

    # ------------------------------------------------ locations, includes, stdin

    dict(name="annotation_before_definition",
         # regression: the definition search must not trip over the annotation's
         # own mention of the function name
         src="""
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN(my_cool_fn)
#endif
void my_cool_fn(int N, float *X) { for(int i=0;i<N;i++) X[i] += 1.0f; }
""",
         out_has=["PyObject * wrap_my_cool_fn"]),

    dict(name="explain_reports_header_locations",
         files={"inc/mylib.h": """
#ifndef MYLIB_H
#define MYLIB_H
int lib_add(int a, int b);
#ifdef CSERPENT
CSERPENT_WRAPFN(lib_add)
#endif
#endif
"""},
         src="""
#include "mylib.h"
#ifdef CSERPENT
CSERPENT_MODULE(m)
#endif
""",
         args=["-p", "cc -E -Iinc", "--explain"],
         err_has=["CSERPENT_WRAPFN(lib_add)", "inc/mylib.h:6"]),

    dict(name="stdin_input",
         src="",
         args=["-"],
         stdin="""
void f(void);
CSERPENT_MODULE(m)
CSERPENT_WRAPFN(f)
""",
         out_has=["wrap_f", "PyInit_m"]),

    dict(name="identical_duplicate_collapses",
         src="""
void f(void);
#ifdef CSERPENT
CSERPENT_MODULE(m)
CSERPENT_WRAPFN(f, doc = "x")
CSERPENT_WRAPFN(f, doc = "x")
#endif
""",
         out_has=["wrap_f"]),

    # ------------------------------------------------ diagnostics

    dict(name="err_unknown_annotation",
         src="#ifdef CSERPENT\nCSERPENT_WRAPFUN(foo)\n#endif\n",
         rc=1, err_has=["unknown annotation 'CSERPENT_WRAPFUN'"]),

    dict(name="err_module_twice",
         src="#ifdef CSERPENT\nCSERPENT_MODULE(a)\nCSERPENT_MODULE(b)\n#endif\n",
         rc=1, err_has=["CSERPENT_MODULE already given"]),

    dict(name="err_conflicting_config",
         src="#ifdef CSERPENT\nCSERPENT_CONFIG(bytes = 1)\nCSERPENT_CONFIG(bytes = 0)\n#endif\n",
         rc=1, err_has=["'bytes' set to 0 here, but to 1 at"]),

    dict(name="err_wrapconst_define",
         src="#define MAX_LEN 10\n#ifdef CSERPENT\nCSERPENT_WRAPCONST(MAX_LEN)\n#endif\n",
         rc=1, err_has=["is a literal, not a name", "#define"]),

    dict(name="err_wrapconst_unknown",
         src="#ifdef CSERPENT\nCSERPENT_WRAPCONST(NOPE)\n#endif\n",
         rc=1, err_has=["'NOPE' is not an enum tag or an enum constant"]),

    dict(name="err_function_not_found",
         src="#ifdef CSERPENT\nCSERPENT_WRAPFN(nope)\n#endif\n",
         rc=1, err_has=["no function called 'nope'"]),

    dict(name="err_conflicting_duplicate",
         src="""
void f(void);
#ifdef CSERPENT
CSERPENT_WRAPFN(f, doc = "x")
CSERPENT_WRAPFN(f, doc = "y")
#endif
""",
         rc=1, err_has=["wrapped here with different options"]),

    dict(name="err_name_with_several_names",
         src="""
void f(void); void g(void);
#ifdef CSERPENT
CSERPENT_WRAPFN(f, g, name = "x")
#endif
""",
         rc=1, err_has=["'name' cannot be used with more than one name"]),

    dict(name="err_struct_bitfield",
         src="""
struct HasBits { int a; int b : 3; };
#ifdef CSERPENT
CSERPENT_WRAPTYPE(HasBits)
#endif
""",
         rc=1, err_has=["expected ';' after member 'b'", "bitfields"]),

    dict(name="err_struct_unsupported_member",
         src="""
struct HasLD { int a; long double d; };
#ifdef CSERPENT
CSERPENT_WRAPTYPE(HasLD)
#endif
""",
         rc=1, err_has=["cannot represent", "exclude = (d)"]),

    dict(name="err_struct_not_found",
         src="#ifdef CSERPENT\nCSERPENT_WRAPTYPE(Nope)\n#endif\n",
         rc=1, err_has=["no struct or union called 'Nope'"]),

    dict(name="err_fields_and_exclude",
         src="""
struct P { int a; int b; };
#ifdef CSERPENT
CSERPENT_WRAPTYPE(P, fields = (a), exclude = (b))
#endif
""",
         rc=1, err_has=["'fields' and 'exclude' cannot both be given"]),

    dict(name="err_unknown_option",
         src="""
void f(void);
#ifdef CSERPENT
CSERPENT_WRAPFN(f, wat = 1)
#endif
""",
         rc=1, err_has=["unknown option 'wat'"]),

    # every v1 flag should name its replacement rather than saying "unrecognized"
    dict(name="err_retired_m",   src="", args=["-m", "foo", "@SRC@"],
         rc=1, err_has=["removed in c-serpent v2", "CSERPENT_MODULE(name)"]),
    dict(name="err_retired_E",   src="", args=["-E", "@SRC@"],
         rc=1, err_has=["CSERPENT_WRAPCONST"]),
    dict(name="err_retired_f",   src="", args=["-f", "@SRC@"],
         rc=1, err_has=["positional"]),
    dict(name="err_retired_g",   src="", args=["-g", "@SRC@"],
         rc=1, err_has=["CSERPENT_WRAPFN_GENERIC"]),
    dict(name="err_retired_t",   src="", args=["-t", "Foo", "@SRC@"],
         rc=1, err_has=["CSERPENT_OPAQUE"]),
    dict(name="err_retired_e_n", src="", args=["-e,2,chk", "@SRC@"],
         rc=1, err_has=["errcheck"]),
    dict(name="err_retired_P",   src="", args=["-P", "@SRC@"],
         rc=1, err_has=["preprocessing is now mandatory"]),
    dict(name="err_retired_I",   src="", args=["-I", "inc", "@SRC@"],
         rc=1, err_has=["include directories in -p"]),

    dict(name="err_unrecognized_flag", src="", args=["--nonsense", "@SRC@"],
         rc=1, err_has=["unrecognized flag"]),

    # ------------------------------------------------ warnings

    dict(name="warn_no_module",
         src="void f(void);\n#ifdef CSERPENT\nCSERPENT_WRAPFN(f)\n#endif\n",
         args=["-W"],
         err_has=["no CSERPENT_MODULE found"]),

    dict(name="warn_errarg_without_errcheck",
         src="void f(int a, int b);\n#ifdef CSERPENT\nCSERPENT_WRAPFN(f, errarg = 2)\n#endif\n",
         args=["-W"],
         err_has=["'errarg' has no effect without 'errcheck'"]),

    dict(name="warnings_silent_without_W",
         src="void f(void);\n#ifdef CSERPENT\nCSERPENT_WRAPFN(f)\n#endif\n",
         err_lacks=["warning"]),
]
