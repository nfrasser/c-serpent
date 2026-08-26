'''
Write C in a notebook cell, compile it, and hot-reload the result.

You say what to wrap with CSERPENT_* annotations in the C source, exactly as
you would in a .c file compiled by the standalone tool -- '#ifdef CSERPENT'
blocks and all. The interface is deliberately identical, because notebook code
is usually a draft of something that ends up in a real source file: porting it
should be copy and paste, with nothing to rewrite.

The one difference is the module name. It is regenerated for every build, which
is what makes reloading work, so this module supplies CSERPENT_MODULE and your
cell must not. When you move the code into a .c file, add a CSERPENT_MODULE
line; that is the whole porting story.


EXAMPLE USAGE

    import cserpent

    m = cserpent.CSerpentModule('my_module')
    my_module = m.compile("""
        #include <stdint.h>

        int64_t add_i64(int64_t a, int64_t b) { return a + b; }

        #ifdef CSERPENT
        CSERPENT_WRAPFN(add_i64)
        #endif
    """)

    my_module.add_i64(3, 4)     # 7

    # change the code and call compile again; the module is rebuilt and
    # reloaded in place, as often as you like
    my_module = m.compile("""
        #include <stdint.h>

        int64_t add_i64(int64_t a, int64_t b) { return a + b + 1; }

        #ifdef CSERPENT
        CSERPENT_WRAPFN(add_i64)
        #endif
    """)

    my_module.add_i64(3, 4)     # 8


IN IPYTHON OR JUPYTER

    Importing cserpent registers a cell magic, which saves the string quoting
    and binds the module into your namespace under the name you give it:

        %%cserpent my_module
        #include <stdint.h>
        int64_t add_i64(int64_t a, int64_t b) { return a + b; }
        #ifdef CSERPENT
        CSERPENT_WRAPFN(add_i64)
        #endif

    Re-running the cell rebuilds and reloads. The cell body is plain C, so it
    can be copied straight into a .c file.


PREREQUISITES

    - The _cserpent extension module. 'pip install cserpent' provides it, or
      build it yourself; see the comments at the top of cserpent_py.c.

    - A C compiler whose preprocessor can write to stdout, which in practice
      means gcc or clang. MSVC is not currently supported.
'''

import importlib
import os
import subprocess
import sys
import sysconfig

import _cserpent

__version__ = "2.0.0"



class CSerpentError(Exception):
    '''Raised when preprocessing, wrapper generation, or compilation fails.'''


# This is the default configuration for gcc.
# You can override this with your own compiler configuration.
# This will probably work with clang with only minor modifications.
compiler_config_gcc = {
        'preprocessor': 'cc -E -', # note the dash: read from stdin
        'preprocessor_include_flag': '-I',
        'compiler': 'cc',
        'include_flag': '-I',
        'linkdir_flag': '-L',
        'output_flag': '-o ', # trailing space is important
        'default_ccflags': [
                "-Wall", "-Wno-unused-function", # default warnings
                "-shared", "-fPIC", # required for building a shared library
                "-x", "c", "-"], # specify that the input is C code, and read from stdin

        'rpath_flag': '-Wl,--disable-new-dtags,-rpath,', # set to None if not needed
        # rpath is a way to "bake" the location of shared libraries into the
        # compiled binary. This is only needed if you're linking against shared
        # libraries that are not in the standard search path. If you're not sure,
        # you probably don't need it, and can set this to None.
}


def _env_list(name):
    return os.environ[name].split() if name in os.environ else []


class CSerpentModule:

        def __init__(self,
                        module_name,
                        working_dir='/tmp',
                        compiler_config=compiler_config_gcc,
                        ):

                if working_dir is None:
                        working_dir = os.getcwd()
                if working_dir not in sys.path:
                        sys.path.append(working_dir)

                self.modname = module_name
                self.working_dir = working_dir
                self.compiler_config = compiler_config
                self.last_opath = None

                # A counter, not a timestamp. Each build needs a fresh module
                # name, and a one-second clock meant two builds in the same
                # second silently reused the older module.
                self._build = 0

        def compile(self, c_code,
                        ccflags=("-O3", "-fopenmp"),
                        includedirs=(),  # recommend absolute paths
                        linkdirs=(),     # recommend absolute paths
                        linkflags=(),
                        extra_cserpent_flags=(),
                        quiet=False,
                        ):
                '''
                Compile c_code into an extension module and import it.

                What gets wrapped is determined by the CSERPENT_* annotations
                in c_code. Returns the imported module, and also binds it under
                this object's module_name so that 'import <name>' works.

                Raises CSerpentError if any stage fails.
                '''

                includedirs = list(includedirs) + _env_list('CSERPENT_EXTRA_INCLUDE_DIRS')
                ccflags     = list(ccflags)     + _env_list('CSERPENT_EXTRA_CCFLAGS')
                linkdirs    = list(linkdirs)    + _env_list('CSERPENT_EXTRA_LINKDIRS')
                linkflags   = list(linkflags)   + _env_list('CSERPENT_EXTRA_LINKFLAGS')

                # imported here rather than at module scope so that the
                # command line program does not require numpy to be installed
                import numpy
                includedirs += [sysconfig.get_paths()['include'], numpy.get_include()]

                # 1. preprocess, with CSERPENT defined so that any '#ifdef
                #    CSERPENT' blocks in the cell survive. The annotation
                #    macros are deliberately left undefined here, so that they
                #    come through as ordinary tokens for c-serpent to read.

                preprocessor_cmd = self.compiler_config['preprocessor'].split()
                preprocessor_cmd += [self.compiler_config['preprocessor_include_flag'] + d
                                     for d in includedirs]
                preprocessor_cmd += ['-DCSERPENT', '-DCSERPENT_VERSION=2']

                pp = subprocess.run(preprocessor_cmd,
                                    input=c_code.encode('utf-8'),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)

                if pp.returncode != 0:
                        raise CSerpentError("Preprocessing failed:\n"
                                            + pp.stderr.decode('utf-8', 'replace'))

                preprocessed_code = pp.stdout.decode('utf-8', 'replace')

                # 2. append the module name. It is regenerated for every build,
                #    which is what makes reloading work, so the cell author
                #    cannot write it. This text is already preprocessed, so it
                #    needs no '#ifdef CSERPENT' wrapper.

                self._build += 1
                python_mod_name = "%s_%d_%d" % (self.modname, os.getpid(), self._build)

                preprocessed_code += (
                        "\nCSERPENT_CONFIG(declarations = 0)\n"
                        "CSERPENT_MODULE(%s)\n" % python_mod_name)

                # 3. generate the wrappers

                cserpent_args = ["-W"] + list(extra_cserpent_flags) + ["-"]
                rc, out, err = _cserpent.run_cserpent(cserpent_args, preprocessed_code)

                if err and not quiet:
                        print(err, file=sys.stderr)
                if rc != 0:
                        hint = ""
                        if "CSERPENT_MODULE already given" in err:
                                hint = ("\n\nThe module name is supplied automatically here, because it "
                                        "has to\nchange on every build for reloading to work. Remove the "
                                        "CSERPENT_MODULE\nline from the cell; add it back when you move "
                                        "this code into a .c file.")
                        raise CSerpentError("C-serpent failed:\n" + err + hint)

                # 4. compile the cell and the generated wrappers as one
                #    translation unit, so that the wrappers can see struct
                #    definitions and enum constants. The annotation macros are
                #    defined away here, so bare annotations vanish.

                full_code = c_code + "\n\n" + out

                opath = os.path.join(self.working_dir, python_mod_name + ".so")
                cmd  = self.compiler_config['compiler'].split()
                cmd += [self.compiler_config['include_flag'] + d for d in includedirs]
                cmd += self.compiler_config['default_ccflags']
                cmd += (self.compiler_config['output_flag'] + opath).split()
                cmd += ccflags
                cmd += [self.compiler_config['linkdir_flag'] + d for d in linkdirs]
                if self.compiler_config['rpath_flag'] is not None:
                        cmd += [self.compiler_config['rpath_flag'] + d for d in linkdirs]
                cmd += linkflags

                if not quiet:
                        print("Running compiler:")
                        print(" ".join(cmd))

                cr = subprocess.run(cmd, input=full_code.encode('utf-8'),
                                    stderr=subprocess.PIPE)

                if cr.returncode != 0:
                        msg = cr.stderr.decode('utf-8', 'replace')
                        hint = ""
                        if "CSERPENT_" in msg:
                                hint = ("\n\nAnnotations must sit inside an '#ifdef CSERPENT' block, so "
                                        "that they\ndisappear from the compiled code. This is the same rule "
                                        "the standalone\ntool follows, which is what makes a cell portable "
                                        "to a .c file unchanged.")
                        raise CSerpentError("Compilation failed:\n" + msg + hint)

                warnings = cr.stderr.decode('utf-8', 'replace').strip()
                if warnings and not quiet:
                        print(warnings, file=sys.stderr)
                if not quiet:
                        print("Build successful!")

                # 5. import it, and alias it under the stable name

                importlib.invalidate_caches()
                imported = importlib.import_module(python_mod_name)
                sys.modules[self.modname] = imported

                if self.last_opath and os.path.isfile(self.last_opath):
                        os.remove(self.last_opath)
                self.last_opath = opath

                return imported


# --------------------------------------------------------------------------
# IPython / Jupyter cell magic
# --------------------------------------------------------------------------

_magic_modules = {}


def _cserpent_magic(line, cell):
        '''%%cserpent <module_name> -- compile the cell and bind the module.'''
        from IPython import get_ipython

        parts = line.split()
        name = parts[0] if parts else 'cserpent_cell'

        if name not in _magic_modules:
                _magic_modules[name] = CSerpentModule(name)

        module = _magic_modules[name].compile(cell, extra_cserpent_flags=parts[1:])

        ip = get_ipython()
        if ip is not None:
                ip.user_ns[name] = module
        return None


def _register_magic():
        try:
                from IPython import get_ipython
        except ImportError:
                return
        ip = get_ipython()
        if ip is None:
                return
        ip.register_magic_function(_cserpent_magic, magic_kind='cell', magic_name='cserpent')


_register_magic()


# --------------------------------------------------------------------------
# Command line entry point
# --------------------------------------------------------------------------

def main(argv=None):
        '''
        The 'cserpent' command. The extension module contains the whole code
        generator, so the command line program is a shim over the same C code
        that the notebook path uses -- there is no separate binary to build.
        '''
        argv = list(sys.argv[1:] if argv is None else argv)

        if "--version" in argv:
                print("c-serpent " + __version__)
                return 0

        # c-serpent opens input files itself; '-' means it wants stdin
        stdin_text = sys.stdin.read() if "-" in argv else ""

        rc, out, err = _cserpent.run_cserpent(argv, stdin_text)

        if out:
                sys.stdout.write(out)
        if err:
                sys.stderr.write(err)
        return rc


if __name__ == "__main__":
        sys.exit(main())
