// Python C Extension - provides native math functions callable from VX
// Compile: gcc -shared -fPIC -o libpymath.dylib py_native_module.c -I$(python3 -c "import sysconfig; print(sysconfig.get_path('include'))") -undefined dynamic_lookup

#include <Python.h>
#include <stdio.h>

/* Native functions that VX can call via extern (C linkage) */
int py_add(int a, int b) {
    return a + b;
}

int py_multiply(int a, int b) {
    return a * b;
}

void py_print(const char* msg) {
    printf("%s", msg);
}

/* Python module functions */
static PyObject* pymath_add(PyObject* self, PyObject* args) {
    int a, b;
    if (!PyArg_ParseTuple(args, "ii", &a, &b))
        return NULL;
    return PyLong_FromLong(py_add(a, b));
}

static PyObject* pymath_multiply(PyObject* self, PyObject* args) {
    int a, b;
    if (!PyArg_ParseTuple(args, "ii", &a, &b))
        return NULL;
    return PyLong_FromLong(py_multiply(a, b));
}

static PyMethodDef PyMathMethods[] = {
    {"add", pymath_add, METH_VARARGS, "Add two integers"},
    {"multiply", pymath_multiply, METH_VARARGS, "Multiply two integers"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef pymathmodule = {
    PyModuleDef_HEAD_INIT,
    "pymath",
    NULL,
    -1,
    PyMathMethods
};

PyMODINIT_FUNC PyInit_pymath(void) {
    return PyModule_Create(&pymathmodule);
}
