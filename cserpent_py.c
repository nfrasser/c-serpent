// Wrapper for cserpent.c to be used as a Python module
// ... Yeah I know, ironic that cserpent can't wrap itself yet
// (at least not with a pleasant python-side API)
//
// Building this creates the '_cserpent' extension module, which backs both
// 'import cserpent' and the 'cserpent' command line program. The standalone
// executable build (just compiling cserpent.c) is independent of this.
//
// You can probably build this with:
//   PY_INC_DIR="$(python -c 'import sysconfig; print(sysconfig.get_paths()["include"])')"
//   cc -shared -fPIC -o _cserpent.so cserpent_py.c -I$PY_INC_DIR
//
// You'll need the python headers, perhaps from python3-dev or python3-devel
// if you're on a debian or redhat based system, respectively.

#define CSERPENT_SUPPRESS_MAIN
#include "cserpent.c"

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdlib.h>

/*
	Output goes to open_memstream rather than a fixed buffer: c-serpent can
	emit a lot of code for a large input, and a fixed buffer silently
	truncates, since fprintf to a full stream fails and nothing checks it.
	cserpent_main takes FILE* directly, so no size has to be guessed.
*/
PyObject *
wrap_run_cserpent(PyObject *self, PyObject *args)
{
	(void) self;

	PyObject *argList;
	const char *stdinString;
	if (!PyArg_ParseTuple(args, "Os", &argList, &stdinString)) {
		return 0;
	}

	// make sure we got a list of strings
	if (!PyList_Check(argList)) {
		PyErr_SetString(PyExc_TypeError, "First argument must be a list");
		return 0;
	}

	Py_ssize_t listSize = PyList_Size(argList);
	for (Py_ssize_t i = 0; i < listSize; i++) {
		PyObject *item = PyList_GetItem(argList, i);
		if (!PyUnicode_Check(item)) {
			PyErr_SetString(PyExc_TypeError, "List items must be strings");
			return 0;
		}
	}

	char   **cserpent_argv = (char **)malloc((listSize + 1) * sizeof(char *));
	char    *out_buf = 0, *err_buf = 0;
	size_t   out_len = 0,  err_len = 0;
	FILE    *in = 0, *out = 0, *err = 0;
	PyObject *result = 0;

	if (!cserpent_argv) return PyErr_NoMemory();

	for (Py_ssize_t i = 0; i < listSize; i++)
		cserpent_argv[i] = (char*) PyUnicode_AsUTF8(PyList_GetItem(argList, i));
	cserpent_argv[listSize] = 0; // Null terminate the array of arguments

	out = open_memstream(&out_buf, &out_len);
	err = open_memstream(&err_buf, &err_len);
	in  = fmemopen((void*)stdinString, strlen(stdinString), "r");

	if (!out || !err || !in) {
		PyErr_SetString(PyExc_OSError, "could not open in-memory streams");
		goto done;
	}

	int retCode = cserpent_main(cserpent_argv, in, out, err);

	fflush(out);
	fflush(err);

	result = Py_BuildValue("iss", retCode,
	                       out_buf ? out_buf : "",
	                       err_buf ? err_buf : "");

done:
	if (in)  fclose(in);
	if (out) fclose(out);
	if (err) fclose(err);
	free(out_buf);
	free(err_buf);
	free(cserpent_argv);
	return result;
}

static PyMethodDef module_methods[] = {
	{"run_cserpent", wrap_run_cserpent, METH_VARARGS,
	 "run_cserpent(args, stdin_text) -> (returncode, stdout, stderr)"},
	{NULL, NULL, 0, NULL}
};

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"_cserpent",
	"C-serpent's code generator, as an extension module.",
	-1,
	module_methods
};

PyMODINIT_FUNC PyInit__cserpent(void) {
	return PyModule_Create(&moduledef);
}
