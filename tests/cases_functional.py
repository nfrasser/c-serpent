"""
Functional tests: compile the generated wrapper into a real extension module,
import it, and assert on actual behaviour. These need Python headers and numpy
headers, so they are skipped when none are available.

Each case is a dict:

    name      test name, also the temp subdirectory
    module    module name, must match CSERPENT_MODULE in src
    src       the C source, annotations included
    args      extra c-serpent arguments
    test      python source run against the built module; it should raise on
              failure, so plain asserts are enough

Source and wrapper are compiled as a single translation unit, which is what
lets the wrapper see struct definitions and enum constants.
"""

CASES = [

    dict(name="functional_scalars_and_arrays",
         module="fn_basic",
         src=r"""
#include <stdint.h>
#include <string.h>

enum status { ST_OK = 0, ST_FAIL = 7 };

int32_t sum_i32(int N, int32_t *a){ int32_t s=0; for(int i=0;i<N;i++) s+=a[i]; return s; }
double  meanf(int N, float *a){ double s=0; for(int i=0;i<N;i++) s+=a[i]; return s/N; }
double  meand(int N, double *a){ double s=0; for(int i=0;i<N;i++) s+=a[i]; return s/N; }
const char *may_fail(int x){ return x ? "boom" : 0; }
int64_t count_bytes(char *b){ return b ? (int64_t)strlen(b) : -1; }
_Bool   negate(_Bool b){ return !b; }

#ifdef CSERPENT
CSERPENT_MODULE(fn_basic)
CSERPENT_WRAPFN(sum_i32, doc = "Sum an int32 array.")
CSERPENT_WRAPFN(may_fail, errstr = 1)
CSERPENT_WRAPFN(count_bytes, name = "nbytes")
CSERPENT_WRAPFN(negate)
CSERPENT_WRAPFN_GENERIC(mean)
CSERPENT_WRAPCONST(status)
#endif
""",
         test=r"""
import numpy, fn_basic as m

# scalars and typed arrays
assert m.sum_i32(4, numpy.array([1,2,3,4], dtype=numpy.int32)) == 10
assert m.negate(False) is True

# keyword arguments use the C parameter names
assert m.sum_i32(N=2, a=numpy.array([10,20], dtype=numpy.int32)) == 30

# generic dispatch picks the variant matching the array dtype
assert m.mean(3, numpy.array([1,2,3], dtype=numpy.float32)) == 2.0
assert m.mean(3, numpy.array([1,2,3], dtype=numpy.float64)) == 2.0
assert m.meanf(3, numpy.array([1,2,3], dtype=numpy.float32)) == 2.0

# char* is a string, not an array; None is a null pointer
assert m.nbytes('hello') == 5
assert m.nbytes(None) == -1
assert not hasattr(m, 'count_bytes')     # renamed

# docstrings and enum constants
assert m.sum_i32.__doc__ == 'Sum an int32 array.'
assert (m.ST_OK, m.ST_FAIL) == (0, 7)

# errstr turns a non-NULL return into an exception
assert m.may_fail(0) is None
try:
    m.may_fail(1)
    raise AssertionError("expected RuntimeError")
except RuntimeError as e:
    assert str(e) == 'boom', e

# wrong dtype, non-contiguous, and no matching variant all raise ValueError
for bad in (lambda: m.sum_i32(2, numpy.array([1.0, 2.0])),
            lambda: m.sum_i32(2, numpy.array([1,2,3,4], dtype=numpy.int32)[::2]),
            lambda: m.mean(3, numpy.array([1,2,3], dtype=numpy.int8))):
    try:
        bad()
        raise AssertionError("expected ValueError")
    except ValueError:
        pass

# a null array argument is allowed
assert m.sum_i32(0, None) == 0

# a read-only array cannot be passed where C takes a non-const pointer, and the
# message says so rather than blaming contiguity
ro = numpy.array([1, 2], dtype=numpy.int32)
ro.setflags(write=False)
try:
    m.sum_i32(2, ro)
    raise AssertionError("expected ValueError for a read-only array")
except ValueError as e:
    assert "read-only" in str(e), e
"""),

    dict(name="functional_bytes_and_addresses",
         module="fn_buf",
         src=r"""
#include <stdint.h>
int32_t first_byte(int8_t *b){ return b ? b[0] : -1; }

#ifdef CSERPENT
CSERPENT_MODULE(fn_buf)
CSERPENT_CONFIG(bytes = 1, addresses = 1)
CSERPENT_WRAPFN(first_byte)
#endif
""",
         test=r"""
import numpy, fn_buf as m
a = numpy.array([65, 66], dtype=numpy.int8)
assert m.first_byte(a) == 65
assert m.first_byte(b'AB') == 65                    # bytes accepted
assert m.first_byte(a.ctypes.data) == 65            # raw address accepted
assert m.first_byte(None) == -1
"""),

    dict(name="functional_structs",
         module="fn_structs",
         src=r"""
#include <stdint.h>
#include <stdlib.h>

typedef struct { int32_t x; int32_t y; } Point;

struct Matrix {
    int32_t rows;
    int32_t cols;
    double  scale;
    _Bool   ready;
    Point   origin;
    double  coeffs[4];
    char   *label;
    double *data;
};

struct Matrix *matrix_new(int32_t r, int32_t c) {
    struct Matrix *m = calloc(1, sizeof *m);
    m->rows = r; m->cols = c; m->scale = 1.0; m->label = "hello";
    return m;
}
void    matrix_free(struct Matrix *m) { free(m); }
double  matrix_trace(struct Matrix *m) { return m ? m->rows * m->scale : -1.0; }
Point   point_add(Point a, Point b) { Point r = { a.x+b.x, a.y+b.y }; return r; }
int32_t point_norm1(Point *p) { int32_t x=p->x, y=p->y; return (x<0?-x:x)+(y<0?-y:y); }

#ifdef CSERPENT
CSERPENT_MODULE(fn_structs)
CSERPENT_WRAPTYPE(Point)
CSERPENT_WRAPTYPE(Matrix, exclude = (data), readonly = (rows, cols))
CSERPENT_WRAPFN(matrix_new, matrix_trace, point_norm1, point_add)
CSERPENT_WRAPFN(matrix_free, invalidates = 1)
#endif
""",
         test=r"""
import gc, fn_structs as m

# construction by keyword, and a struct pointer argument
p = m.Point(x=3, y=-4)
assert (p.x, p.y) == (3, -4)
assert m.point_norm1(p) == 7
assert repr(p) == '<Point x=3 y=-4>'

# struct by value, in and out
q = m.point_add(p, m.Point(x=1, y=1))
assert type(q).__name__ == 'Point' and (q.x, q.y) == (4, -3)

# a C-owned pointer return
mat = m.matrix_new(3, 4)
assert (mat.rows, mat.cols) == (3, 4)
assert m.matrix_trace(mat) == 3.0
assert mat.label == 'hello'          # char* reads as str
assert mat.ready is False            # _Bool reads as bool
assert isinstance(mat.address, int) and mat.address != 0

# scalar members are writable
mat.scale = 2.5
assert m.matrix_trace(mat) == 7.5

# nested struct members are views: writes go through to the parent
mat.origin.x = 7
assert mat.origin.x == 7

# fixed-size array members are numpy views over the parent's storage
assert mat.coeffs.shape == (4,) and str(mat.coeffs.dtype) == 'float64'
mat.coeffs[:] = [1, 2, 3, 4]
assert list(mat.coeffs) == [1.0, 2.0, 3.0, 4.0]
assert mat.coeffs.base is not None

# excluded members are absent; readonly and pointer members reject writes
assert not hasattr(mat, 'data')
for attr, val in (('rows', 9), ('label', 'x'), ('coeffs', [0,0,0,0])):
    try:
        setattr(mat, attr, val)
        raise AssertionError('expected AttributeError for ' + attr)
    except AttributeError:
        pass

# invalidates = 1 makes use-after-free an exception rather than a crash
m.matrix_free(mat)
try:
    mat.rows
    raise AssertionError('expected ValueError after free')
except ValueError:
    pass

# None is a null pointer
assert m.matrix_trace(None) == -1.0

# a view keeps its parent alive even after the parent name goes away
owned = m.Matrix()
view, arr = owned.origin, owned.coeffs
view.x = 11
arr[0] = 9.5
del owned
gc.collect()
assert view.x == 11 and arr[0] == 9.5

# wrong type for a struct argument
try:
    m.point_norm1(42)
    raise AssertionError('expected TypeError')
except TypeError:
    pass
"""),


    dict(name="functional_errbuf",
         module="fn_err",
         src=r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
    The caller-supplied buffer idiom: NULL means report and die, non-NULL means
    a buffer of at least 80 bytes which is zeroed on entry.
*/
void scale(int n, float *x, float k, char *errmsg)
{
    if (errmsg) errmsg[0] = 0;
    if (n < 0) {
        if (!errmsg) { fprintf(stderr, "negative length\n"); exit(EXIT_FAILURE); }
        snprintf(errmsg, 80, "negative length: %d", n);
        return;
    }
    for (int i = 0; i < n; i++) x[i] *= k;
}

#ifdef CSERPENT
CSERPENT_MODULE(fn_err)
CSERPENT_CONFIG(errbuf_size = 80)
CSERPENT_WRAPFN(scale, errbuf = errmsg)
#endif
""",
         test=r"""
import numpy, fn_err as m

x = numpy.array([1, 2, 3], dtype=numpy.float32)
m.scale(3, x, 2.0)
assert x.tolist() == [2.0, 4.0, 6.0]

# an error written into the buffer becomes an exception carrying the message
try:
    m.scale(-1, x, 2.0)
    raise AssertionError("expected RuntimeError")
except RuntimeError as e:
    assert str(e) == "negative length: -1", e

# the buffer argument is supplied by the wrapper: python cannot pass it, and
# the function is therefore never called with errmsg == NULL, so its
# exit(EXIT_FAILURE) path is unreachable from python
try:
    m.scale(3, x, 2.0, "oops")
    raise AssertionError("expected TypeError")
except TypeError:
    pass
"""),

    dict(name="functional_errjmp",
         module="fn_jmp",
         src=r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
    A library with an overridable assert and no return path for error data:
    the handler is not expected to return, so the only alternatives are abort
    (which takes the interpreter with it) and longjmp.
*/
static void (*lib_panic)(const char *msg, const char *file, int line);
void lib_set_panic(void (*fn)(const char*, const char*, int)) { lib_panic = fn; }

#define LIB_ASSERT(c) do { if(!(c)) { \
    if (lib_panic) lib_panic(#c, __FILE__, __LINE__); \
    fprintf(stderr, "assert failed: %s\n", #c); abort(); } } while(0)

float lib_mean(int n, float *x)
{
    LIB_ASSERT(n > 0);
    LIB_ASSERT(x != 0);
    float s = 0;
    for (int i = 0; i < n; i++) s += x[i];
    return s / n;
}

/* the adapter the user writes: c-serpent cannot know the library's hook API */
void cserpent_raise(const char *msg);

static void my_panic(const char *msg, const char *file, int line)
{
    char buf[256];
    snprintf(buf, sizeof buf, "assertion failed: %s (%s:%d)", msg, file, line);
    cserpent_raise(buf);
}

void install(void) { lib_set_panic(my_panic); }

#ifdef CSERPENT
CSERPENT_MODULE(fn_jmp)
CSERPENT_CONFIG(runtime = 1, errjmp = 1)
CSERPENT_WRAPFN(lib_mean, install)
#endif
""",
         test=r"""
import numpy, threading, fn_jmp as m

m.install()
x = numpy.array([1, 2, 3], dtype=numpy.float32)

assert m.lib_mean(3, x) == 2.0

# a failed assertion arrives as an exception rather than killing the process
for bad, want in ((-1, "n > 0"), (None, "x != 0")):
    try:
        m.lib_mean(3 if bad is None else bad, None if bad is None else x)
        raise AssertionError("expected RuntimeError")
    except RuntimeError as e:
        assert want in str(e), e
        assert "assertion failed" in str(e), e

# and the interpreter is still here afterwards
assert m.lib_mean(3, x) == 2.0

# the jmp_buf is thread-local, so concurrent failures do not tread on each
# other -- and crucially the GIL is reacquired on the longjmp path, which is
# what stops this from deadlocking or crashing
errs = []
def worker():
    try:
        m.lib_mean(-5, x)
    except RuntimeError as e:
        errs.append(str(e))

ts = [threading.Thread(target=worker) for _ in range(4)]
for t in ts: t.start()
for t in ts: t.join()
assert len(errs) == 4, errs
assert len(set(errs)) == 1, errs

assert m.lib_mean(3, x) == 2.0
"""),
    dict(name="functional_converter",
         module="fn_view",
         # the converters mention PyObject and numpy, so c-serpent's own
         # preprocessor run needs the real include directories
         args=["-p", "cc -E -I@PYINC@ -I@NPINC@"],
         src=r"""
#include <stdint.h>
#define NPY_NO_DEPRECATED_API NPY_1_8_API_VERSION
#include <Python.h>
#include <numpy/arrayobject.h>

/* ---- the project's own view types ------------------------------------- */

typedef struct
{
        float *p;
	int64_t n;     // length
} View1D;

typedef struct
{
        float *p;
	int64_t n[2];  // shape
	int64_t st[1]; // stride in elements, implied contiguous last dimension
} View2D;

typedef struct
{
        float *p;
	int64_t n[3];  // shape
	int64_t st[2]; // stride in elements, implied contiguous last dimension
} View3D;

typedef struct
{
        float *p;
	int64_t n[4];  // shape
	int64_t st[3]; // stride in elements, implied contiguous last dimension
} View4D;

/* ---- the converters ---------------------------------------------------- */

/*
	Shared validation. The view convention is strides in *elements* for every
	axis but the last, which is implied contiguous -- so this accepts an array
	sliced along any outer axis, and rejects one whose last axis is strided.
	numpy strides are in bytes, hence the division.
*/
static int cs_view_unpack(PyObject *o, const char *argname, int ndim,
                          float **p, int64_t *n, int64_t *st)
{
	PyArrayObject *a = (PyArrayObject *)o;
	const npy_intp isz = (npy_intp) sizeof(float);

	if (!PyArray_Check(o)) {
		PyErr_Format(PyExc_TypeError,
			"argument '%s' must be a numpy array", argname);
		return 0;
	}
	if (PyArray_TYPE(a) != NPY_FLOAT) {
		PyErr_Format(PyExc_ValueError,
			"argument '%s' must have dtype float32", argname);
		return 0;
	}
	if (PyArray_NDIM(a) != ndim) {
		PyErr_Format(PyExc_ValueError,
			"argument '%s' must be %d-dimensional, not %d-dimensional",
			argname, ndim, PyArray_NDIM(a));
		return 0;
	}
	if (!PyArray_ISALIGNED(a)) {
		PyErr_Format(PyExc_ValueError,
			"argument '%s' is not suitably aligned", argname);
		return 0;
	}
	/*
		The view hands C a mutable float*, and mutation is the common case,
		so a read-only array must be refused rather than silently written to.
	*/
	if (!PyArray_ISWRITEABLE(a)) {
		PyErr_Format(PyExc_ValueError,
			"argument '%s' is read-only, but the view allows writing", argname);
		return 0;
	}
	if (PyArray_STRIDE(a, ndim-1) != isz) {
		PyErr_Format(PyExc_ValueError,
			"argument '%s' must be contiguous along its last axis", argname);
		return 0;
	}

	for (int i = 0; i < ndim; i++)
		n[i] = (int64_t) PyArray_DIM(a, i);

	for (int i = 0; i < ndim-1; i++) {
		npy_intp s = PyArray_STRIDE(a, i);
		if (s % isz) {
			PyErr_Format(PyExc_ValueError,
				"argument '%s' has a stride that is not a whole number of elements",
				argname);
			return 0;
		}
		st[i] = (int64_t)(s / isz);
	}

	*p = (float *) PyArray_DATA(a);
	return 1;
}

/*
	Building an array back out. The result does not own its memory and has no
	base object, so whatever owns the buffer must outlive it.
*/
static PyObject *cs_view_pack(float *p, int ndim, const int64_t *n, const int64_t *st)
{
	npy_intp dims[4], strides[4];

	for (int i = 0; i < ndim; i++) dims[i] = (npy_intp) n[i];
	strides[ndim-1] = (npy_intp) sizeof(float);
	for (int i = 0; i < ndim-1; i++)
		strides[i] = (npy_intp) st[i] * (npy_intp) sizeof(float);

	return PyArray_New(&PyArray_Type, ndim, dims, NPY_FLOAT, strides,
	                   p, 0, NPY_ARRAY_WRITEABLE, NULL);
}

static int view1d_from_obj(PyObject *o, const char *argname, View1D *out)
{
	int64_t n[1], st[1];
	if (!cs_view_unpack(o, argname, 1, &out->p, n, st)) return 0;
	out->n = n[0];
	return 1;
}
static int view2d_from_obj(PyObject *o, const char *argname, View2D *out)
{ return cs_view_unpack(o, argname, 2, &out->p, out->n, out->st); }
static int view3d_from_obj(PyObject *o, const char *argname, View3D *out)
{ return cs_view_unpack(o, argname, 3, &out->p, out->n, out->st); }
static int view4d_from_obj(PyObject *o, const char *argname, View4D *out)
{ return cs_view_unpack(o, argname, 4, &out->p, out->n, out->st); }

static PyObject *view1d_to_obj(View1D v)
{ int64_t n[1] = { v.n }; return cs_view_pack(v.p, 1, n, 0); }
static PyObject *view2d_to_obj(View2D v)
{ return cs_view_pack(v.p, 2, v.n, v.st); }
static PyObject *view3d_to_obj(View3D v)
{ return cs_view_pack(v.p, 3, v.n, v.st); }
static PyObject *view4d_to_obj(View4D v)
{ return cs_view_pack(v.p, 4, v.n, v.st); }

/* ---- ordinary project code, with no Python in sight -------------------- */

float sum1(View1D v)
{
	float s = 0;
	for (int64_t i = 0; i < v.n; i++) s += v.p[i];
	return s;
}

float sum2(View2D v)
{
	float s = 0;
	for (int64_t i = 0; i < v.n[0]; i++)
		for (int64_t j = 0; j < v.n[1]; j++)
			s += v.p[i*v.st[0] + j];
	return s;
}

void scale2(View2D v, float k)
{
	for (int64_t i = 0; i < v.n[0]; i++)
		for (int64_t j = 0; j < v.n[1]; j++)
			v.p[i*v.st[0] + j] *= k;
}

int64_t rows2(View2D v) { return v.n[0]; }

int64_t stride2(View2D v) { return v.st[0]; }

View2D self2(View2D v) { return v; }

float sum3(View3D v)
{
	float s = 0;
	for (int64_t i = 0; i < v.n[0]; i++)
		for (int64_t j = 0; j < v.n[1]; j++)
			for (int64_t k = 0; k < v.n[2]; k++)
				s += v.p[i*v.st[0] + j*v.st[1] + k];
	return s;
}

float sum4(View4D v)
{
	float s = 0;
	for (int64_t i = 0; i < v.n[0]; i++)
		for (int64_t j = 0; j < v.n[1]; j++)
			for (int64_t k = 0; k < v.n[2]; k++)
				for (int64_t l = 0; l < v.n[3]; l++)
					s += v.p[i*v.st[0] + j*v.st[1] + k*v.st[2] + l];
	return s;
}

#ifdef CSERPENT
CSERPENT_MODULE(fn_view)

CSERPENT_CONVERTER(from_python = view1d_from_obj, to_python = view1d_to_obj)
CSERPENT_CONVERTER(from_python = view2d_from_obj, to_python = view2d_to_obj)
CSERPENT_CONVERTER(from_python = view3d_from_obj, to_python = view3d_to_obj)
CSERPENT_CONVERTER(from_python = view4d_from_obj, to_python = view4d_to_obj)

CSERPENT_WRAPFN(sum1, sum2, sum3, sum4, scale2, rows2, stride2, self2)
#endif
""",
         test=r"""
import numpy, fn_view as m

def arr(*shape):
    n = 1
    for s in shape: n *= s
    return numpy.arange(n, dtype=numpy.float32).reshape(*shape)

# the plain contiguous cases
a1, a2, a3, a4 = arr(6), arr(2, 3), arr(2, 3, 4), arr(2, 3, 4, 5)
assert m.sum1(a1) == a1.sum()
assert m.sum2(a2) == a2.sum()
assert m.sum3(a3) == a3.sum()
assert m.sum4(a4) == a4.sum()
assert m.rows2(a2) == 2

# the point of carrying strides: an array sliced along an outer axis is still
# a valid view, because only the last axis has to be contiguous
big = arr(6, 4)
sliced = big[::2]                      # stride 8 elements, last axis contiguous
assert not sliced.flags["C_CONTIGUOUS"]
assert m.sum2(sliced) == sliced.sum()
assert m.rows2(sliced) == 3

# strides reach C in *elements*, not bytes: numpy reports 32 bytes here, and
# the view must say 8. This is what a bytes/elements mix-up would break.
assert sliced.strides[0] == 32
assert m.stride2(sliced) == 8
assert m.stride2(a2) == 3          # contiguous 2x3: one row is 3 elements

col = arr(4, 6)[:, ::2]                # last axis strided: must be rejected
try:
    m.sum2(col)
    raise AssertionError("expected ValueError for a strided last axis")
except ValueError as e:
    assert "contiguous along its last axis" in str(e), e

# non-owning, so C writes through to the caller's array -- including a slice
before = big.copy()
m.scale2(sliced, 2.0)
assert (big[::2] == before[::2] * 2).all()
assert (big[1::2] == before[1::2]).all()      # untouched rows

# the other direction preserves shape and strides
r = m.self2(sliced)
assert r.shape == sliced.shape
assert r.strides == sliced.strides
r[0, 0] = 99.0
assert big[0, 0] == 99.0                      # still the same memory

# validation
try:
    m.sum2(arr(2, 3).astype(numpy.float64))
    raise AssertionError("expected ValueError for float64")
except ValueError as e:
    assert "dtype float32" in str(e), e

try:
    m.sum2(a1)
    raise AssertionError("expected ValueError for wrong ndim")
except ValueError as e:
    assert "2-dimensional" in str(e), e

try:
    m.sum1([1.0, 2.0])
    raise AssertionError("expected TypeError for a list")
except TypeError as e:
    assert "numpy array" in str(e), e

# the view exposes a mutable float*, so a read-only array is refused rather
# than being written to behind numpy's back
ro = arr(2, 3)
ro.setflags(write=False)
try:
    m.sum2(ro)
    raise AssertionError("expected ValueError for a read-only array")
except ValueError as e:
    assert "read-only" in str(e), e
"""),
]
