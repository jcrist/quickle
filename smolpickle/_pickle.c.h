PyDoc_STRVAR(_pickle_Pickler___init____doc__,
"Pickler(file, protocol=None, fix_imports=True, buffer_callback=None)\n"
"--\n"
"\n"
"This takes a binary file for writing a pickle data stream.\n"
"\n"
"The optional *protocol* argument tells the pickler to use the given\n"
"protocol; supported protocols are 0, 1, 2, 3, 4 and 5.  The default\n"
"protocol is 4. It was introduced in Python 3.4, and is incompatible\n"
"with previous versions.\n"
"\n"
"Specifying a negative protocol version selects the highest protocol\n"
"version supported.  The higher the protocol used, the more recent the\n"
"version of Python needed to read the pickle produced.\n"
"\n"
"The *file* argument must have a write() method that accepts a single\n"
"bytes argument. It can thus be a file object opened for binary\n"
"writing, an io.BytesIO instance, or any other custom object that meets\n"
"this interface.\n"
"\n"
"If *fix_imports* is True and protocol is less than 3, pickle will try\n"
"to map the new Python 3 names to the old module names used in Python\n"
"2, so that the pickle data stream is readable with Python 2.\n"
"\n"
"If *buffer_callback* is None (the default), buffer views are\n"
"serialized into *file* as part of the pickle stream.\n"
"\n"
"If *buffer_callback* is not None, then it can be called any number\n"
"of times with a buffer view.  If the callback returns a false value\n"
"(such as None), the given buffer is out-of-band; otherwise the\n"
"buffer is serialized in-band, i.e. inside the pickle stream.\n"
"\n"
"It is an error if *buffer_callback* is not None and *protocol*\n"
"is None or smaller than 5.");

static int
_pickle_Pickler___init___impl(PicklerObject *self, PyObject *file,
                              PyObject *protocol, int fix_imports,
                              PyObject *buffer_callback);

static int
_pickle_Pickler___init__(PyObject *self, PyObject *args, PyObject *kwargs)
{
    int return_value = -1;
    static const char * const _keywords[] = {"file", "protocol", "fix_imports", "buffer_callback", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "Pickler", 0};
    PyObject *argsbuf[4];
    PyObject * const *fastargs;
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t noptargs = nargs + (kwargs ? PyDict_GET_SIZE(kwargs) : 0) - 1;
    PyObject *file;
    PyObject *protocol = Py_None;
    int fix_imports = 1;
    PyObject *buffer_callback = Py_None;

    fastargs = _PyArg_UnpackKeywords(_PyTuple_CAST(args)->ob_item, nargs, kwargs, NULL, &_parser, 1, 4, 0, argsbuf);
    if (!fastargs) {
        goto exit;
    }
    file = fastargs[0];
    if (!noptargs) {
        goto skip_optional_pos;
    }
    if (fastargs[1]) {
        protocol = fastargs[1];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    if (fastargs[2]) {
        fix_imports = PyObject_IsTrue(fastargs[2]);
        if (fix_imports < 0) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
    buffer_callback = fastargs[3];
skip_optional_pos:
    return_value = _pickle_Pickler___init___impl((PicklerObject *)self, file, protocol, fix_imports, buffer_callback);

exit:
    return return_value;
}

PyDoc_STRVAR(_pickle_Unpickler___init____doc__,
"Unpickler(file, *, fix_imports=True, encoding=\'ASCII\', errors=\'strict\',\n"
"          buffers=())\n"
"--\n"
"\n"
"This takes a binary file for reading a pickle data stream.\n"
"\n"
"The protocol version of the pickle is detected automatically, so no\n"
"protocol argument is needed.  Bytes past the pickled object\'s\n"
"representation are ignored.\n"
"\n"
"The argument *file* must have two methods, a read() method that takes\n"
"an integer argument, and a readline() method that requires no\n"
"arguments.  Both methods should return bytes.  Thus *file* can be a\n"
"binary file object opened for reading, an io.BytesIO object, or any\n"
"other custom object that meets this interface.\n"
"\n"
"Optional keyword arguments are *fix_imports*, *encoding* and *errors*,\n"
"which are used to control compatibility support for pickle stream\n"
"generated by Python 2.  If *fix_imports* is True, pickle will try to\n"
"map the old Python 2 names to the new names used in Python 3.  The\n"
"*encoding* and *errors* tell pickle how to decode 8-bit string\n"
"instances pickled by Python 2; these default to \'ASCII\' and \'strict\',\n"
"respectively.  The *encoding* can be \'bytes\' to read these 8-bit\n"
"string instances as bytes objects.");

static int
_pickle_Unpickler___init___impl(UnpicklerObject *self, PyObject *file,
                                int fix_imports, const char *encoding,
                                const char *errors, PyObject *buffers);

static int
_pickle_Unpickler___init__(PyObject *self, PyObject *args, PyObject *kwargs)
{
    int return_value = -1;
    static const char * const _keywords[] = {"file", "fix_imports", "encoding", "errors", "buffers", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "Unpickler", 0};
    PyObject *argsbuf[5];
    PyObject * const *fastargs;
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t noptargs = nargs + (kwargs ? PyDict_GET_SIZE(kwargs) : 0) - 1;
    PyObject *file;
    int fix_imports = 1;
    const char *encoding = "ASCII";
    const char *errors = "strict";
    PyObject *buffers = NULL;

    fastargs = _PyArg_UnpackKeywords(_PyTuple_CAST(args)->ob_item, nargs, kwargs, NULL, &_parser, 1, 1, 0, argsbuf);
    if (!fastargs) {
        goto exit;
    }
    file = fastargs[0];
    if (!noptargs) {
        goto skip_optional_kwonly;
    }
    if (fastargs[1]) {
        fix_imports = PyObject_IsTrue(fastargs[1]);
        if (fix_imports < 0) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (fastargs[2]) {
        if (!PyUnicode_Check(fastargs[2])) {
            _PyArg_BadArgument("Unpickler", "argument 'encoding'", "str", fastargs[2]);
            goto exit;
        }
        Py_ssize_t encoding_length;
        encoding = PyUnicode_AsUTF8AndSize(fastargs[2], &encoding_length);
        if (encoding == NULL) {
            goto exit;
        }
        if (strlen(encoding) != (size_t)encoding_length) {
            PyErr_SetString(PyExc_ValueError, "embedded null character");
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (fastargs[3]) {
        if (!PyUnicode_Check(fastargs[3])) {
            _PyArg_BadArgument("Unpickler", "argument 'errors'", "str", fastargs[3]);
            goto exit;
        }
        Py_ssize_t errors_length;
        errors = PyUnicode_AsUTF8AndSize(fastargs[3], &errors_length);
        if (errors == NULL) {
            goto exit;
        }
        if (strlen(errors) != (size_t)errors_length) {
            PyErr_SetString(PyExc_ValueError, "embedded null character");
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    buffers = fastargs[4];
skip_optional_kwonly:
    return_value = _pickle_Unpickler___init___impl((UnpicklerObject *)self, file, fix_imports, encoding, errors, buffers);

exit:
    return return_value;
}

PyDoc_STRVAR(_pickle_dumps__doc__,
"dumps($module, /, obj, protocol=None, *, fix_imports=True,\n"
"      buffer_callback=None)\n"
"--\n"
"\n"
"Return the pickled representation of the object as a bytes object.\n"
"\n"
"The optional *protocol* argument tells the pickler to use the given\n"
"protocol; supported protocols are 0, 1, 2, 3, 4 and 5.  The default\n"
"protocol is 4. It was introduced in Python 3.4, and is incompatible\n"
"with previous versions.\n"
"\n"
"Specifying a negative protocol version selects the highest protocol\n"
"version supported.  The higher the protocol used, the more recent the\n"
"version of Python needed to read the pickle produced.\n"
"\n"
"If *fix_imports* is True and *protocol* is less than 3, pickle will\n"
"try to map the new Python 3 names to the old module names used in\n"
"Python 2, so that the pickle data stream is readable with Python 2.\n"
"\n"
"If *buffer_callback* is None (the default), buffer views are serialized\n"
"into *file* as part of the pickle stream.  It is an error if\n"
"*buffer_callback* is not None and *protocol* is None or smaller than 5.");

#define _PICKLE_DUMPS_METHODDEF    \
    {"dumps", (PyCFunction)(void(*)(void))_pickle_dumps, METH_FASTCALL|METH_KEYWORDS, _pickle_dumps__doc__},

static PyObject *
_pickle_dumps_impl(PyObject *module, PyObject *obj, PyObject *protocol,
                   int fix_imports, PyObject *buffer_callback);

static PyObject *
_pickle_dumps(PyObject *module, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *return_value = NULL;
    static const char * const _keywords[] = {"obj", "protocol", "fix_imports", "buffer_callback", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "dumps", 0};
    PyObject *argsbuf[4];
    Py_ssize_t noptargs = nargs + (kwnames ? PyTuple_GET_SIZE(kwnames) : 0) - 1;
    PyObject *obj;
    PyObject *protocol = Py_None;
    int fix_imports = 1;
    PyObject *buffer_callback = Py_None;

    args = _PyArg_UnpackKeywords(args, nargs, NULL, kwnames, &_parser, 1, 2, 0, argsbuf);
    if (!args) {
        goto exit;
    }
    obj = args[0];
    if (!noptargs) {
        goto skip_optional_pos;
    }
    if (args[1]) {
        protocol = args[1];
        if (!--noptargs) {
            goto skip_optional_pos;
        }
    }
skip_optional_pos:
    if (!noptargs) {
        goto skip_optional_kwonly;
    }
    if (args[2]) {
        fix_imports = PyObject_IsTrue(args[2]);
        if (fix_imports < 0) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    buffer_callback = args[3];
skip_optional_kwonly:
    return_value = _pickle_dumps_impl(module, obj, protocol, fix_imports, buffer_callback);

exit:
    return return_value;
}

PyDoc_STRVAR(_pickle_loads__doc__,
"loads($module, /, data, *, fix_imports=True, encoding=\'ASCII\',\n"
"      errors=\'strict\', buffers=())\n"
"--\n"
"\n"
"Read and return an object from the given pickle data.\n"
"\n"
"The protocol version of the pickle is detected automatically, so no\n"
"protocol argument is needed.  Bytes past the pickled object\'s\n"
"representation are ignored.\n"
"\n"
"Optional keyword arguments are *fix_imports*, *encoding* and *errors*,\n"
"which are used to control compatibility support for pickle stream\n"
"generated by Python 2.  If *fix_imports* is True, pickle will try to\n"
"map the old Python 2 names to the new names used in Python 3.  The\n"
"*encoding* and *errors* tell pickle how to decode 8-bit string\n"
"instances pickled by Python 2; these default to \'ASCII\' and \'strict\',\n"
"respectively.  The *encoding* can be \'bytes\' to read these 8-bit\n"
"string instances as bytes objects.");

#define _PICKLE_LOADS_METHODDEF    \
    {"loads", (PyCFunction)(void(*)(void))_pickle_loads, METH_FASTCALL|METH_KEYWORDS, _pickle_loads__doc__},

static PyObject *
_pickle_loads_impl(PyObject *module, PyObject *data, int fix_imports,
                   const char *encoding, const char *errors,
                   PyObject *buffers);

static PyObject *
_pickle_loads(PyObject *module, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *return_value = NULL;
    static const char * const _keywords[] = {"data", "fix_imports", "encoding", "errors", "buffers", NULL};
    static _PyArg_Parser _parser = {NULL, _keywords, "loads", 0};
    PyObject *argsbuf[5];
    Py_ssize_t noptargs = nargs + (kwnames ? PyTuple_GET_SIZE(kwnames) : 0) - 1;
    PyObject *data;
    int fix_imports = 1;
    const char *encoding = "ASCII";
    const char *errors = "strict";
    PyObject *buffers = NULL;

    args = _PyArg_UnpackKeywords(args, nargs, NULL, kwnames, &_parser, 1, 1, 0, argsbuf);
    if (!args) {
        goto exit;
    }
    data = args[0];
    if (!noptargs) {
        goto skip_optional_kwonly;
    }
    if (args[1]) {
        fix_imports = PyObject_IsTrue(args[1]);
        if (fix_imports < 0) {
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[2]) {
        if (!PyUnicode_Check(args[2])) {
            _PyArg_BadArgument("loads", "argument 'encoding'", "str", args[2]);
            goto exit;
        }
        Py_ssize_t encoding_length;
        encoding = PyUnicode_AsUTF8AndSize(args[2], &encoding_length);
        if (encoding == NULL) {
            goto exit;
        }
        if (strlen(encoding) != (size_t)encoding_length) {
            PyErr_SetString(PyExc_ValueError, "embedded null character");
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[3]) {
        if (!PyUnicode_Check(args[3])) {
            _PyArg_BadArgument("loads", "argument 'errors'", "str", args[3]);
            goto exit;
        }
        Py_ssize_t errors_length;
        errors = PyUnicode_AsUTF8AndSize(args[3], &errors_length);
        if (errors == NULL) {
            goto exit;
        }
        if (strlen(errors) != (size_t)errors_length) {
            PyErr_SetString(PyExc_ValueError, "embedded null character");
            goto exit;
        }
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    buffers = args[4];
skip_optional_kwonly:
    return_value = _pickle_loads_impl(module, data, fix_imports, encoding, errors, buffers);

exit:
    return return_value;
}
