'''
Tests for the notebook path (cserpent.py).

These are built differently from the other functional tests: the notebook path
calls c-serpent through the _cserpent extension module rather than the
standalone binary, so the harness compiles cserpent_py.c against the target
Python first. Skipped along with the other functional tests when no suitable
Python is available.

Each case is a dict:

    name    test name, also the temp subdirectory
    test    python source, run with the temp directory as the working directory
            and _cserpent.so and cserpent.py alongside it. It should raise on
            failure, so plain asserts are enough.
'''

CASES = [

    dict(name="notebook_compile_and_reload",
         test=r'''
import sys
sys.path.insert(0, ".")
import cserpent

m = cserpent.CSerpentModule("nbt", working_dir=".")

code = """
#include <stdint.h>
enum colour { RED, GREEN };
typedef struct { int32_t x; int32_t y; } Point;

int32_t twice(int32_t x){ return %d*x; }
int32_t norm1(Point *p){ return (p->x<0?-p->x:p->x) + (p->y<0?-p->y:p->y); }

#ifdef CSERPENT
CSERPENT_WRAPTYPE(Point)
CSERPENT_WRAPFN(twice, norm1)
CSERPENT_WRAPCONST(colour)
#endif
"""

mod = m.compile(code % 2, quiet=True)
assert mod.twice(21) == 42
assert (mod.RED, mod.GREEN) == (0, 1)
assert mod.norm1(mod.Point(x=3, y=-4)) == 7

# Hot reload with no delay. Two builds inside the same second must not
# collide, which is what the old timestamp-based module name got wrong.
mod2 = m.compile(code % 3, quiet=True)
assert mod2.twice(21) == 63

# the stable alias follows the most recent build
import nbt
assert nbt.twice(21) == 63
'''),

    dict(name="notebook_interface_matches_standalone",
         test=r'''
import sys
sys.path.insert(0, ".")
import cserpent

m = cserpent.CSerpentModule("nbi", working_dir=".")

# An annotation outside an '#ifdef CSERPENT' block has to fail here exactly as
# it would in a .c file: the two interfaces are meant to be identical, so that
# cell code ports by copy and paste. The failure carries a hint.
try:
    m.compile("int f(void){return 1;}\nCSERPENT_WRAPFN(f)\n", quiet=True)
    raise AssertionError("expected CSerpentError for a bare annotation")
except cserpent.CSerpentError as e:
    assert "#ifdef CSERPENT" in str(e), e

# The module name is the one thing the notebook supplies, so a cell must not
# also declare it. That should say so, rather than reporting a duplicate.
try:
    m.compile("int g(void){return 1;}\n"
              "#ifdef CSERPENT\n"
              "CSERPENT_MODULE(mine)\n"
              "CSERPENT_WRAPFN(g)\n"
              "#endif\n", quiet=True)
    raise AssertionError("expected CSerpentError for a cell-supplied module name")
except cserpent.CSerpentError as e:
    assert "supplied automatically" in str(e), e
'''),
]
