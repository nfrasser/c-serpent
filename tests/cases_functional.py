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

]
