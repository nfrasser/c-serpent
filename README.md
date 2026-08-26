<img src='https://github.com/hsnyder/c-serpent/blob/master/logo.png' height=128 width=128 />

C-SERPENT
=========

C-serpent is a tool designed to make it easier to call C code from Python.
CPython (the main Python implementation) has a C API that makes it possible
to write "extension modules" in C. These extension modules are imported and
called just like regular Python code, but are actually written in C.
However, that API has a bit of a learning curve. C-serpent aims to make 
this process easier by automatically generating the necessary "wrapper" 
code that is required to make a C function callable just like an ordinary
python function. 

There are two ways to use C-serpent:

- **As a standalone program.** You can compile cserpent.c and then use
  it as a standalone command-line program. You mark up your C source with
  annotations saying which functions you want wrapped — these live inside
  `#ifdef CSERPENT` blocks, so they disappear from every ordinary build —
  and hand C-serpent the files. It reads them, parses the function
  signatures, and emits the wrapper code for you. You compile the wrapper
  and your code together yourself. Using C-serpent this way gives you full
  control over your build process: C-serpent is *only* generating
  boilerplate code for you.

- **For interactive development in Jupyter.** If you use Jupyter for
  interactive development, you can just write C code in a string within
  your notebook and use the machinery in cserpentmodule.py to compile and
  run it on the fly. Live code re-loading is supported.
  cserpentmodule assumes the compiler is gcc, but it should work with 
  clang as well if you override some default options. See cserpentmodule.py
  for details. MSVC is not currently supported.

  *March 2024:* C-Serpent is now available on PyPI, and can be installed with 
  `pip install cserpent`. It is distributed as a *source package,* so you
  will need a compiler and python development headers available. Contributions 
  which improve the packaging and distribution are very, very welcome.

The rest of this README focuses on C-serpent as a standalone program. 
See NotebookExample.py to learn about using C-serpent interactively.

C-serpent is not completely general-purpose:

 - It only supports wrapping a subset of all possible C function signatures 
   (details below).

 - It assumes the use of the popular `numpy` package for numerical arrays. 


| C type                     | Python type                |
| -------------------------- | -------------------------- |
| signed/unsigned char       | int                        |
| signed/unsigned short      | int                        |
| signed/unsigned int        | int                        |
| signed/unsigned long       | int                        |
| signed/unsigned long long  | int                        |
| float                      | float                      |
| double                     | float                      | 
| long double                | (not supported)            |
| complex float              | (not supported)            |
| complex double             | (not supported)            |
| complex long double        | (not supported)            |
| char*                      | str                        |
| void*                      | int                        |
| signed/unsigned char*      | numpy array (int8/uint8)   |
| signed/unsigned short*     | numpy array (int16/uint16) |
| signed/unsigned int*       | numpy array (int32/uint32) |
| signed/unsigned long*      | numpy array (varies)       |
| signed/unsigned long long* | numpy array (int64/uint64) |
| float*                     | numpy array (float32)      |
| double*                    | numpy array (float64)      |
| complex float*             | numpy array (complex64)    |
| complex double*            | numpy array (complex128)   |

`long*` arguments are mapped either to 32 or 64 bit integers, depending
on the size of `long` on your platform (use `int32_t`/`int64_t` from 
`stdint.h` to avoid this). `char*` arguments are assumed to be strings 
(use `signed char*` for int8). `void*` arguments are converted to and 
from Python integers. `char*` and `void*` are also supported as return 
types, but other pointer types are not (so define your output arrays in 
Python, and populate them with C code). Non-void pointer arguments also 
accept `None`, which results in a null pointer being passed to the C function.
`stdint.h` types are supported, since they are just typedefs.

Example, consider this C function:

    double mean_i32(int N, int32_t *array) 
    {
        double sum = 0.0;
        for (int i = 0; i < N; i++) sum += array[i];
        return sum/N;
    }

The resulting wrapper will accept a Python integer for `N`, and either `None` 
or a numpy array with `dtype=numpy.int32` for `array`. An exception will
be raised if the supplied types don't match. 

Compiling
---------

To build C-serpent, just point your C compiler at cserpent.c. 
For example on a unix-derivative, 

    $ cc -g cserpent.c -o cserpent 

You can define `CSERPENT_DISABLE_ASSERT` to compile out assertions, if
you wish to.
        
Usage
-----

You tell C-serpent what to wrap *in the C source itself*, inside `#ifdef CSERPENT`
blocks. C-serpent defines `CSERPENT` when it preprocesses your file; nothing else
does, so those blocks vanish from every ordinary build. They are plain C syntax,
so editors and language servers do not complain about them.

C-serpent generates code, so you'll have to compile that code into an extension
module in order to actually import and use the module.

Consider the same example function from above:

    double mean_i32(int N, int32_t *array) 
    {
        double sum = 0.0;
        for (int i = 0; i < N; i++) sum += array[i];
        return sum/N;
    }

To make this function callable from Python, you might do the following:

1. save that function to a file (e.g. `mean.c`), and annotate it:

```c
#ifdef CSERPENT
CSERPENT_MODULE(means)
CSERPENT_WRAPFN(mean_i32)
#endif
```

2. ask python where its headers are

```
$ python
>>> import sysconfig
>>> sysconfig.get_paths()['include']
'/usr/include/python3.11'
>>> import numpy
>>> numpy.get_include()
'/usr/lib/python3.11/site-packages/numpy/_core/include'
```

3. use cserpent to generate the wrapper code

```
$ cserpent mean.c > mean_wrappers.c
```

4. compile the wrapper code and the original C code into an extension module

```
$ cc -fPIC -shared \
     -I/usr/lib/python3.11/site-packages/numpy/_core/include \
     -I/usr/include/python3.11 \
     mean_wrappers.c mean.c \
     -lpython -o means.so
```

5. call it:
 
```
$ python
>>> import means, numpy
>>> x = numpy.array([1,2,3,4], dtype=numpy.int32)
>>> means.mean_i32(len(x), x)
2.5
```

Manifest files
--------------

Annotations do not have to live next to the code they describe. Because they
work in headers too, the usual arrangement is a *manifest*: a file that includes
the headers it needs and otherwise contains nothing but instructions.

```c
/* mymodule.cs.c */
#include "mylib.h"
#include <thirdparty/lib.h>

#ifdef CSERPENT
CSERPENT_MODULE(mymodule)
CSERPENT_WRAPFN(mean_i32)
CSERPENT_WRAPFN_GENERIC(sum_)
#endif
```

```
$ cserpent mymodule.cs.c > wrappers.c
```

This is the recommended style. It works on code you cannot edit — vendored
libraries, system headers, generated sources — and it keeps module names out of
library sources, where they do not belong. Several inputs may be given at once:
`cserpent mymodule.cs.c extra.c`.

Annotations
-----------

    CSERPENT_MODULE(name)

        Name the generated module. It must match the name of the shared library
        you build. At most once across all inputs. If you leave it out entirely,
        C-serpent emits the wrappers with no module definition, which is useful
        if you are assembling a module by hand.

    CSERPENT_WRAPFN(names..., options...)

        Wrap one or more functions. Any options given apply to all the names.

            name = "str"      the name Python sees, if different from the C
                              name. Only valid with a single name.
            doc = "str"       docstring
            errstr = 1        the function returns `const char *`, and a
                              non-NULL return is an error message: raise it
            errcheck = fn     after calling, call `fn`, which must return
                              `const char *`; non-NULL becomes an exception
            errarg = N        which argument to hand to `errcheck`. 0, the
                              default, means the wrapped function's return value
            addresses = 0|1   accept python integers, representing raw
                              addresses, where numpy arrays are expected
            bytes = 0|1       accept bytes objects where numpy arrays are
                              expected

        For example:

            CSERPENT_WRAPFN(alpha, beta, gamma)
            CSERPENT_WRAPFN(read_frame, errcheck = check_err, errarg = 2)
            CSERPENT_WRAPFN(fft_internal, name = "fft", doc = "In-place FFT.")

    CSERPENT_WRAPFN_GENERIC(prefix, options...)

        Generate a type-dispatching wrapper over the suffixed variants of
        `prefix`, plus a wrapper for each variant found. See "Generic functions"
        below. Accepts every `CSERPENT_WRAPFN` option, applied to all variants,
        plus:

            strip_underscore = 0|1   by default a trailing underscore is removed
                                     from the dispatcher's Python name, so
                                     `sum_` is called as `sum`. 0 keeps it.

    CSERPENT_WRAPFN_MANUAL(names..., options...)

        If you are writing a wrapper by hand — because C-serpent doesn't support
        some type or usage pattern — this registers it. C-serpent adds `name` to
        the module and expects you to supply a function called `wrap_name`,
        which you prepend to the code C-serpent generates. Accepts `doc` and
        `name`.

    CSERPENT_WRAPCONST(names...)

        Add integer constants to the module. A name that is an enum tag adds
        every constant of that enum; a name that is an enum constant adds just
        that one, which is how you reach the members of an anonymous enum.

            CSERPENT_WRAPCONST(error_code)
            CSERPENT_WRAPCONST(TP_OK, TP_EOF)

        `#define`d constants cannot be wrapped: the preprocessor expands them
        before C-serpent ever sees them.

        Note that the emitted wrapper code needs to be able to access the
        constants. Either use `CSERPENT_CONFIG(declarations = 0)` and assemble
        the generated code into the same translation unit as the enum, or
        prepend an `#include` of the relevant header to the output.

    CSERPENT_OPAQUE(TypeName)

        Treat `TypeName` as equivalent to `void`, so that pointers to it are
        converted to and from python integers. This is how you handle pointers
        to types C-serpent cannot otherwise deal with, such as structs.

    CSERPENT_CONFIG(options...)

        Settings for the whole generated file. Setting the same key to two
        different values is an error, except `float16`, which is OR'd together.

            addresses = 0|1     default for the per-function option above
            bytes = 0|1         default for the per-function option above
            declarations = 0|1  by default the output contains declarations for
                                the functions being wrapped, which saves writing
                                a separate `.h`. Set 0 if you are compiling
                                everything as one translation unit, or if a
                                manifest already includes the real headers.
            float16 = 0|1       enable `_Float16` support (requires compiler
                                support)

Command line
------------

    cserpent [-p CMD] [-v] [-W] [--explain] input.c [input2.c ...] > wrappers.c

Input files are positional. `-` means already-preprocessed source on standard
input, which C-serpent will not preprocess again — whoever preprocessed it must
have passed `-DCSERPENT`, or the annotations will have been stripped.

    -h          print help and exit

    -p CMD      the preprocessor command, `cc -E` by default. Include
                directories go here, e.g. -p "cc -E -Ivendor/include".

    -v          verbose: print the typedefs that were parsed, for debugging

    -W          enable warnings

    --explain   list every wrap C-serpent found, with the file and line each
                annotation came from, then exit without generating anything.
                Useful when annotations are spread across several headers.

Environment variables:

    CSERPENT_PP   acts like the -p flag, but -p overrides it

Generic functions
-----------------

     If you have several copies of a function that accept arguments that are   
     of different data types, then c-serpent may be able to automatically      
     generate a dispatch function for you, that allows it to be called from   
     python in a type-generic way. In order to use this feature, your function 
     must use a function-name suffix to indicate the data type, following this 
     convention: 
                                                                               
       type            suffix 
       ----            ------ 
       int8            b 
       int16           s 
       int32           i 
       int64           l 
        
       uint8           B 
       uint16          S 
       uint32          I 
       uint64          L 
        
       float           f 
       _Float16        h 
       double          d 
        
       complex float    F 
       complex _Float16 H 
       complex double   D 
                                                                               
     You do not need to supply all of these variants; c-serpent will support   
     whichever variants it finds. 
                                                                               
     Example: if whatever.c contains the following functions, and is annotated
     with `CSERPENT_WRAPFN_GENERIC(mean)`, then python code will be able to
     call `mymodule.mean(N, arr)` where arr is a float or double array:
                                                                               
       double meanf(int N, float *arr);                                        
       double meand(int N, double *arr);                                       
                                                                               
     C-serpent will try to figure out which arguments change according to the  
     convention and which do not. Return values may also change.               
                                                                               
     Lastly, the type-specific versions of the function do still get wrapped.  

Upgrading from v1
-----------------

v1 configured everything with command-line flags. v2 moves the semantic ones
into the source and keeps only the flags that describe your machine. Old flags
are recognised and produce an error naming their replacement.

| v1 flag | v2 |
| ------- | -- |
| `-f file` | input files are positional |
| function names on the command line | `CSERPENT_WRAPFN(name)` |
| `-m name` | `CSERPENT_MODULE(name)` |
| `-D` | `CSERPENT_CONFIG(declarations = 0)` |
| `-f16` | `CSERPENT_CONFIG(float16 = 1)` |
| `-x name` | `CSERPENT_WRAPFN_MANUAL(name)` |
| `-e` | `errstr = 1` |
| `-e,n,chkfn` | `errcheck = chkfn, errarg = n` |
| `-g` | `CSERPENT_WRAPFN_GENERIC(prefix)` |
| `-G` | `strip_underscore = 0` |
| `-E` | `CSERPENT_WRAPCONST(...)`, naming the constants you want |
| `-t T` | `CSERPENT_OPAQUE(T)` |
| `-a` / `-b` | `addresses = 1` / `bytes = 1`, per function or in `CSERPENT_CONFIG` |
| `-P`, `-i`, `-I` | removed; preprocessing is now mandatory, and include directories go in `-p` |

`-E` is worth a special mention. It wrapped every enum constant in the
preprocessed file, which meant its behaviour depended on which headers you had
included — harmless for some, and 98 stray constants from `netdb.h` for others.
`CSERPENT_WRAPCONST` requires you to say what you want.

v1 remains available at the git tag `v1.1.4`.
