#include <stdarg.h>
#include "Python.h"
#include "datetime.h"
#include "structmember.h"

PyDoc_STRVAR(quickle__doc__,
"`quickle` - a quicker pickle.");

#define QUICKLE_VERSION "0.4.0"

enum opcode {
    MARK             = '(',
    STOP             = '.',
    POP              = '0',
    POP_MARK         = '1',
    BININT           = 'J',
    BININT1          = 'K',
    BININT2          = 'M',
    NONE             = 'N',
    BINUNICODE       = 'X',
    APPEND           = 'a',
    EMPTY_DICT       = '}',
    APPENDS          = 'e',
    BINGET           = 'h',
    LONG_BINGET      = 'j',
    EMPTY_LIST       = ']',
    SETITEM          = 's',
    TUPLE            = 't',
    EMPTY_TUPLE      = ')',
    SETITEMS         = 'u',
    BINFLOAT         = 'G',
    TUPLE1           = '\x85',
    TUPLE2           = '\x86',
    TUPLE3           = '\x87',
    NEWTRUE          = '\x88',
    NEWFALSE         = '\x89',
    LONG1            = '\x8a',
    LONG4            = '\x8b',
    BINBYTES         = 'B',
    SHORT_BINBYTES   = 'C',
    SHORT_BINUNICODE = '\x8c',
    BINUNICODE8      = '\x8d',
    BINBYTES8        = '\x8e',
    EMPTY_SET        = '\x8f',
    ADDITEMS         = '\x90',
    FROZENSET        = '\x91',
    MEMOIZE          = '\x94',
    BYTEARRAY8       = '\x96',
    NEXT_BUFFER      = '\x97',
    READONLY_BUFFER  = '\x98',

    /* Quickle only */
    BUILDSTRUCT      = '\xb0',
    STRUCT1          = '\xb1',
    STRUCT2          = '\xb2',
    STRUCT4          = '\xb3',
    ENUM1            = '\xb4',
    ENUM2            = '\xb5',
    ENUM4            = '\xb6',
    COMPLEX          = '\xb7',
    TIMEDELTA        = '\xb8',
    DATE             = '\xb9',
    TIME             = '\xba',
    DATETIME         = '\xbb',
    TIME_TZ          = '\xbc',
    DATETIME_TZ      = '\xbd',
    TIMEZONE_UTC     = '\xbe',
    TIMEZONE         = '\xbf',
    ZONEINFO         = '\xc0',

    /* Unused, but kept for compt with pickle */
    PROTO            = '\x80',
    FRAME            = '\x95',
};

enum {
   /* Number of elements save_list/dict/set writes out before
    * doing APPENDS/SETITEMS/ADDITEMS. */
    BATCHSIZE = 1000,
};

/*************************************************************************
 * Module level state                                                    *
 *************************************************************************/

/* State of the quickle module */
typedef struct {
    PyObject *QuickleError;
    PyObject *EncodingError;
    PyObject *DecodingError;
    PyObject *StructType;
    PyTypeObject *EnumType;
    PyTypeObject *TimeZoneType;
    PyTypeObject *ZoneInfoType;
    PyObject *encoder_dumps_kws;
    PyObject *decoder_loads_kws;
    PyObject *value2member_map_str;
    PyObject *name_str;
} QuickleState;

/* Forward declaration of the quickle module definition. */
static struct PyModuleDef quicklemodule;

/* Given a module object, get its per-module state. */
static QuickleState *
quickle_get_state(PyObject *module)
{
    return (QuickleState *)PyModule_GetState(module);
}

/* Find the module instance imported in the currently running sub-interpreter
   and get its state. */
static QuickleState *
quickle_get_global_state()
{
    return quickle_get_state(PyState_FindModule(&quicklemodule));
}

/*************************************************************************
 * Parsing utilities                                                     *
 *************************************************************************/

static PyObject*
make_keyword_tuple(char *const *kwnames) {
    Py_ssize_t nkwargs = 0, i;
    PyObject *kw = NULL, *out = NULL;

    while (kwnames[nkwargs] != NULL)
        nkwargs++;

    out = PyTuple_New(nkwargs);
    if (out == NULL)
        return NULL;

    for (i = 0; i < nkwargs; i++) {
        kw = PyUnicode_InternFromString(kwnames[i]);
        if (kw == NULL) {
            Py_DECREF(out);
            return NULL;
        }
        PyTuple_SET_ITEM(out, i, kw);
    }
    return out;
}

static PyObject*
find_keyword(PyObject *kwnames, PyObject *const *kwstack, PyObject *key)
{
    Py_ssize_t i, nkwargs;

    nkwargs = PyTuple_GET_SIZE(kwnames);
    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);

        /* kwname == key will normally find a match in since keyword keys
           should be interned strings; if not retry below in a new loop. */
        if (kwname == key) {
            return kwstack[i];
        }
    }

    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);
        assert(PyUnicode_Check(kwname));
        if (_PyUnicode_EQ(kwname, key)) {
            return kwstack[i];
        }
    }
    return NULL;
}

static int
check_positional_nargs(Py_ssize_t nargs, Py_ssize_t min, Py_ssize_t max) {
    if (nargs > max) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra positional arguments provided"
        );
        return 0;
    }
    else if (nargs < min) {
        PyErr_Format(
            PyExc_TypeError,
            "Missing %zd required arguments",
            min - nargs
        );
        return 0;
    }
    return 1;
}

static int
parse_keywords(PyObject *kw_keys, PyObject *const *kw_values, PyObject *expected_kws, ...) {
    va_list targets;
    Py_ssize_t i, n_expected, n_provided;
    PyObject *key, *val, **target;

    if (kw_keys == NULL)
        return 1;

    va_start(targets, expected_kws);

    n_expected = PyTuple_GET_SIZE(expected_kws);
    n_provided = PyTuple_GET_SIZE(kw_keys);

    for (i = 0; i < n_expected; i++) {
        key = PyTuple_GET_ITEM(expected_kws, i);
        val = find_keyword(kw_keys, kw_values, key);
        target = va_arg(targets, PyObject **);
        if (val != NULL) {
            *target = val;
            n_provided--;
        }
    }

    va_end(targets);

    if (n_provided > 0) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra keyword arguments provided"
        );
        return 0;
    }
    return 1;
}

/*************************************************************************
 * Struct Types                                                          *
 *************************************************************************/

typedef struct {
    PyHeapTypeObject base;
    PyObject *struct_fields;
    PyObject *struct_defaults;
    Py_ssize_t *struct_offsets;
} StructMetaObject;

static PyTypeObject StructMetaType;
static PyTypeObject StructMixinType;

#define StructMeta_GET_FIELDS(s) (((StructMetaObject *)(s))->struct_fields);
#define StructMeta_GET_NFIELDS(s) (PyTuple_GET_SIZE((((StructMetaObject *)(s))->struct_fields)));
#define StructMeta_GET_DEFAULTS(s) (((StructMetaObject *)(s))->struct_defaults);
#define StructMeta_GET_OFFSETS(s) (((StructMetaObject *)(s))->struct_offsets);

static int
dict_discard(PyObject *dict, PyObject *key) {
    int status = PyDict_Contains(dict, key);
    if (status < 0)
        return status;
    return (status == 1) ? PyDict_DelItem(dict, key) : 0;
}

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames);

static PyObject *
StructMeta_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    StructMetaObject *cls = NULL;
    PyObject *name = NULL, *bases = NULL, *orig_dict = NULL;
    PyObject *arg_fields = NULL, *kwarg_fields = NULL, *new_dict = NULL, *new_args = NULL;
    PyObject *fields = NULL, *defaults = NULL, *offsets_lk = NULL, *offset = NULL, *slots = NULL, *slots_list = NULL;
    PyObject *base, *base_fields, *base_defaults, *annotations;
    PyObject *default_val, *field;
    Py_ssize_t nfields, ndefaults, i, j, k;
    Py_ssize_t *offsets = NULL, *base_offsets;

    /* Parse arguments: (name, bases, dict) */
    if (!PyArg_ParseTuple(args, "UO!O!:StructMeta.__new__", &name, &PyTuple_Type,
                          &bases, &PyDict_Type, &orig_dict))
        return NULL;

    if (PyDict_GetItemString(orig_dict, "__init__") != NULL) {
        PyErr_SetString(PyExc_TypeError, "Struct types cannot define __init__");
        return NULL;
    }
    if (PyDict_GetItemString(orig_dict, "__new__") != NULL) {
        PyErr_SetString(PyExc_TypeError, "Struct types cannot define __new__");
        return NULL;
    }
    if (PyDict_GetItemString(orig_dict, "__slots__") != NULL) {
        PyErr_SetString(PyExc_TypeError, "Struct types cannot define __slots__");
        return NULL;
    }

    arg_fields = PyDict_New();
    if (arg_fields == NULL)
        goto error;
    kwarg_fields = PyDict_New();
    if (kwarg_fields == NULL)
        goto error;
    offsets_lk = PyDict_New();
    if (offsets_lk == NULL)
        goto error;

    for (i = PyTuple_GET_SIZE(bases) - 1; i >= 0; i--) {
        base = PyTuple_GET_ITEM(bases, i);
        if ((PyTypeObject *)base == &StructMixinType) {
            continue;
        }

        if (!(PyType_Check(base) && (Py_TYPE(base) == &StructMetaType))) {
            PyErr_SetString(
                PyExc_TypeError,
                "All base classes must be subclasses of quickle.Struct"
            );
            goto error;
        }
        base_fields = StructMeta_GET_FIELDS(base);
        base_defaults = StructMeta_GET_DEFAULTS(base);
        base_offsets = StructMeta_GET_OFFSETS(base);
        nfields = PyTuple_GET_SIZE(base_fields);
        ndefaults = PyTuple_GET_SIZE(base_defaults);
        for (j = 0; j < nfields; j++) {
            field = PyTuple_GET_ITEM(base_fields, j);
            if (j < (nfields - ndefaults)) {
                if (PyDict_SetItem(arg_fields, field, Py_None) < 0)
                    goto error;
                if (dict_discard(kwarg_fields, field) < 0)
                    goto error;
            }
            else {
                default_val = PyTuple_GET_ITEM(base_defaults, (j + ndefaults - nfields));
                if (PyDict_SetItem(kwarg_fields, field, default_val) < 0)
                    goto error;
                if (dict_discard(arg_fields, field) < 0)
                    goto error;
            }
            offset = PyLong_FromSsize_t(base_offsets[j]);
            if (offset == NULL)
                goto error;
            if (PyDict_SetItem(offsets_lk, field, offset) < 0)
                goto error;
            Py_DECREF(offset);
        }
    }

    new_dict = PyDict_Copy(orig_dict);
    if (new_dict == NULL)
        goto error;
    slots_list = PyList_New(0);
    if (slots_list == NULL)
        goto error;

    annotations = PyDict_GetItemString(orig_dict, "__annotations__");
    if (annotations != NULL) {
        if (!PyDict_Check(annotations)) {
            PyErr_SetString(PyExc_TypeError, "__annotations__ must be a dict");
            goto error;
        }

        i = 0;
        while (PyDict_Next(annotations, &i, &field, NULL)) {
            if (!PyUnicode_CheckExact(field)) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "__annotations__ keys must be strings"
                );
                goto error;
            }

            /* If the field is new, add it to slots */
            if (PyDict_GetItem(arg_fields, field) == NULL && PyDict_GetItem(kwarg_fields, field) == NULL) {
                if (PyList_Append(slots_list, field) < 0)
                    goto error;
            }

            default_val = PyDict_GetItem(new_dict, field);
            if (default_val != NULL) {
                if (dict_discard(arg_fields, field) < 0)
                    goto error;
                if (PyDict_SetItem(kwarg_fields, field, default_val) < 0)
                    goto error;
                if (dict_discard(new_dict, field) < 0)
                    goto error;
            }
            else {
                if (dict_discard(kwarg_fields, field) < 0)
                    goto error;
                if (PyDict_SetItem(arg_fields, field, Py_None) < 0)
                    goto error;
            }
        }
    }

    fields = PyTuple_New(PyDict_Size(arg_fields) + PyDict_Size(kwarg_fields));
    if (fields == NULL)
        goto error;
    defaults = PyTuple_New(PyDict_Size(kwarg_fields));

    i = 0;
    j = 0;
    while (PyDict_Next(arg_fields, &i, &field, NULL)) {
        Py_INCREF(field);
        PyTuple_SET_ITEM(fields, j, field);
        j++;
    }
    i = 0;
    k = 0;
    while (PyDict_Next(kwarg_fields, &i, &field, &default_val)) {
        Py_INCREF(field);
        PyTuple_SET_ITEM(fields, j, field);
        Py_INCREF(default_val);
        PyTuple_SET_ITEM(defaults, k, default_val);
        j++;
        k++;
    }
    Py_CLEAR(arg_fields);
    Py_CLEAR(kwarg_fields);

    if (PyList_Sort(slots_list) < 0)
        goto error;

    slots = PyList_AsTuple(slots_list);
    if (slots == NULL)
        goto error;
    Py_CLEAR(slots_list);

    if (PyDict_SetItemString(new_dict, "__slots__", slots) < 0)
        goto error;
    Py_CLEAR(slots);

    new_args = Py_BuildValue("(OOO)", name, bases, new_dict);
    if (new_args == NULL)
        goto error;

    cls = (StructMetaObject *) PyType_Type.tp_new(type, new_args, kwargs);
    if (cls == NULL)
        goto error;
    ((PyTypeObject *)cls)->tp_vectorcall = (vectorcallfunc)Struct_vectorcall;
    Py_CLEAR(new_args);

    PyMemberDef *mp = PyHeapType_GET_MEMBERS(cls);
    for (i = 0; i < Py_SIZE(cls); i++, mp++) {
        offset = PyLong_FromSsize_t(mp->offset);
        if (offset == NULL)
            goto error;
        if (PyDict_SetItemString(offsets_lk, mp->name, offset) < 0)
            goto error;
    }
    offsets = PyMem_New(Py_ssize_t, PyTuple_GET_SIZE(fields));
    if (offsets == NULL)
        goto error;
    for (i = 0; i < PyTuple_GET_SIZE(fields); i++) {
        field = PyTuple_GET_ITEM(fields, i);
        offset = PyDict_GetItem(offsets_lk, field);
        if (offset == NULL) {
            PyErr_Format(PyExc_RuntimeError, "Failed to get offset for %R", field);
            goto error;
        }
        offsets[i] = PyLong_AsSsize_t(offset);
    }
    Py_CLEAR(offsets_lk);

    cls->struct_fields = fields;
    cls->struct_defaults = defaults;
    cls->struct_offsets = offsets;
    return (PyObject *) cls;
error:
    Py_XDECREF(arg_fields);
    Py_XDECREF(kwarg_fields);
    Py_XDECREF(fields);
    Py_XDECREF(defaults);
    Py_XDECREF(new_dict);
    Py_XDECREF(slots_list);
    Py_XDECREF(new_args);
    Py_XDECREF(offsets_lk);
    Py_XDECREF(offset);
    if (offsets != NULL)
        PyMem_Free(offsets);
    return NULL;
}

static int
StructMeta_traverse(StructMetaObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->struct_fields);
    Py_VISIT(self->struct_defaults);
    return PyType_Type.tp_traverse((PyObject *)self, visit, arg);
}

static int
StructMeta_clear(StructMetaObject *self)
{
    Py_CLEAR(self->struct_fields);
    Py_CLEAR(self->struct_defaults);
    PyMem_Free(self->struct_offsets);
    return PyType_Type.tp_clear((PyObject *)self);
}

static void
StructMeta_dealloc(StructMetaObject *self)
{
    Py_XDECREF(self->struct_fields);
    Py_XDECREF(self->struct_defaults);
    PyType_Type.tp_dealloc((PyObject *)self);
}

static PyObject*
StructMeta_signature(StructMetaObject *self, void *closure)
{
    Py_ssize_t nfields, ndefaults, npos, i;
    PyObject *res = NULL;
    PyObject *inspect = NULL;
    PyObject *parameter_cls = NULL;
    PyObject *parameter_empty = NULL;
    PyObject *parameter_kind = NULL;
    PyObject *signature_cls = NULL;
    PyObject *typing = NULL;
    PyObject *get_type_hints = NULL;
    PyObject *annotations = NULL;
    PyObject *parameters = NULL;
    PyObject *temp_args = NULL, *temp_kwargs = NULL;
    PyObject *field, *default_val, *parameter, *annotation;

    nfields = PyTuple_GET_SIZE(self->struct_fields);
    ndefaults = PyTuple_GET_SIZE(self->struct_defaults);
    npos = nfields - ndefaults;

    inspect = PyImport_ImportModule("inspect");
    if (inspect == NULL)
        goto cleanup;
    parameter_cls = PyObject_GetAttrString(inspect, "Parameter");
    if (parameter_cls == NULL)
        goto cleanup;
    parameter_empty = PyObject_GetAttrString(parameter_cls, "empty");
    if (parameter_empty == NULL)
        goto cleanup;
    parameter_kind = PyObject_GetAttrString(parameter_cls, "POSITIONAL_OR_KEYWORD");
    if (parameter_kind == NULL)
        goto cleanup;
    signature_cls = PyObject_GetAttrString(inspect, "Signature");
    if (signature_cls == NULL)
        goto cleanup;
    typing = PyImport_ImportModule("typing");
    if (typing == NULL)
        goto cleanup;
    get_type_hints = PyObject_GetAttrString(typing, "get_type_hints");
    if (get_type_hints == NULL)
        goto cleanup;

    annotations = PyObject_CallFunctionObjArgs(get_type_hints, self, NULL);
    if (annotations == NULL)
        goto cleanup;

    parameters = PyList_New(nfields);
    if (parameters == NULL)
        return NULL;

    temp_args = PyTuple_New(0);
    if (temp_args == NULL)
        goto cleanup;
    temp_kwargs = PyDict_New();
    if (temp_kwargs == NULL)
        goto cleanup;
    if (PyDict_SetItemString(temp_kwargs, "kind", parameter_kind) < 0)
        goto cleanup;

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(self->struct_fields, i);
        if (i < npos) {
            default_val = parameter_empty;
        } else {
            default_val = PyTuple_GET_ITEM(self->struct_defaults, i - npos);
        }
        annotation = PyDict_GetItem(annotations, field);
        if (annotation == NULL) {
            annotation = parameter_empty;
        }
        if (PyDict_SetItemString(temp_kwargs, "name", field) < 0)
            goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "default", default_val) < 0)
            goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "annotation", annotation) < 0)
            goto cleanup;
        parameter = PyObject_Call(parameter_cls, temp_args, temp_kwargs);
        if (parameter == NULL)
            goto cleanup;
        PyList_SET_ITEM(parameters, i, parameter);
    }
    res = PyObject_CallFunctionObjArgs(signature_cls, parameters, NULL);
cleanup:
    Py_XDECREF(inspect);
    Py_XDECREF(parameter_cls);
    Py_XDECREF(parameter_empty);
    Py_XDECREF(parameter_kind);
    Py_XDECREF(signature_cls);
    Py_XDECREF(typing);
    Py_XDECREF(get_type_hints);
    Py_XDECREF(annotations);
    Py_XDECREF(parameters);
    Py_XDECREF(temp_args);
    Py_XDECREF(temp_kwargs);
    return res;
}

static PyMemberDef StructMeta_members[] = {
    {"__struct_fields__", T_OBJECT_EX, offsetof(StructMetaObject, struct_fields), READONLY, "Struct fields"},
    {"__struct_defaults__", T_OBJECT_EX, offsetof(StructMetaObject, struct_defaults), READONLY, "Struct defaults"},
    {NULL},
};

static PyGetSetDef StructMeta_getset[] = {
    {"__signature__", (getter) StructMeta_signature, NULL, NULL, NULL},
    {NULL},
};

static PyTypeObject StructMetaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "quickle.StructMeta",
    .tp_basicsize = sizeof(StructMetaObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_TYPE_SUBCLASS | Py_TPFLAGS_HAVE_GC | _Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_new = StructMeta_new,
    .tp_dealloc = (destructor) StructMeta_dealloc,
    .tp_clear = (inquiry) StructMeta_clear,
    .tp_traverse = (traverseproc) StructMeta_traverse,
    .tp_members = StructMeta_members,
    .tp_getset = StructMeta_getset,
    .tp_call = PyVectorcall_Call,
    .tp_vectorcall_offset = offsetof(PyTypeObject, tp_vectorcall),
};


static PyObject *
maybe_deepcopy_default(PyObject *obj, int *is_copy) {
    QuickleState *st;
    PyObject *copy = NULL, *deepcopy = NULL, *res = NULL;
    PyTypeObject *type = Py_TYPE(obj);

    /* Known non-collection types */
    if (obj == Py_None || obj == Py_False || obj == Py_True ||
        type == &PyLong_Type || type == &PyFloat_Type ||
        type == &PyComplex_Type || type == &PyBytes_Type ||
        type == &PyUnicode_Type || type == &PyByteArray_Type ||
        type == &PyPickleBuffer_Type
    ) {
        return obj;
    }
    else if (type == &PyTuple_Type && (PyTuple_GET_SIZE(obj) == 0)) {
        return obj;
    }
    else if (type == &PyFrozenSet_Type && PySet_GET_SIZE(obj) == 0) {
        return obj;
    }
    else if (type == PyDateTimeAPI->DeltaType ||
             type == PyDateTimeAPI->DateTimeType ||
             type == PyDateTimeAPI->DateType ||
             type == PyDateTimeAPI->TimeType
    ) {
        return obj;
    }
    st = quickle_get_global_state();
    if (type == st->TimeZoneType || type == st->ZoneInfoType) {
        return obj;
    }
    else if (PyType_IsSubtype(type, st->EnumType)) {
        return obj;
    }

    if (is_copy != NULL)
        *is_copy = 1;

    /* Fast paths for known empty collections */
    if (type == &PyDict_Type && PyDict_Size(obj) == 0) {
        return PyDict_New();
    }
    else if (type == &PyList_Type && PyList_GET_SIZE(obj) == 0) {
        return PyList_New(0);
    }
    else if (type == &PySet_Type && PySet_GET_SIZE(obj) == 0) {
        return PySet_New(NULL);
    }
    /* More complicated, invoke full deepcopy */
    copy = PyImport_ImportModule("copy");
    if (copy == NULL)
        goto cleanup;
    deepcopy = PyObject_GetAttrString(copy, "deepcopy");
    if (deepcopy == NULL)
        goto cleanup;
    res = PyObject_CallFunctionObjArgs(deepcopy, obj, NULL);
cleanup:
    Py_XDECREF(copy);
    Py_XDECREF(deepcopy);
    return res;
}

#if PY_VERSION_HEX < 0x03090000
#define IS_TRACKED _PyObject_GC_IS_TRACKED
#define CALL_ONE_ARG(fn, arg) PyObject_CallFunctionObjArgs((fn), (arg), NULL)
#else
#define IS_TRACKED  PyObject_GC_IsTracked
#define CALL_ONE_ARG(fn, arg) PyObject_CallOneArg((fn), (arg))
#endif
/* Is this object something that is/could be GC tracked? True if
 * - the value supports GC
 * - the value isn't a tuple or the object is tracked (skip tracked checks for non-tuples)
 */
#define OBJ_IS_GC(x) \
    (PyType_IS_GC(Py_TYPE(x)) && \
     (!PyTuple_CheckExact(x) || IS_TRACKED(x)))

/* Set field #index on obj. Steals a reference to val */
static inline void
Struct_set_index(PyObject *obj, Py_ssize_t index, PyObject *val) {
    StructMetaObject *cls;
    char *addr;
    PyObject *old;

    cls = (StructMetaObject *)Py_TYPE(obj);
    addr = (char *)obj + cls->struct_offsets[index];
    old = *(PyObject **)addr;
    Py_XDECREF(old);
    *(PyObject **)addr = val;
}

/* Get field #index on obj. Returns a borrowed reference */
static inline PyObject*
Struct_get_index(PyObject *obj, Py_ssize_t index) {
    StructMetaObject *cls;
    char *addr;
    PyObject *val;

    cls = (StructMetaObject *)Py_TYPE(obj);
    addr = (char *)obj + cls->struct_offsets[index];
    val = *(PyObject **)addr;
    if (val == NULL)
        PyErr_Format(PyExc_AttributeError,
                     "Struct field %R is unset",
                     PyTuple_GET_ITEM(cls->struct_fields, index));
    return val;
}

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PyObject *self, *fields, *defaults, *field, *val;
    Py_ssize_t nargs, nkwargs, nfields, ndefaults, npos, i;
    int is_copy, should_untrack;

    self = cls->tp_alloc(cls, 0);
    if (self == NULL)
        return NULL;

    fields = StructMeta_GET_FIELDS(Py_TYPE(self));
    defaults = StructMeta_GET_DEFAULTS(Py_TYPE(self));

    nargs = PyVectorcall_NARGS(nargsf);
    nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);
    ndefaults = PyTuple_GET_SIZE(defaults);
    nfields = PyTuple_GET_SIZE(fields);
    npos = nfields - ndefaults;

    if (nargs > nfields) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra positional arguments provided"
        );
        return NULL;
    }

    should_untrack = PyObject_IS_GC(self);

    for (i = 0; i < nfields; i++) {
        is_copy = 0;
        field = PyTuple_GET_ITEM(fields, i);
        val = (nkwargs == 0) ? NULL : find_keyword(kwnames, args + nargs, field);
        if (val != NULL) {
            if (i < nargs) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Argument '%U' given by name and position",
                    field
                );
                return NULL;
            }
            nkwargs -= 1;
        }
        else if (i < nargs) {
            val = args[i];
        }
        else if (i < npos) {
            PyErr_Format(
                PyExc_TypeError,
                "Missing required argument '%U'",
                field
            );
            return NULL;
        }
        else {
            val = maybe_deepcopy_default(PyTuple_GET_ITEM(defaults, i - npos), &is_copy);
            if (val == NULL)
                return NULL;
        }
        Struct_set_index(self, i, val);
        if (!is_copy)
            Py_INCREF(val);
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(val);
        }
    }
    if (nkwargs > 0) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra keyword arguments provided"
        );
        return NULL;
    }
    if (should_untrack)
        PyObject_GC_UnTrack(self);
    return self;
}

static int
Struct_setattro(PyObject *self, PyObject *key, PyObject *value) {
    if (PyObject_GenericSetAttr(self, key, value) < 0)
        return -1;
    if (value != NULL && OBJ_IS_GC(value) && !IS_TRACKED(self))
        PyObject_GC_Track(self);
    return 0;
}

static PyObject *
Struct_repr(PyObject *self) {
    int recursive;
    Py_ssize_t nfields, i;
    PyObject *parts = NULL, *empty = NULL, *out = NULL;
    PyObject *part, *fields, *field, *val;

    recursive = Py_ReprEnter(self);
    if (recursive != 0) {
        out = (recursive < 0) ? NULL : PyUnicode_FromString("...");
        goto cleanup;
    }

    fields = StructMeta_GET_FIELDS(Py_TYPE(self));
    nfields = PyTuple_GET_SIZE(fields);
    if (nfields == 0) {
        out = PyUnicode_FromFormat("%s()", Py_TYPE(self)->tp_name);
        goto cleanup;
    }

    parts = PyList_New(nfields + 1);
    if (parts == NULL)
        goto cleanup;

    part = PyUnicode_FromFormat("%s(", Py_TYPE(self)->tp_name);
    if (part == NULL)
        goto cleanup;
    PyList_SET_ITEM(parts, 0, part);

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(fields, i);
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto cleanup;

        if (i == (nfields - 1)) {
            part = PyUnicode_FromFormat("%U=%R)", field, val);
        } else {
            part = PyUnicode_FromFormat("%U=%R, ", field, val);
        }
        if (part == NULL)
            goto cleanup;
        PyList_SET_ITEM(parts, i + 1, part);
    }
    empty = PyUnicode_FromString("");
    if (empty == NULL)
        goto cleanup;
    out = PyUnicode_Join(empty, parts);

cleanup:
    Py_XDECREF(parts);
    Py_XDECREF(empty);
    Py_ReprLeave(self);
    return out;
}

static PyObject *
Struct_richcompare(PyObject *self, PyObject *other, int op) {
    int status;
    PyObject *left, *right;
    Py_ssize_t nfields, i;

    if (!(Py_TYPE(Py_TYPE(other)) == &StructMetaType)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    status = Py_TYPE(self) == Py_TYPE(other);
    if (status == 0)
        goto done;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));

    for (i = 0; i < nfields; i++) {
        left = Struct_get_index(self, i);
        if (left == NULL)
            return NULL;
        right = Struct_get_index(other, i);
        if (right == NULL)
            return NULL;
        Py_INCREF(left);
        Py_INCREF(right);
        status = PyObject_RichCompareBool(left, right, Py_EQ);
        Py_DECREF(left);
        Py_DECREF(right);
        if (status < 0)
            return NULL;
        if (status == 0)
            goto done;
    }
done:
    if (status == ((op == Py_EQ) ? 1 : 0)) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

static PyObject *
Struct_copy(PyObject *self, PyObject *args)
{
    Py_ssize_t i, nfields;
    PyObject *val, *res = NULL;

    res = Py_TYPE(self)->tp_alloc(Py_TYPE(self), 0);
    if (res == NULL)
        return NULL;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));
    for (i = 0; i < nfields; i++) {
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto error;
        Py_INCREF(val);
        Struct_set_index(res, i, val);
    }
    /* If self is untracked, then copy is untracked */
    if (PyObject_IS_GC(self) && !IS_TRACKED(self))
        PyObject_GC_UnTrack(res);
    return res;
error:
    Py_DECREF(res);
    return NULL;
}

static PyObject *
Struct_reduce(PyObject *self, PyObject *args)
{
    Py_ssize_t i, nfields;
    PyObject *values, *val;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));
    values = PyTuple_New(nfields);
    if (values == NULL)
        return NULL;
    for (i = 0; i < nfields; i++) {
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto error;
        Py_INCREF(val);
        PyTuple_SET_ITEM(values, i, val);
    }
    return PyTuple_Pack(2, Py_TYPE(self), values);
error:
    Py_XDECREF(values);
    return NULL;
}

static PyObject *
StructMixin_fields(PyObject *self, void *closure) {
    PyObject *out;
    out = StructMeta_GET_FIELDS(Py_TYPE(self));
    Py_INCREF(out);
    return out;
}

static PyObject *
StructMixin_defaults(PyObject *self, void *closure) {
    PyObject *out;
    out = StructMeta_GET_DEFAULTS(Py_TYPE(self));
    Py_INCREF(out);
    return out;
}

static PyMethodDef Struct_methods[] = {
    {"__copy__", Struct_copy, METH_NOARGS, "copy a struct"},
    {"__reduce__", Struct_reduce, METH_NOARGS, "reduce a struct"},
    {NULL, NULL},
};

static PyGetSetDef StructMixin_getset[] = {
    {"__struct_fields__", (getter) StructMixin_fields, NULL, "Struct fields", NULL},
    {"__struct_defaults__", (getter) StructMixin_defaults, NULL, "Struct defaults", NULL},
    {NULL},
};

static PyTypeObject StructMixinType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "quickle._StructMixin",
    .tp_basicsize = 0,
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_setattro = Struct_setattro,
    .tp_repr = Struct_repr,
    .tp_richcompare = Struct_richcompare,
    .tp_methods = Struct_methods,
    .tp_getset = StructMixin_getset,
};

PyDoc_STRVAR(Struct__doc__,
"A base class for defining efficient serializable objects.\n"
"\n"
"Fields are defined using type annotations. Fields may optionally have\n"
"default values, which result in keyword parameters to the constructor.\n"
"Note that mutable default values are deepcopied in the constructor to\n"
"prevent accidental sharing.\n"
"\n"
"Structs automatically define ``__init__``, ``__eq__``, ``__repr__``, and\n"
"``__copy__`` methods. Additional methods can be defined on the class as\n"
"needed. Note that ``__init__``/``__new__`` cannot be overridden, but other\n"
"methods can. A tuple of the field names is available on the class via the\n"
"``__struct_fields__`` attribute if needed.\n"
"\n"
"Examples\n"
"--------\n"
"Here we define a new `Struct` type for describing a dog. It has three fields;\n"
"two required and one optional.\n"
"\n"
">>> class Dog(Struct):\n"
"...     name: str\n"
"...     breed: str\n"
"...     is_good_boy: bool = True\n"
"...\n"
">>> Dog('snickers', breed='corgi')\n"
"Dog(name='snickers', breed='corgi', is_good_boy=True)\n"
"\n"
"To serialize or deserialize `Struct` types, they need to be registered with\n"
"an `Encoder` and `Decoder` through the ``registry`` argument.\n"
"\n"
">>> enc = Encoder(registry=[Dog])\n"
">>> dec = Decoder(registry=[Dog])\n"
">>> data = enc.dumps(Dog('snickers', 'corgi'))\n"
">>> dec.loads(data)\n"
"Dog(name='snickers', breed='corgi', is_good_boy=True)\n"
);

/*************************************************************************
 * LookupTable object                                                    *
 *************************************************************************
 * A custom hashtable mapping PyObject * to Py_ssize_t. This is used by the
 * encoder for both memoization and type registry. Using a custom hashtable
 * rather than PyDict allows us to skip a bunch of unnecessary object creation.
 * This makes a huge performance difference. */
typedef struct {
    PyObject *key;
    Py_ssize_t value;
} LookupEntry;

typedef struct {
    size_t mask;
    size_t used;
    size_t allocated;
    size_t buffered_size;
    LookupEntry *table;
} LookupTable;

#define LT_MINSIZE 8
#define PERTURB_SHIFT 5

static LookupTable *
LookupTable_New(Py_ssize_t buffered_size)
{
    LookupTable *self = PyMem_Malloc(sizeof(LookupTable));
    if (self == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    self->buffered_size = LT_MINSIZE;
    if (buffered_size > 0) {
        /* Find the smallest valid table size >= buffered_size. */
        while (self->buffered_size < (size_t)buffered_size) {
            self->buffered_size <<= 1;
        }
    }
    self->used = 0;
    self->allocated = LT_MINSIZE;
    self->mask = LT_MINSIZE - 1;
    self->table = PyMem_Malloc(LT_MINSIZE * sizeof(LookupEntry));
    if (self->table == NULL) {
        PyMem_Free(self);
        PyErr_NoMemory();
        return NULL;
    }
    memset(self->table, 0, LT_MINSIZE * sizeof(LookupEntry));

    return self;
}

static Py_ssize_t
LookupTable_Size(LookupTable *self)
{
    return self->used;
}

static int
LookupTable_Traverse(LookupTable *self, visitproc visit, void *arg)
{
    Py_ssize_t i = self->allocated;
    while (--i >= 0) {
        Py_VISIT(self->table[i].key);
    }
    return 0;
}

static int
LookupTable_Clear(LookupTable *self)
{
    Py_ssize_t i = self->allocated;

    while (--i >= 0) {
        Py_XDECREF(self->table[i].key);
    }
    self->used = 0;
    memset(self->table, 0, self->allocated * sizeof(LookupEntry));
    return 0;
}

static void
LookupTable_Del(LookupTable *self)
{
    LookupTable_Clear(self);
    PyMem_Free(self->table);
    PyMem_Free(self);
}

/* Since entries cannot be deleted from this hashtable, _LookupTable_Lookup()
   can be considerably simpler than dictobject.c's lookdict(). */
static LookupEntry *
_LookupTable_Lookup(LookupTable *self, PyObject *key)
{
    size_t i;
    size_t perturb;
    size_t mask = self->mask;
    LookupEntry *table = self->table;
    LookupEntry *entry;
    Py_hash_t hash = (Py_hash_t)key >> 3;

    i = hash & mask;
    entry = &table[i];
    if (entry->key == NULL || entry->key == key)
        return entry;

    for (perturb = hash; ; perturb >>= PERTURB_SHIFT) {
        i = (i << 2) + i + perturb + 1;
        entry = &table[i & mask];
        if (entry->key == NULL || entry->key == key)
            return entry;
    }
    Py_UNREACHABLE();
}

/* Returns -1 on failure, 0 on success. */
static int
_LookupTable_Resize(LookupTable *self, size_t min_size)
{
    LookupEntry *oldtable = NULL;
    LookupEntry *oldentry, *newentry;
    size_t new_size = LT_MINSIZE;
    size_t to_process;

    assert(min_size > 0);

    if (min_size > PY_SSIZE_T_MAX) {
        PyErr_NoMemory();
        return -1;
    }

    /* Find the smallest valid table size >= min_size. */
    while (new_size < min_size) {
        new_size <<= 1;
    }
    /* new_size needs to be a power of two. */
    assert((new_size & (new_size - 1)) == 0);

    /* Allocate new table. */
    oldtable = self->table;
    self->table = PyMem_NEW(LookupEntry, new_size);
    if (self->table == NULL) {
        self->table = oldtable;
        PyErr_NoMemory();
        return -1;
    }
    self->allocated = new_size;
    self->mask = new_size - 1;
    memset(self->table, 0, sizeof(LookupEntry) * new_size);

    /* Copy entries from the old table. */
    to_process = self->used;
    for (oldentry = oldtable; to_process > 0; oldentry++) {
        if (oldentry->key != NULL) {
            to_process--;
            /* newentry is a pointer to a chunk of the new
               table, so we're setting the key:value pair
               in-place. */
            newentry = _LookupTable_Lookup(self, oldentry->key);
            newentry->key = oldentry->key;
            newentry->value = oldentry->value;
        }
    }

    /* Deallocate the old table. */
    PyMem_Free(oldtable);
    return 0;
}

static int
LookupTable_Reset(LookupTable *self)
{
    LookupTable_Clear(self);
    if (self->allocated > self->buffered_size) {
        return _LookupTable_Resize(self, self->buffered_size);
    }
    return 0;
}

/* Returns -1 on failure, a value otherwise. */
static Py_ssize_t
LookupTable_Get(LookupTable *self, PyObject *key)
{
    LookupEntry *entry = _LookupTable_Lookup(self, key);
    if (entry->key == NULL)
        return -1;
    return entry->value;
}
#define MEMO_GET(self, obj) \
    ((self)->active_memoize ? LookupTable_Get((self)->memo, (obj)) : -1)

/* Returns -1 on failure, 0 on success. */
static int
LookupTable_Set(LookupTable *self, PyObject *key, Py_ssize_t value)
{
    LookupEntry *entry;

    assert(key != NULL);

    entry = _LookupTable_Lookup(self, key);
    if (entry->key != NULL) {
        entry->value = value;
        return 0;
    }
    Py_INCREF(key);
    entry->key = key;
    entry->value = value;
    self->used++;

    /* If we added a key, we can safely resize. Otherwise just return!
     * If used >= 2/3 size, adjust size. Normally, this quaduples the size.
     *
     * Quadrupling the size improves average table sparseness
     * (reducing collisions) at the cost of some memory. It also halves
     * the number of expensive resize operations in a growing memo table.
     *
     * Very large memo tables (over 50K items) use doubling instead.
     * This may help applications with severe memory constraints.
     */
    if (SIZE_MAX / 3 >= self->used && self->used * 3 < self->allocated * 2) {
        return 0;
    }
    // self->used is always < PY_SSIZE_T_MAX, so this can't overflow.
    size_t desired_size = (self->used > 50000 ? 2 : 4) * self->used;
    return _LookupTable_Resize(self, desired_size);
}

#undef MT_MINSIZE
#undef PERTURB_SHIFT

/*************************************************************************
 * Encoder object                                                        *
 *************************************************************************/
typedef struct EncoderObject {
    PyObject_HEAD
    /* Configuration */
    Py_ssize_t write_buffer_size;
    LookupTable *registry;
    int collect_buffers;

    /* Per-dumps state */
    int active_collect_buffers;
    int memoize;
    int active_memoize;
    PyObject *buffers;
    LookupTable *memo;            /* Memo table, keep track of the seen
                                   objects to support self-referential objects */
    PyObject *output_buffer;    /* Write into a local bytearray buffer before
                                   flushing to the stream. */
    Py_ssize_t output_len;      /* Length of output_buffer. */
    Py_ssize_t max_output_len;  /* Allocation size of output_buffer. */
} EncoderObject;

static int save(EncoderObject *, PyObject *, int);
static int save_unicode(EncoderObject *, PyObject *);

static void
_write_size64(char *out, size_t value)
{
    size_t i;

    Py_BUILD_ASSERT(sizeof(size_t) <= 8);

    for (i = 0; i < sizeof(size_t); i++) {
        out[i] = (unsigned char)((value >> (8 * i)) & 0xff);
    }
    for (i = sizeof(size_t); i < 8; i++) {
        out[i] = 0;
    }
}

static Py_ssize_t
_Encoder_Write(EncoderObject *self, const char *s, Py_ssize_t data_len)
{
    Py_ssize_t i, n, required;
    char *buffer;

    assert(s != NULL);

    n = data_len;

    required = self->output_len + n;
    if (required > self->max_output_len) {
        /* Make space in buffer */
        if (self->output_len >= PY_SSIZE_T_MAX / 2 - n) {
            PyErr_NoMemory();
            return -1;
        }
        self->max_output_len = (self->output_len + n) / 2 * 3;
        if (_PyBytes_Resize(&self->output_buffer, self->max_output_len) < 0)
            return -1;
    }
    buffer = PyBytes_AS_STRING(self->output_buffer);
    if (data_len < 8) {
        /* This is faster than memcpy when the string is short. */
        for (i = 0; i < data_len; i++) {
            buffer[self->output_len + i] = s[i];
        }
    }
    else {
        memcpy(buffer + self->output_len, s, data_len);
    }
    self->output_len += data_len;
    return data_len;
}

/* Generate a GET opcode for an object stored in the memo. */
static int
memo_get(EncoderObject *self, PyObject *key, Py_ssize_t memo_index)
{
    char pdata[30];
    Py_ssize_t len;

    if (memo_index < 256) {
        pdata[0] = BINGET;
        pdata[1] = (unsigned char)(memo_index & 0xff);
        len = 2;
    }
    else if ((size_t)memo_index <= 0xffffffffUL) {
        pdata[0] = LONG_BINGET;
        pdata[1] = (unsigned char)(memo_index & 0xff);
        pdata[2] = (unsigned char)((memo_index >> 8) & 0xff);
        pdata[3] = (unsigned char)((memo_index >> 16) & 0xff);
        pdata[4] = (unsigned char)((memo_index >> 24) & 0xff);
        len = 5;
    }
    else { /* unlikely */
        QuickleState *st = quickle_get_global_state();
        PyErr_SetString(st->EncodingError,
                        "memo id too large for LONG_BINGET");
        return -1;
    }

    if (_Encoder_Write(self, pdata, len) < 0)
        return -1;

    return 0;
}

/* Store an object in the memo, assign it a new unique ID based on the number
   of objects currently stored in the memo and generate a PUT opcode. */
static int
memo_put(EncoderObject *self, PyObject *obj)
{
    Py_ssize_t idx;

    const char memoize_op = MEMOIZE;

    idx = LookupTable_Size(self->memo);
    if (LookupTable_Set(self->memo, obj, idx) < 0)
        return -1;

    if (_Encoder_Write(self, &memoize_op, 1) < 0)
        return -1;
    return 0;
}
#define MEMO_PUT(self, obj) \
    (((self)->active_memoize) ? memo_put((self), (obj)) : 0)
#define MEMO_PUT_MAYBE(self, obj, memoize) \
    (((self)->active_memoize && (memoize || Py_REFCNT(obj) > 1)) ? memo_put((self), (obj)) : 0)

static int
save_none(EncoderObject *self, PyObject *obj)
{
    const char none_op = NONE;
    if (_Encoder_Write(self, &none_op, 1) < 0)
        return -1;

    return 0;
}

static int
save_bool(EncoderObject *self, PyObject *obj)
{
    const char bool_op = (obj == Py_True) ? NEWTRUE : NEWFALSE;
    if (_Encoder_Write(self, &bool_op, 1) < 0)
        return -1;
    return 0;
}

static int
save_long(EncoderObject *self, PyObject *obj)
{
    PyObject *repr = NULL;
    Py_ssize_t size;
    long val;
    int overflow;
    int status = 0;

    val= PyLong_AsLongAndOverflow(obj, &overflow);
    if (!overflow && (sizeof(long) <= 4 ||
            (val <= 0x7fffffffL && val >= (-0x7fffffffL - 1))))
    {
        /* result fits in a signed 4-byte integer.

           Note: we can't use -0x80000000L in the above condition because some
           compilers (e.g., MSVC) will promote 0x80000000L to an unsigned type
           before applying the unary minus when sizeof(long) <= 4. The
           resulting value stays unsigned which is commonly not what we want,
           so MSVC happily warns us about it.  However, that result would have
           been fine because we guard for sizeof(long) <= 4 which turns the
           condition true in that particular case. */
        char pdata[32];
        Py_ssize_t len = 0;

        pdata[1] = (unsigned char)(val & 0xff);
        pdata[2] = (unsigned char)((val >> 8) & 0xff);
        pdata[3] = (unsigned char)((val >> 16) & 0xff);
        pdata[4] = (unsigned char)((val >> 24) & 0xff);

        if ((pdata[4] != 0) || (pdata[3] != 0)) {
            pdata[0] = BININT;
            len = 5;
        }
        else if (pdata[2] != 0) {
            pdata[0] = BININT2;
            len = 3;
        }
        else {
            pdata[0] = BININT1;
            len = 2;
        }
        if (_Encoder_Write(self, pdata, len) < 0)
            return -1;

        return 0;
    }
    assert(!PyErr_Occurred());

    /* Linear-time */
    size_t nbits;
    size_t nbytes;
    unsigned char *pdata;
    char header[5];
    int i;
    int sign = _PyLong_Sign(obj);

    if (sign == 0) {
        header[0] = LONG1;
        header[1] = 0;      /* It's 0 -- an empty bytestring. */
        if (_Encoder_Write(self, header, 2) < 0)
            goto error;
        return 0;
    }
    nbits = _PyLong_NumBits(obj);
    if (nbits == (size_t)-1 && PyErr_Occurred())
        goto error;
    /* How many bytes do we need?  There are nbits >> 3 full
        * bytes of data, and nbits & 7 leftover bits.  If there
        * are any leftover bits, then we clearly need another
        * byte.  What's not so obvious is that we *probably*
        * need another byte even if there aren't any leftovers:
        * the most-significant bit of the most-significant byte
        * acts like a sign bit, and it's usually got a sense
        * opposite of the one we need.  The exception is ints
        * of the form -(2**(8*j-1)) for j > 0.  Such an int is
        * its own 256's-complement, so has the right sign bit
        * even without the extra byte.  That's a pain to check
        * for in advance, though, so we always grab an extra
        * byte at the start, and cut it back later if possible.
        */
    nbytes = (nbits >> 3) + 1;
    if (nbytes > 0x7fffffffL) {
        PyErr_SetString(PyExc_OverflowError,
                        "int too large to serialize");
        goto error;
    }
    repr = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)nbytes);
    if (repr == NULL)
        goto error;
    pdata = (unsigned char *)PyBytes_AS_STRING(repr);
    i = _PyLong_AsByteArray((PyLongObject *)obj,
                            pdata, nbytes,
                            1 /* little endian */ , 1 /* signed */ );
    if (i < 0)
        goto error;
    /* If the int is negative, this may be a byte more than
        * needed.  This is so iff the MSB is all redundant sign
        * bits.
        */
    if (sign < 0 &&
        nbytes > 1 &&
        pdata[nbytes - 1] == 0xff &&
        (pdata[nbytes - 2] & 0x80) != 0) {
        nbytes--;
    }

    if (nbytes < 256) {
        header[0] = LONG1;
        header[1] = (unsigned char)nbytes;
        size = 2;
    }
    else {
        header[0] = LONG4;
        size = (Py_ssize_t) nbytes;
        for (i = 1; i < 5; i++) {
            header[i] = (unsigned char)(size & 0xff);
            size >>= 8;
        }
        size = 5;
    }
    if (_Encoder_Write(self, header, size) < 0 ||
        _Encoder_Write(self, (char *)pdata, (int)nbytes) < 0)
        goto error;

    if (0) {
  error:
      status = -1;
    }
    Py_XDECREF(repr);

    return status;
}

static int
save_float(EncoderObject *self, PyObject *obj)
{
    double x = PyFloat_AS_DOUBLE((PyFloatObject *)obj);

    char pdata[9];
    pdata[0] = BINFLOAT;
    if (_PyFloat_Pack8(x, (unsigned char *)&pdata[1], 0) < 0)
        return -1;
    if (_Encoder_Write(self, pdata, 9) < 0)
        return -1;
    return 0;
}

static int
save_complex(EncoderObject *self, PyObject *obj)
{
    char pdata[17];
    pdata[0] = COMPLEX;
    if (_PyFloat_Pack8(PyComplex_RealAsDouble(obj), (unsigned char *)&pdata[1], 0) < 0)
        return -1;
    if (_PyFloat_Pack8(PyComplex_ImagAsDouble(obj), (unsigned char *)&pdata[9], 0) < 0)
        return -1;
    if (_Encoder_Write(self, pdata, 17) < 0)
        return -1;
    return 0;
}

static inline void
pack_int(char *buf, int ind, int n, int val) {
    for (int i = 0; i < n; i++) {
        buf[ind + i] = (unsigned char)((val >> (i * 8)) & 0xff); \
    }
}

static inline int
unpack_int(char *buf, int ind, int n)
{
    unsigned char *s = (unsigned char *)buf;
    int x = 0;
    for (int i = 0; i < n; i++) {
        x |= (int)s[ind + i] << (i * 8);
    }
    return x;
}

static int
save_timedelta(EncoderObject *self, PyObject *obj) {
    char pdata[11];
    pdata[0] = TIMEDELTA;
    pack_int(pdata, 1, 4, PyDateTime_DELTA_GET_DAYS(obj));
    pack_int(pdata, 5, 3, PyDateTime_DELTA_GET_SECONDS(obj));
    pack_int(pdata, 8, 3, PyDateTime_DELTA_GET_MICROSECONDS(obj));
    if (_Encoder_Write(self, pdata, 11) < 0)
        return -1;
    if (MEMO_PUT_MAYBE(self, obj, 0) < 0) {
        return -1;
    }
    return 0;
}

static int
save_date(EncoderObject *self, PyObject *obj) {
    char pdata[5];
    pdata[0] = DATE;
    pack_int(pdata, 1, 2, PyDateTime_GET_YEAR(obj));
    pack_int(pdata, 3, 1, PyDateTime_GET_MONTH(obj));
    pack_int(pdata, 4, 1, PyDateTime_GET_DAY(obj));
    if (_Encoder_Write(self, pdata, 5) < 0)
        return -1;
    if (MEMO_PUT_MAYBE(self, obj, 0) < 0) {
        return -1;
    }
    return 0;
}

#if !defined(PyDateTime_TIME_GET_TZINFO)
#define PyDateTime_TIME_GET_TZINFO(o)      ((((PyDateTime_Time *)(o))->hastzinfo) ? \
    ((PyDateTime_Time *)(o))->tzinfo : Py_None)
#endif

static int
save_time(EncoderObject *self, PyObject *obj) {
    char pdata[7];
    PyObject *tzinfo = PyDateTime_TIME_GET_TZINFO(obj);
    if (tzinfo != Py_None) {
        if (save(self, tzinfo, 0) < 0)
            return -1;
        pdata[0] = TIME_TZ;
    }
    else {
        pdata[0] = TIME;
    }
    pack_int(pdata, 1, 1, PyDateTime_TIME_GET_HOUR(obj));
    pack_int(pdata, 2, 1, PyDateTime_TIME_GET_MINUTE(obj));
    pack_int(pdata, 3, 1, PyDateTime_TIME_GET_SECOND(obj));
    pack_int(pdata, 4, 3, PyDateTime_TIME_GET_MICROSECOND(obj));
    if (PyDateTime_TIME_GET_FOLD(obj))
        pdata[1] |= (1 << 7);
    if (_Encoder_Write(self, pdata, 7) < 0)
        return -1;
    if (MEMO_PUT_MAYBE(self, obj, 0) < 0) {
        return -1;
    }
    return 0;
}

#if !defined(PyDateTime_DATE_GET_TZINFO)
#define PyDateTime_DATE_GET_TZINFO(o)      ((((PyDateTime_DateTime *)(o))->hastzinfo) ? \
    ((PyDateTime_DateTime *)(o))->tzinfo : Py_None)
#endif

static int
save_datetime(EncoderObject *self, PyObject *obj) {
    char pdata[11];
    PyObject *tzinfo = PyDateTime_DATE_GET_TZINFO(obj);
    if (tzinfo != Py_None) {
        if (save(self, tzinfo, 0) < 0)
            return -1;
        pdata[0] = DATETIME_TZ;
    }
    else {
        pdata[0] = DATETIME;
    }
    pack_int(pdata, 1, 2, PyDateTime_GET_YEAR(obj));
    pack_int(pdata, 3, 1, PyDateTime_GET_MONTH(obj));
    pack_int(pdata, 4, 1, PyDateTime_GET_DAY(obj));
    pack_int(pdata, 5, 1, PyDateTime_DATE_GET_HOUR(obj));
    pack_int(pdata, 6, 1, PyDateTime_DATE_GET_MINUTE(obj));
    pack_int(pdata, 7, 1, PyDateTime_DATE_GET_SECOND(obj));
    pack_int(pdata, 8, 3, PyDateTime_DATE_GET_MICROSECOND(obj));
    if (PyDateTime_DATE_GET_FOLD(obj))
        pdata[5] |= (1 << 7);
    if (_Encoder_Write(self, pdata, 11) < 0)
        return -1;
    if (MEMO_PUT_MAYBE(self, obj, 0) < 0) {
        return -1;
    }
    return 0;
}

static int
save_timezone_utc(EncoderObject *self, PyObject *obj)
{
    const char op = TIMEZONE_UTC;
    if (_Encoder_Write(self, &op, 1) < 0)
        return -1;
    return 0;
}

/* The TimeZone type in CPython isn't exposed, we mock the definition here
 * to get access to the layout */
typedef struct
{
    PyObject_HEAD
    PyObject *offset;
} MockTimeZone;

static int
save_timezone(EncoderObject *self, PyObject *obj)
{
    PyObject *offset;
    char pdata[7];
    int seconds, microseconds;
    offset = ((MockTimeZone *)(obj))->offset;
    pdata[0] = TIMEZONE;
    seconds = PyDateTime_DELTA_GET_SECONDS(offset);
    microseconds = PyDateTime_DELTA_GET_MICROSECONDS(offset);
    if (PyDateTime_DELTA_GET_DAYS(offset) < 0)
        seconds |= (1 << 23);
    pack_int(pdata, 1, 3, seconds);
    pack_int(pdata, 4, 3, microseconds);
    if (_Encoder_Write(self, pdata, 7) < 0)
        return -1;
    if (MEMO_PUT_MAYBE(self, obj, 0) < 0) {
        return -1;
    }
    return 0;
}

/* The ZoneInfo type in CPython isn't exposed, we mock the definition here
 * to get access to the layout */
typedef struct
{
    PyDateTime_TZInfo base;
    PyObject *key;
} MockZoneInfo;

static int
save_zoneinfo(EncoderObject *self, PyObject *obj)
{
    PyObject *key;
    const char op = ZONEINFO;
    key = ((MockZoneInfo *)(obj))->key;
    if (key == NULL || !PyUnicode_CheckExact(key)) {
        QuickleState *st = quickle_get_global_state();
        PyErr_Format(
            st->EncodingError, "Cannot serialize `%R`, unsupported key", obj
        );
        return -1;
    }
    if (save_unicode(self, key) < 0)
        return -1;
    if (_Encoder_Write(self, &op, 1) < 0)
        return -1;
    if (MEMO_PUT_MAYBE(self, obj, 0) < 0)
        return -1;
    return 0;
}

static int
_write_bytes(EncoderObject *self,
             const char *header, Py_ssize_t header_size,
             const char *data, Py_ssize_t data_size,
             PyObject *payload)
{
    if (_Encoder_Write(self, header, header_size) < 0 ||
        _Encoder_Write(self, data, data_size) < 0) {
        return -1;
    }
    return 0;
}

static int
_save_bytes_data(EncoderObject *self, PyObject *obj, const char *data,
                 Py_ssize_t size)
{
    char header[9];
    Py_ssize_t len;

    if (size < 0)
        return -1;

    if (size <= 0xff) {
        header[0] = SHORT_BINBYTES;
        header[1] = (unsigned char)size;
        len = 2;
    }
    else if ((size_t)size <= 0xffffffffUL) {
        header[0] = BINBYTES;
        header[1] = (unsigned char)(size & 0xff);
        header[2] = (unsigned char)((size >> 8) & 0xff);
        header[3] = (unsigned char)((size >> 16) & 0xff);
        header[4] = (unsigned char)((size >> 24) & 0xff);
        len = 5;
    }
    else {
        header[0] = BINBYTES8;
        _write_size64(header + 1, size);
        len = 9;
    }

    if (_write_bytes(self, header, len, data, size, obj) < 0) {
        return -1;
    }

    if (MEMO_PUT_MAYBE(self, obj, 0) < 0) {
        return -1;
    }

    return 0;
}

static int
save_bytes(EncoderObject *self, PyObject *obj)
{
    return _save_bytes_data(self, obj, PyBytes_AS_STRING(obj),
                            PyBytes_GET_SIZE(obj));
}

static int
_save_bytearray_data(EncoderObject *self, PyObject *obj, const char *data,
                     Py_ssize_t size)
{
    char header[9];
    Py_ssize_t len;

    if (size < 0)
        return -1;

    header[0] = BYTEARRAY8;
    _write_size64(header + 1, size);
    len = 9;

    if (_write_bytes(self, header, len, data, size, obj) < 0) {
        return -1;
    }

    if (MEMO_PUT_MAYBE(self, obj, 0) < 0) {
        return -1;
    }

    return 0;
}

static int
save_bytearray(EncoderObject *self, PyObject *obj)
{
    return _save_bytearray_data(self, obj, PyByteArray_AS_STRING(obj),
                                PyByteArray_GET_SIZE(obj));
}

static int
save_picklebuffer(EncoderObject *self, PyObject *obj)
{
    const Py_buffer* view = PyPickleBuffer_GetBuffer(obj);
    if (view == NULL) {
        return -1;
    }
    if (view->suboffsets != NULL || !PyBuffer_IsContiguous(view, 'A')) {
        QuickleState *st = quickle_get_global_state();
        PyErr_SetString(st->EncodingError,
                        "PickleBuffer can not be serialized when "
                        "pointing to a non-contiguous buffer");
        return -1;
    }
    if (self->active_collect_buffers) {
        /* Write data out-of-band */
        if (PyList_Append(self->buffers, obj) < 0)
            return -1;
        const char next_buffer_op = NEXT_BUFFER;
        if (_Encoder_Write(self, &next_buffer_op, 1) < 0) {
            return -1;
        }
        if (view->readonly) {
            const char readonly_buffer_op = READONLY_BUFFER;
            if (_Encoder_Write(self, &readonly_buffer_op, 1) < 0) {
                return -1;
            }
        }
    }
    else {
        /* Write data in-band */
        if (view->readonly) {
            return _save_bytes_data(self, obj, (const char*) view->buf,
                                    view->len);
        }
        else {
            return _save_bytearray_data(self, obj, (const char*) view->buf,
                                        view->len);
        }
    }
    return 0;
}

static int
save_unicode(EncoderObject *self, PyObject *obj)
{
    char header[9];
    Py_ssize_t len;
    PyObject *encoded = NULL;
    Py_ssize_t size;
    const char *data;

    if (PyUnicode_READY(obj))
        return -1;

    data = PyUnicode_AsUTF8AndSize(obj, &size);
    if (data == NULL) {
        /* Issue #8383: for strings with lone surrogates, fallback on the
           "surrogatepass" error handler. */
        PyErr_Clear();
        encoded = PyUnicode_AsEncodedString(obj, "utf-8", "surrogatepass");
        if (encoded == NULL)
            return -1;

        data = PyBytes_AS_STRING(encoded);
        size = PyBytes_GET_SIZE(encoded);
    }

    assert(size >= 0);
    if (size <= 0xff) {
        header[0] = SHORT_BINUNICODE;
        header[1] = (unsigned char)(size & 0xff);
        len = 2;
    }
    else if ((size_t)size <= 0xffffffffUL) {
        header[0] = BINUNICODE;
        header[1] = (unsigned char)(size & 0xff);
        header[2] = (unsigned char)((size >> 8) & 0xff);
        header[3] = (unsigned char)((size >> 16) & 0xff);
        header[4] = (unsigned char)((size >> 24) & 0xff);
        len = 5;
    }
    else {
        header[0] = BINUNICODE8;
        _write_size64(header + 1, size);
        len = 9;
    }

    if (_write_bytes(self, header, len, data, size, encoded) < 0) {
        Py_XDECREF(encoded);
        return -1;
    }
    Py_XDECREF(encoded);

    if (MEMO_PUT_MAYBE(self, obj, 0) < 0) {
        return -1;
    }

    return 0;
}

/* A helper for save_tuple.  Push the len elements in tuple t on the stack. */
static int
store_tuple_elements(EncoderObject *self, PyObject *t, Py_ssize_t len, int memoize)
{
    Py_ssize_t i;
    int memoize2;

    assert(PyTuple_Size(t) == len);

    /* Since tuples are immutable, cycle checks happen on the elements not the
     * tuple itself. We disable the memo refcnt optimization if the tuple has
     * more than once reference, since it might be recursive then. This could
     * be alleviated by adding new opcodes for tuples that won't be seen by
     * python code (since we can mutate those), but that'd be diverting from
     * cpython's pickle even more, and might not be worth it. */
    memoize2 = memoize || Py_REFCNT(t) > 1;

    for (i = 0; i < len; i++) {
        PyObject *element = PyTuple_GET_ITEM(t, i);

        if (element == NULL)
            return -1;
        if (save(self, element, memoize2) < 0)
            return -1;
    }

    return 0;
}

static int
save_tuple(EncoderObject *self, PyObject *obj, int memoize)
{
    Py_ssize_t len, i, memo_index;

    const char empty_tuple_op = EMPTY_TUPLE;
    const char mark_op = MARK;
    const char tuple_op = TUPLE;
    const char pop_op = POP;
    const char pop_mark_op = POP_MARK;
    const char len2opcode[] = {EMPTY_TUPLE, TUPLE1, TUPLE2, TUPLE3};

    if ((len = PyTuple_Size(obj)) < 0)
        return -1;

    if (len == 0) {
        if (_Encoder_Write(self, &empty_tuple_op, 1) < 0)
            return -1;
        return 0;
    }

    /* The tuple isn't in the memo now.  If it shows up there after
     * saving the tuple elements, the tuple must be recursive, in
     * which case we'll pop everything we put on the stack, and fetch
     * its value from the memo.
     */
    if (len <= 3) {
        /* Use TUPLE{1,2,3} opcodes. */
        if (store_tuple_elements(self, obj, len, memoize) < 0)
            return -1;

        memo_index = MEMO_GET(self, obj);
        if (memo_index >= 0) {
            /* pop the len elements */
            for (i = 0; i < len; i++)
                if (_Encoder_Write(self, &pop_op, 1) < 0)
                    return -1;
            /* fetch from memo */
            if (memo_get(self, obj, memo_index) < 0)
                return -1;

            return 0;
        }
        else { /* Not recursive. */
            if (_Encoder_Write(self, len2opcode + len, 1) < 0)
                return -1;
        }
    }
    else {
        /* Generate MARK e1 e2 ... TUPLE */
        if (_Encoder_Write(self, &mark_op, 1) < 0)
            return -1;

        if (store_tuple_elements(self, obj, len, memoize) < 0)
            return -1;

        memo_index = MEMO_GET(self, obj);
        if (memo_index >= 0) {
            /* pop the stack stuff we pushed */
            if (_Encoder_Write(self, &pop_mark_op, 1) < 0)
                return -1;
            /* fetch from memo */
            if (memo_get(self, obj, memo_index) < 0)
                return -1;

            return 0;
        }
        else { /* Not recursive. */
            if (_Encoder_Write(self, &tuple_op, 1) < 0)
                return -1;
        }
    }

    if (MEMO_PUT_MAYBE(self, obj, memoize) < 0)
        return -1;

    return 0;
}


/*
 * Batch up chunks of `MARK item item ... item APPENDS`
 * opcode sequences.  Calling code should have arranged to first create an
 * empty list, or list-like object, for the APPENDS to operate on.
 * Returns 0 on success, -1 on error.
 */
static int
batch_list(EncoderObject *self, PyObject *obj, int memoize)
{
    PyObject *item = NULL;
    Py_ssize_t this_batch, total;

    const char append_op = APPEND;
    const char appends_op = APPENDS;
    const char mark_op = MARK;

    assert(obj != NULL);
    assert(PyList_CheckExact(obj));

    if (PyList_GET_SIZE(obj) == 1) {
        item = PyList_GET_ITEM(obj, 0);
        if (save(self, item, memoize) < 0)
            return -1;
        if (_Encoder_Write(self, &append_op, 1) < 0)
            return -1;
        return 0;
    }

    /* Write in batches of BATCHSIZE. */
    total = 0;
    do {
        this_batch = 0;
        if (_Encoder_Write(self, &mark_op, 1) < 0)
            return -1;
        while (total < PyList_GET_SIZE(obj)) {
            item = PyList_GET_ITEM(obj, total);
            if (save(self, item, memoize) < 0)
                return -1;
            total++;
            if (++this_batch == BATCHSIZE)
                break;
        }
        if (_Encoder_Write(self, &appends_op, 1) < 0)
            return -1;

    } while (total < PyList_GET_SIZE(obj));

    return 0;
}

static int
save_list(EncoderObject *self, PyObject *obj, int memoize)
{
    char header[3];
    Py_ssize_t len;

    /* Create an empty list. */
    header[0] = EMPTY_LIST;
    len = 1;

    if (_Encoder_Write(self, header, len) < 0)
        return -1;

    if (MEMO_PUT_MAYBE(self, obj, memoize) < 0)
        return -1;

    if (PyList_GET_SIZE(obj))
        return batch_list(self, obj, memoize);
    return 0;
}

/*
 * Batch up chunks of `MARK key value ... key value SETITEMS`
 * opcode sequences.  Calling code should have arranged to first create an
 * empty dict, or dict-like object, for the SETITEMS to operate on.
 * Returns 0 on success, -1 on error.
 */
static int
batch_dict(EncoderObject *self, PyObject *obj, int memoize)
{
    PyObject *key = NULL, *value = NULL;
    int i;
    Py_ssize_t dict_size, ppos = 0;

    const char mark_op = MARK;
    const char setitem_op = SETITEM;
    const char setitems_op = SETITEMS;

    assert(obj != NULL && PyDict_CheckExact(obj));

    dict_size = PyDict_GET_SIZE(obj);

    /* Special-case len(d) == 1 to save space. */
    if (dict_size == 1) {
        PyDict_Next(obj, &ppos, &key, &value);
        if (save(self, key, memoize) < 0)
            return -1;
        if (save(self, value, memoize) < 0)
            return -1;
        if (_Encoder_Write(self, &setitem_op, 1) < 0)
            return -1;
        return 0;
    }

    /* Write in batches of BATCHSIZE. */
    do {
        i = 0;
        if (_Encoder_Write(self, &mark_op, 1) < 0)
            return -1;
        while (PyDict_Next(obj, &ppos, &key, &value)) {
            if (save(self, key, memoize) < 0)
                return -1;
            if (save(self, value, memoize) < 0)
                return -1;
            if (++i == BATCHSIZE)
                break;
        }
        if (_Encoder_Write(self, &setitems_op, 1) < 0)
            return -1;
        if (PyDict_GET_SIZE(obj) != dict_size) {
            PyErr_Format(
                PyExc_RuntimeError,
                "dictionary changed size during iteration");
            return -1;
        }

    } while (i == BATCHSIZE);
    return 0;
}

static int
save_dict(EncoderObject *self, PyObject *obj, int memoize)
{
    char header[3];
    Py_ssize_t len;
    assert(PyDict_Check(obj));

    /* Create an empty dict. */
    header[0] = EMPTY_DICT;
    len = 1;

    if (_Encoder_Write(self, header, len) < 0)
        return -1;

    if (MEMO_PUT_MAYBE(self, obj, memoize) < 0)
        return -1;

    if (PyDict_GET_SIZE(obj))
        return batch_dict(self, obj, memoize);
    return 0;
}

static int
save_set(EncoderObject *self, PyObject *obj, int memoize)
{
    PyObject *item;
    int i;
    Py_ssize_t set_size, ppos = 0;
    Py_hash_t hash;

    const char empty_set_op = EMPTY_SET;
    const char mark_op = MARK;
    const char additems_op = ADDITEMS;

    if (_Encoder_Write(self, &empty_set_op, 1) < 0)
        return -1;

    if (MEMO_PUT_MAYBE(self, obj, memoize) < 0)
        return -1;

    set_size = PySet_GET_SIZE(obj);
    if (set_size == 0)
        return 0;  /* nothing to do */

    /* Write in batches of BATCHSIZE. */
    do {
        i = 0;
        if (_Encoder_Write(self, &mark_op, 1) < 0)
            return -1;
        while (_PySet_NextEntry(obj, &ppos, &item, &hash)) {
            if (save(self, item, memoize) < 0)
                return -1;
            if (++i == BATCHSIZE)
                break;
        }
        if (_Encoder_Write(self, &additems_op, 1) < 0)
            return -1;
        if (PySet_GET_SIZE(obj) != set_size) {
            PyErr_Format(
                PyExc_RuntimeError,
                "set changed size during iteration");
            return -1;
        }
    } while (i == BATCHSIZE);

    return 0;
}

static int
save_frozenset(EncoderObject *self, PyObject *obj, int memoize)
{
    Py_ssize_t memo_index;
    PyObject *iter;

    const char mark_op = MARK;
    const char frozenset_op = FROZENSET;

    if (_Encoder_Write(self, &mark_op, 1) < 0)
        return -1;

    iter = PyObject_GetIter(obj);
    if (iter == NULL) {
        return -1;
    }
    for (;;) {
        PyObject *item;

        item = PyIter_Next(iter);
        if (item == NULL) {
            if (PyErr_Occurred()) {
                Py_DECREF(iter);
                return -1;
            }
            break;
        }
        /* Convert to borrowed reference, enables refcnt optimization */
        Py_DECREF(item);
        if (save(self, item, memoize) < 0) {
            Py_DECREF(iter);
            return -1;
        }
    }
    Py_DECREF(iter);

    /* If the object is already in the memo, this means it is
       recursive. In this case, throw away everything we put on the
       stack, and fetch the object back from the memo. */
    memo_index = MEMO_GET(self, obj);
    if (memo_index >= 0) {
        const char pop_mark_op = POP_MARK;

        if (_Encoder_Write(self, &pop_mark_op, 1) < 0)
            return -1;
        if (memo_get(self, obj, memo_index) < 0)
            return -1;
        return 0;
    }

    if (_Encoder_Write(self, &frozenset_op, 1) < 0)
        return -1;
    if (MEMO_PUT_MAYBE(self, obj, memoize) < 0)
        return -1;

    return 0;
}

static int
write_typecode(EncoderObject *self, PyObject *obj, const char op1, const char op2, const char op3) {
    int n;
    Py_ssize_t code = -1;
    char pdata[6];

    if (self->registry != NULL) {
        code = LookupTable_Get(self->registry, (PyObject*)Py_TYPE(obj));
    }
    if (code == -1) {
        PyErr_Format(PyExc_TypeError,
                     "Type %.200s isn't in type registry",
                     Py_TYPE(obj)->tp_name);
        return -1;
    }
    if (code < 0xff) {
        pdata[0] = op1;
        pdata[1] = (unsigned char)code;
        n = 2;
    }
    else if (code <= 0xffff) {
        pdata[0] = op2;
        pdata[1] = (unsigned char)(code & 0xff);
        pdata[2] = (unsigned char)((code >> 8) & 0xff);
        n = 3;
    }
    else {
        pdata[0] = op3;
        pdata[1] = (unsigned char)(code & 0xff);
        pdata[2] = (unsigned char)((code >> 8) & 0xff);
        pdata[3] = (unsigned char)((code >> 16) & 0xff);
        pdata[4] = (unsigned char)((code >> 24) & 0xff);
        n = 5;
    }
    if (_Encoder_Write(self, pdata, n) < 0)
        return -1;

    return 0;
}

static int
save_struct(EncoderObject *self, PyObject *obj, int memoize)
{
    Py_ssize_t i, nfields;
    PyObject *val;

    const char mark_op = MARK;
    const char buildstruct_op = BUILDSTRUCT;

    if (write_typecode(self, obj, STRUCT1, STRUCT2, STRUCT4) < 0)
        return -1;

    if (MEMO_PUT_MAYBE(self, obj, memoize) < 0)
        return -1;

    if (_Encoder_Write(self, &mark_op, 1) < 0)
        return -1;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(obj));
    for (i = 0; i < nfields; i++) {
        val = Struct_get_index(obj, i);
        if (val == NULL) {
            return -1;
        }
        if (save(self, val, memoize) < 0)
            return -1;
    }
    if (_Encoder_Write(self, &buildstruct_op, 1) < 0)
        return -1;

    return 0;
}

static int
save_enum(EncoderObject *self, PyObject *obj)
{
    if (PyLong_Check(obj)) {
        if (save_long(self, obj) < 0) {
            return -1;
        }
    } else {
        int status;
        PyObject *name = NULL;
        QuickleState *st = quickle_get_global_state();
        name = PyObject_GetAttr(obj, st->name_str);
        if (name == NULL)
            return -1;

        status = save(self, name, 0);
        Py_DECREF(name);
        if (status < 0)
            return -1;
    }

    if (write_typecode(self, obj, ENUM1, ENUM2, ENUM4) < 0)
        return -1;

    if (MEMO_PUT(self, obj) < 0)
        return -1;

    return 0;
}

# define RETURN_RECURSIVE(call) \
    do { \
        if (Py_EnterRecursiveCall(" while serializing an object")) return -1; \
        int status = (call); \
        Py_LeaveRecursiveCall(); \
        return status; \
    } while (0)


static int
save(EncoderObject *self, PyObject *obj, int memoize)
{
    PyTypeObject *type;
    QuickleState *st;
    Py_ssize_t memo_index;

    type = Py_TYPE(obj);

    /* Atom types; these aren't memoized, so don't check the memo. */
    if (obj == Py_None) {
        return save_none(self, obj);
    }
    else if (obj == Py_False || obj == Py_True) {
        return save_bool(self, obj);
    }
    else if (type == &PyLong_Type) {
        return save_long(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return save_float(self, obj);
    }
    else if (type == &PyComplex_Type) {
        return save_complex(self, obj);
    }
    else if (obj == PyDateTime_TimeZone_UTC) {
        return save_timezone_utc(self, obj);
    }

    /* Check the memo to see if it has the object. If so, generate
       a BINGET opcode, instead of reserializing the object */
    memo_index = MEMO_GET(self, obj);
    if (memo_index >= 0) {
        return memo_get(self, obj, memo_index);
    }

    if (type == &PyUnicode_Type) {
        return save_unicode(self, obj);
    }
    else if (type == &PyBytes_Type) {
        return save_bytes(self, obj);
    }
    else if (type == &PyByteArray_Type) {
        return save_bytearray(self, obj);
    }
    else if (Py_TYPE(type) == &StructMetaType) {
        RETURN_RECURSIVE(save_struct(self, obj, memoize));
    }
    else if (type == &PyDict_Type) {
        RETURN_RECURSIVE(save_dict(self, obj, memoize));
    }
    else if (type == &PyList_Type) {
        RETURN_RECURSIVE(save_list(self, obj, memoize));
    }
    else if (type == &PyTuple_Type) {
        RETURN_RECURSIVE(save_tuple(self, obj, memoize));
    }
    else if (type == &PySet_Type) {
        RETURN_RECURSIVE(save_set(self, obj, memoize));
    }
    else if (type == &PyFrozenSet_Type) {
        RETURN_RECURSIVE(save_frozenset(self, obj, memoize));
    }
    else if (type == &PyPickleBuffer_Type) {
        return save_picklebuffer(self, obj);
    }
    else if (type == PyDateTimeAPI->DeltaType) {
        return save_timedelta(self, obj);
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        return save_datetime(self, obj);
    }
    else if (type == PyDateTimeAPI->DateType) {
        return save_date(self, obj);
    }
    else if (type == PyDateTimeAPI->TimeType) {
        return save_time(self, obj);
    }

    st = quickle_get_global_state();
    if (PyType_IsSubtype(type, st->EnumType)) {
        return save_enum(self, obj);
    }
    else if (type == st->TimeZoneType) {
        return save_timezone(self, obj);
    }
    else if (type == st->ZoneInfoType) {
        return save_zoneinfo(self, obj);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "quickle doesn't support objects of type %.200s",
                     type->tp_name);
        return -1;
    }
}

static int
dump(EncoderObject *self, PyObject *obj)
{
    const char stop_op = STOP;

    if (save(self, obj, 0) < 0 || _Encoder_Write(self, &stop_op, 1) < 0)
        return -1;
    return 0;
}

static PyObject*
Encoder_dumps_internal(EncoderObject *self, PyObject *obj)
{
    int status;
    PyObject *buffers, *temp, *res = NULL;

    /* reset buffers */
    self->output_len = 0;
    if (self->output_buffer == NULL) {
        self->max_output_len = self->write_buffer_size;
        self->output_buffer = PyBytes_FromStringAndSize(NULL, self->max_output_len);
        if (self->output_buffer == NULL)
            return NULL;
    }
    /* Allocate a new list for buffers if needed */
    if (self->active_collect_buffers && self->buffers == NULL) {
        self->buffers = PyList_New(0);
        if (self->buffers == NULL) {
            return NULL;
        }
    }

    status = dump(self, obj);

    /* Reset temporary state */
    if (self->active_memoize) {
        if (LookupTable_Reset(self->memo) < 0)
            status = -1;
    }
    self->active_memoize = self->memoize;

    if (status == 0) {
        if (self->max_output_len > self->write_buffer_size) {
            /* Buffer was resized, trim to length */
            res = self->output_buffer;
            self->output_buffer = NULL;
            _PyBytes_Resize(&res, self->output_len);
        }
        else {
            /* Only constant buffer used, copy to output */
            res = PyBytes_FromStringAndSize(
                PyBytes_AS_STRING(self->output_buffer),
                self->output_len
            );
        }
        if (self->active_collect_buffers) {
            if (PyList_GET_SIZE(self->buffers) > 0) {
                buffers = self->buffers;
                self->buffers = NULL;
            }
            else {
                buffers = Py_None;
                Py_INCREF(buffers);
            }
            temp = PyTuple_New(2);
            if (temp == NULL) {
                Py_DECREF(res);
                Py_DECREF(buffers);
                return NULL;
            }
            PyTuple_SET_ITEM(temp, 0, res);
            PyTuple_SET_ITEM(temp, 1, buffers);
            res = temp;
        }
    } else {
        /* Error in dumps, drop buffer if necessary */
        if (self->max_output_len > self->write_buffer_size) {
            Py_DECREF(self->output_buffer);
            self->output_buffer = NULL;
        }
        if (self->buffers != NULL && PyList_GET_SIZE(self->buffers)) {
            Py_CLEAR(self->buffers);
        }
    }
    self->active_collect_buffers = self->collect_buffers;
    return res;
}


static char *Encoder_dumps_kws[] = {"memoize", "collect_buffers", NULL};

PyDoc_STRVAR(Encoder_dumps__doc__,
"dumps(obj, *, memoize=None, collect_buffers=None)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"memoize : bool, optional\n"
"    Whether to enable memoization. Defaults to the value set on the encoder.\n"
"    If True, any duplicate objects will only be serialized once. Disabling\n"
"    memoization can be more efficient for objects unlikely to contain duplicate\n"
"    values, but self-referential objects will then fail to serialize.\n"
"collect_buffers : bool, optional\n"
"    Whether to collect out-of-band buffers. Defaults to the value set on the\n"
"    encoder. If True, the return value will be the serialized object, and a\n"
"    list of any `PickleBuffer` objects found (or `None` if none are found).\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object\n"
"buffers : list of `PickleBuffer` or `None`, optional\n"
"    If ``collect_buffers`` is `True`, a list of out-of-band buffers will\n"
"    also be returned (or None if no buffers are found). Not returned if\n"
"    ``collect_buffers`` is `False`"
);
static PyObject*
Encoder_dumps(EncoderObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    int temp;
    PyObject *obj = NULL;
    PyObject *memoize = Py_None;
    PyObject *collect_buffers = Py_None;
    QuickleState *st = quickle_get_global_state();

    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }
    obj = args[0];
    if (kwnames != NULL) {
        if (!parse_keywords(kwnames, args + nargs, st->encoder_dumps_kws, &memoize, &collect_buffers)) {
            return NULL;
        }
    }

    if (memoize == Py_None) {
        self->active_memoize = self->memoize;
    }
    else {
        temp = PyObject_IsTrue(memoize);
        if (temp < 0) {
            return NULL;
        }
        self->active_memoize = temp;
    }
    if (collect_buffers == Py_None) {
        self->active_collect_buffers = self->collect_buffers;
    }
    else {
        temp = PyObject_IsTrue(collect_buffers);
        if (temp < 0) {
            return NULL;
        }
        self->active_collect_buffers = temp;
    }
    return Encoder_dumps_internal(self, obj);
}

static PyObject*
Encoder_sizeof(EncoderObject *self)
{
    Py_ssize_t res;

    res = sizeof(EncoderObject);
    if (self->memo != NULL) {
        res += sizeof(LookupTable);
        res += self->memo->allocated * sizeof(LookupEntry);
    }
    if (self->output_buffer != NULL) {
        res += self->max_output_len;
    }
    return PyLong_FromSsize_t(res);
}

static struct PyMethodDef Encoder_methods[] = {
    {
        "dumps", (PyCFunction) Encoder_dumps, METH_FASTCALL | METH_KEYWORDS,
        Encoder_dumps__doc__,
    },
    {
        "__sizeof__", (PyCFunction) Encoder_sizeof, METH_NOARGS,
        PyDoc_STR("Size in bytes")
    },
    {NULL, NULL}                /* sentinel */
};

static int
Encoder_clear(EncoderObject *self)
{
    Py_CLEAR(self->output_buffer);
    Py_CLEAR(self->buffers);
    if (self->registry != NULL) {
        LookupTable_Del(self->registry);
        self->registry = NULL;
    }

    if (self->memo != NULL) {
        LookupTable_Del(self->memo);
        self->memo = NULL;
    }
    return 0;
}

static void
Encoder_dealloc(EncoderObject *self)
{
    PyObject_GC_UnTrack(self);
    Encoder_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Encoder_traverse(EncoderObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->buffers);
    if ((self->registry != NULL) && (LookupTable_Traverse(self->registry, visit, arg) < 0))
        return -1;
    if ((self->memo != NULL) && (LookupTable_Traverse(self->memo, visit, arg) < 0))
        return -1;
    return 0;
}

static int
Encoder_init_internal(
    EncoderObject *self, int memoize,
    int collect_buffers, PyObject *registry,
    Py_ssize_t write_buffer_size
) {
    Py_ssize_t i;

    self->collect_buffers = collect_buffers;
    self->active_collect_buffers = collect_buffers;
    self->registry = NULL;
    self->memo = NULL;
    self->output_buffer = NULL;
    self->buffers = NULL;

    if (registry == NULL || registry == Py_None) {
        self->registry = NULL;
    }
    else if (PyList_Check(registry)) {
        self->registry = LookupTable_New(PyList_GET_SIZE(registry));
        if (self->registry == NULL)
            return -1;
        for (i = 0; i < PyList_GET_SIZE(registry); i++) {
            if (LookupTable_Set(self->registry, PyList_GET_ITEM(registry, i), i) < 0)
                return -1;
        }
        Py_INCREF(registry);
    }
    else if (PyDict_Check(registry)) {
        PyObject *key, *value;
        Py_ssize_t code, pos = 0;

        self->registry = LookupTable_New(PyDict_Size(registry));
        if (self->registry == NULL)
            return -1;
        while (PyDict_Next(registry, &pos, &key, &value)) {
            code = PyLong_AsSsize_t(value);
            if (code < 0 || code > 0x7fffffffL) {
                if (!PyErr_Occurred())
                    PyErr_Format(
                        PyExc_ValueError,
                        "registry values must be between 0 and 2147483647, got %zd",
                        code
                    );
                return -1;
            }
            if (LookupTable_Set(self->registry, key, code))
                return -1;
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError, "registry must be a list or a dict");
        return -1;
    }

    self->memoize = memoize;
    self->active_memoize = memoize;
    self->memo = LookupTable_New(64);
    if (self->memo == NULL)
        return -1;

    self->write_buffer_size = Py_MAX(write_buffer_size, 32);
    self->max_output_len = self->write_buffer_size;
    self->output_len = 0;
    self->output_buffer = PyBytes_FromStringAndSize(NULL, self->max_output_len);
    if (self->output_buffer == NULL)
        return -1;
    return 0;
}

PyDoc_STRVAR(Encoder__doc__,
"Encoder(*, memoize=True, collect_buffers=False, registry=None, write_buffer_size=4096)\n"
"--\n"
"\n"
"A quickle encoder.\n"
"\n"
"Creating an `Encoder` and calling the `Encoder.dumps` method multiple times is more\n"
"efficient than calling `quickle.dumps` multiple times.\n"
"\n"
"Parameters\n"
"----------\n"
"memoize : bool, optional\n"
"    Whether to enable memoization. If True (default), any duplicate objects will\n"
"    only be serialized once. Disabling memoization can be more efficient for\n"
"    objects unlikely to contain duplicate values, but self-referential objects\n"
"    will then fail to serialize.\n"
"collect_buffers : bool, optional\n"
"    Whether to collect out-of-band buffers. If True, the return value of\n"
"    `Encoder.dumps` will be the serialized bytes, and a list of any\n"
"    `PickleBuffer` objects found (or `None` if none are found).\n"
"registry : list or dict, optional\n"
"    A registry of user-defined types this decoder instance should support. Can\n"
"    be either a list of types (recommended), or a dict mapping the type to a\n"
"    unique positive integer. Note that for deserialization to be successful,\n"
"    the registry should match that of the corresponding `Decoder`.\n"
"write_buffer_size : int, optional\n"
"    The size of the internal static write buffer."
);
static int
Encoder_init(EncoderObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"memoize", "collect_buffers", "registry", "write_buffer_size", NULL};

    int memoize = 1;
    int collect_buffers = 0;
    PyObject *registry = NULL;
    Py_ssize_t write_buffer_size = 4096;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$ppOn", kwlist,
                                     &memoize,
                                     &collect_buffers,
                                     &registry,
                                     &write_buffer_size)) {
        return -1;
    }
    return Encoder_init_internal(self, memoize, collect_buffers, registry, write_buffer_size);
}

static PyObject *
Encoder_get_memoize(EncoderObject *self, void *closure) {
    if (self->memoize)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject *
Encoder_get_collect_buffers(EncoderObject *self, void *closure) {
    if (self->collect_buffers)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyGetSetDef Encoder_getset[] = {
    {"memoize", (getter) Encoder_get_memoize, NULL,
     "The default ``memoize`` value for this encoder", NULL},
    {"collect_buffers", (getter) Encoder_get_collect_buffers, NULL,
     "The default ``collect_buffers`` value for this encoder", NULL},
    {NULL},
};

static PyTypeObject Encoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "quickle.Encoder",
    .tp_doc = Encoder__doc__,
    .tp_basicsize = sizeof(EncoderObject),
    .tp_dealloc = (destructor)Encoder_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)Encoder_traverse,
    .tp_clear = (inquiry)Encoder_clear,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Encoder_init,
    .tp_methods = Encoder_methods,
    .tp_getset = Encoder_getset,
};

/*************************************************************************
 * Decoder object                                                      *
 *************************************************************************/
typedef struct DecoderObject {
    PyObject_HEAD
    /* Static configuration */
    Py_ssize_t reset_stack_size;
    size_t reset_memo_size;
    Py_ssize_t reset_marks_size;
    PyObject *registry;

    /*Per-loads call*/
    Py_buffer buffer;
    char *input_buffer;
    Py_ssize_t input_len;
    Py_ssize_t next_read_idx;

    PyObject *buffers;          /* iterable of out-of-band buffers, or NULL */

    /* stack */
    PyObject **stack;
    Py_ssize_t fence;
    Py_ssize_t stack_allocated;
    Py_ssize_t stack_len;

    /* memo */
    PyObject **memo;
    size_t memo_allocated;      /* Capacity of the memo array */
    size_t memo_len;            /* Number of objects in the memo */

    /* marks */
    Py_ssize_t *marks;          /* Mark stack, used for deserializing container
                                   objects. */
    Py_ssize_t marks_allocated; /* Current allocated size of the mark stack. */
    Py_ssize_t marks_len;       /* Number of marks in the mark stack. */
} DecoderObject;

static int
Decoder_init_internal(DecoderObject *self, PyObject *registry)
{
    /* These could be made configurable later - these defaults should be good
     * for most users */
    self->reset_stack_size = 64;
    self->reset_memo_size = 64;
    self->reset_marks_size = 64;

    self->fence = 0;
    self->stack_len = 0;
    self->stack_allocated = 0;
    self->stack = NULL;

    self->memo_len = 0;
    self->memo_allocated = 0;
    self->memo = NULL;

    self->marks_len = 0;
    self->marks_allocated = 0;
    self->marks = NULL;

    self->buffers = NULL;
    self->buffer.buf = NULL;

    if (registry == NULL || registry == Py_None) {
        self->registry = NULL;
    }
    else if (PyList_CheckExact(registry) | PyDict_CheckExact(registry)) {
        self->registry = registry;
        Py_INCREF(registry);
    }
    else {
        PyErr_SetString(PyExc_TypeError, "registry must be a list or a dict");
        return -1;
    }
    return 0;
}

PyDoc_STRVAR(Decoder__doc__,
"Decoder(registry=None)\n"
"--\n"
"\n"
"A quickle decoder.\n"
"\n"
"\n"
"Creating a `Decoder` and calling the `Decoder.loads` method multiple times is more\n"
"efficient than calling `quickle.loads` multiple times.\n"
"\n"
"Parameters\n"
"----------\n"
"registry : list or dict, optional\n"
"    A registry of user-defined types this encoder instance should support. Can\n"
"    be either a list of types (recommended), or a dict mapping positive\n"
"    integers to each type. Note that for deserialization to be successful,\n"
"    the registry should match that of the corresponding `Encoder`.\n"
);
static int
Decoder_init(DecoderObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *registry = NULL;
    static char *kwlist[] = {"registry", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$O", kwlist, &registry)) {
        return -1;
    }
    return Decoder_init_internal(self, registry);
}

static void _Decoder_memo_clear(DecoderObject *self);
static void _Decoder_stack_clear(DecoderObject *self, Py_ssize_t clearto);

static int
Decoder_clear(DecoderObject *self)
{
    Py_CLEAR(self->registry);

    _Decoder_stack_clear(self, 0);
    PyMem_Free(self->stack);
    self->stack = NULL;

    _Decoder_memo_clear(self);
    PyMem_Free(self->memo);
    self->memo = NULL;

    PyMem_Free(self->marks);
    self->marks = NULL;

    Py_CLEAR(self->buffers);
    if (self->buffer.buf != NULL) {
        PyBuffer_Release(&self->buffer);
        self->buffer.buf = NULL;
    }
    return 0;
}

static void
Decoder_dealloc(DecoderObject *self)
{
    PyObject_GC_UnTrack((PyObject *)self);

    Decoder_clear(self);

    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Decoder_traverse(DecoderObject *self, visitproc visit, void *arg)
{
    Py_ssize_t i = self->stack_len;
    while (--i >= 0) {
        Py_VISIT(self->stack[i]);
    }
    Py_VISIT(self->buffers);
    Py_VISIT(self->registry);
    return 0;
}

static Py_ssize_t
bad_readline()
{
    QuickleState *st = quickle_get_global_state();
    PyErr_SetString(st->DecodingError, "quickle data was truncated");
    return -1;
}

static Py_ssize_t
_Decoder_Read(DecoderObject *self, char **s, Py_ssize_t n)
{
    if (n <= self->input_len - self->next_read_idx) {
        *s = self->input_buffer + self->next_read_idx;
        self->next_read_idx += n;
        return n;
    }
    return bad_readline();
}

static Py_ssize_t
_Decoder_ReadInto(DecoderObject *self, char *buf, Py_ssize_t n)
{
    Py_ssize_t in_buffer = self->input_len - self->next_read_idx;
    if (in_buffer > n) {
        memcpy(buf, self->input_buffer + self->next_read_idx, n);
        self->next_read_idx += n;
        return 0;
    }
    return bad_readline();
}

/* Retain only the initial clearto items.  If clearto >= the current
 * number of items, this is a (non-erroneous) NOP.
 */
static void
_Decoder_stack_clear(DecoderObject *self, Py_ssize_t clearto)
{
    Py_ssize_t i = self->stack_len;
    if (i > clearto) {
        while (--i >= clearto) {
            Py_CLEAR(self->stack[i]);
        }
        self->stack_len = clearto;
    }
}

static int
_Decoder_stack_grow(DecoderObject *self)
{
    PyObject **stack = self->stack;
    size_t allocated = (size_t)self->stack_allocated;
    size_t new_allocated;

    new_allocated = (allocated >> 3) + 6;
    /* check for integer overflow */
    if (new_allocated > (size_t)PY_SSIZE_T_MAX - allocated)
        goto nomemory;
    new_allocated += allocated;
    PyMem_Resize(stack, PyObject *, new_allocated);
    if (stack == NULL)
        goto nomemory;

    self->stack = stack;
    self->stack_allocated = (Py_ssize_t)new_allocated;
    return 0;

  nomemory:
    PyErr_NoMemory();
    return -1;
}

static int
_Decoder_stack_underflow(DecoderObject *self)
{
    QuickleState *st = quickle_get_global_state();
    PyErr_SetString(st->DecodingError,
                    self->marks_len ?
                    "unexpected MARK found" :
                    "decoder stack underflow");
    return -1;
}

static PyObject *
_Decoder_stack_pop(DecoderObject *self)
{
    if (self->stack_len <= self->fence) {
        _Decoder_stack_underflow(self);
        return NULL;
    }
    return self->stack[--self->stack_len];
}
/* Pop the top element and store it into V. On stack underflow,
 * DecodingError is raised and V is set to NULL.
 */
#define STACK_POP(U, V) do { (V) = _Decoder_stack_pop((U)); } while (0)

static int
_Decoder_stack_push(DecoderObject *self, PyObject *obj)
{
    if (self->stack_len == self->stack_allocated && _Decoder_stack_grow(self) < 0) {
        return -1;
    }
    self->stack[self->stack_len++] = obj;
    return 0;
}

/* Push an object on stack, transferring its ownership to the stack. */
#define STACK_PUSH(U, O) do {                               \
        if (_Decoder_stack_push((U), (O)) < 0) return -1; } while(0)

/* Push an object on stack, adding a new reference to the object. */
#define STACK_INCREF_PUSH(U, O) do {                             \
        Py_INCREF((O));                                         \
        if (_Decoder_stack_push((U), (O)) < 0) return -1; } while(0)

static PyObject *
_Decoder_stack_poptuple(DecoderObject *self, Py_ssize_t start)
{
    PyObject *tuple;
    Py_ssize_t len, i, j;

    if (start < self->fence) {
        _Decoder_stack_underflow(self);
        return NULL;
    }
    len = self->stack_len - start;
    tuple = PyTuple_New(len);
    if (tuple == NULL)
        return NULL;
    for (i = start, j = 0; j < len; i++, j++) {
        PyTuple_SET_ITEM(tuple, j, self->stack[i]);
    }
    self->stack_len = start;
    return tuple;
}

static PyObject *
_Decoder_stack_poplist(DecoderObject *self, Py_ssize_t start)
{
    PyObject *list;
    Py_ssize_t len, i, j;

    len = self->stack_len - start;
    list = PyList_New(len);
    if (list == NULL)
        return NULL;
    for (i = start, j = 0; j < len; i++, j++) {
        PyList_SET_ITEM(list, j, self->stack[i]);
    }
    self->stack_len = start;
    return list;
}

static int
_Decoder_memo_resize(DecoderObject *self, size_t new_size)
{
    size_t i;

    assert(new_size > self->memo_allocated);

    PyObject **memo_new = self->memo;
    PyMem_Resize(memo_new, PyObject *, new_size);
    if (memo_new == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    self->memo = memo_new;
    for (i = self->memo_allocated; i < new_size; i++)
        self->memo[i] = NULL;
    self->memo_allocated = new_size;
    return 0;
}

/* Returns NULL if idx is out of bounds. */
#define _Decoder_memo_get(self, idx) \
    (((size_t)(idx) >= (self)->memo_len) ? NULL : (self)->memo[(size_t)(idx)]);

/* Returns -1 (with an exception set) on failure, 0 on success.
   This takes its own reference to `value`. */
static int
_Decoder_memo_put(DecoderObject *self, size_t idx, PyObject *value)
{
    PyObject *old_item;

    if (idx >= self->memo_allocated) {
        if (_Decoder_memo_resize(self, idx * 2) < 0)
            return -1;
        assert(idx < self->memo_allocated);
    }
    Py_INCREF(value);
    old_item = self->memo[idx];
    self->memo[idx] = value;
    if (old_item != NULL) {
        Py_DECREF(old_item);
    }
    else {
        self->memo_len++;
    }
    return 0;
}

static void
_Decoder_memo_clear(DecoderObject *self)
{
    Py_ssize_t i;
    if (self->memo == NULL)
        return;
    i = self->memo_len;
    while (--i >= 0) {
        Py_CLEAR(self->memo[i]);
    }
    self->memo_len = 0;
}

static Py_ssize_t
marker(DecoderObject *self)
{
    Py_ssize_t mark;

    if (self->marks_len < 1) {
        QuickleState *st = quickle_get_global_state();
        PyErr_SetString(st->DecodingError, "could not find MARK");
        return -1;
    }

    mark = self->marks[--self->marks_len];
    self->fence = self->marks_len ?
            self->marks[self->marks_len - 1] : 0;
    return mark;
}

static int
load_none(DecoderObject *self)
{
    STACK_INCREF_PUSH(self, Py_None);
    return 0;
}

static int
load_bool(DecoderObject *self, PyObject *boolean)
{
    assert(boolean == Py_True || boolean == Py_False);
    STACK_INCREF_PUSH(self, boolean);
    return 0;
}

/* s contains x bytes of an unsigned little-endian integer.  Return its value
 * as a C Py_ssize_t, or -1 if it's higher than PY_SSIZE_T_MAX.
 */
static Py_ssize_t
calc_binsize(char *bytes, int nbytes)
{
    unsigned char *s = (unsigned char *)bytes;
    int i;
    size_t x = 0;

    if (nbytes > (int)sizeof(size_t)) {
        /* Check for integer overflow.  BINBYTES8 and BINUNICODE8 opcodes
         * have 64-bit size that can't be represented on 32-bit platform.
         */
        for (i = (int)sizeof(size_t); i < nbytes; i++) {
            if (s[i])
                return -1;
        }
        nbytes = (int)sizeof(size_t);
    }
    for (i = 0; i < nbytes; i++) {
        x |= (size_t) s[i] << (8 * i);
    }

    if (x > PY_SSIZE_T_MAX)
        return -1;
    else
        return (Py_ssize_t) x;
}

/* s contains x bytes of a little-endian integer.  Return its value as a
 * C int.  Obscure:  when x is 1 or 2, this is an unsigned little-endian
 * int, but when x is 4 it's a signed one.  This is a historical source
 * of x-platform bugs.
 */
static long
calc_binint(char *bytes, int nbytes)
{
    unsigned char *s = (unsigned char *)bytes;
    Py_ssize_t i;
    long x = 0;

    for (i = 0; i < nbytes; i++) {
        x |= (long)s[i] << (8 * i);
    }

    /* Unlike BININT1 and BININT2, BININT (more accurately BININT4)
     * is signed, so on a box with longs bigger than 4 bytes we need
     * to extend a BININT's sign bit to the full width.
     */
    if (SIZEOF_LONG > 4 && nbytes == 4) {
        x |= -(x & (1L << 31));
    }

    return x;
}

static int
load_binintx(DecoderObject *self, char *s, int size)
{
    PyObject *value;
    long x;

    x = calc_binint(s, size);

    if ((value = PyLong_FromLong(x)) == NULL)
        return -1;

    STACK_PUSH(self, value);
    return 0;
}

static int
load_binint(DecoderObject *self)
{
    char *s;

    if (_Decoder_Read(self, &s, 4) < 0)
        return -1;

    return load_binintx(self, s, 4);
}

static int
load_binint1(DecoderObject *self)
{
    char *s;

    if (_Decoder_Read(self, &s, 1) < 0)
        return -1;

    return load_binintx(self, s, 1);
}

static int
load_binint2(DecoderObject *self)
{
    char *s;

    if (_Decoder_Read(self, &s, 2) < 0)
        return -1;

    return load_binintx(self, s, 2);
}

/* 'size' bytes contain the # of bytes of little-endian 256's-complement
 * data following.
 */
static int
load_counted_long(DecoderObject *self, int size)
{
    PyObject *value;
    char *nbytes;
    char *pdata;

    assert(size == 1 || size == 4);
    if (_Decoder_Read(self, &nbytes, size) < 0)
        return -1;

    size = calc_binint(nbytes, size);
    if (size < 0) {
        QuickleState *st = quickle_get_global_state();
        /* Corrupt or hostile quickle -- we never write one like this */
        PyErr_SetString(st->DecodingError,
                        "LONG quickle has negative byte count");
        return -1;
    }

    if (size == 0)
        value = PyLong_FromLong(0L);
    else {
        /* Read the raw little-endian bytes and convert. */
        if (_Decoder_Read(self, &pdata, size) < 0)
            return -1;
        value = _PyLong_FromByteArray((unsigned char *)pdata, (size_t)size,
                                      1 /* little endian */ , 1 /* signed */ );
    }
    if (value == NULL)
        return -1;
    STACK_PUSH(self, value);
    return 0;
}

static int
load_binfloat(DecoderObject *self)
{
    PyObject *value;
    double x;
    char *s;

    if (_Decoder_Read(self, &s, 8) < 0)
        return -1;

    x = _PyFloat_Unpack8((unsigned char *)s, 0);
    if (x == -1.0 && PyErr_Occurred())
        return -1;

    if ((value = PyFloat_FromDouble(x)) == NULL)
        return -1;

    STACK_PUSH(self, value);
    return 0;
}

static int
load_complex(DecoderObject *self)
{
    PyObject *value;
    double real, imag;
    char *s;

    if (_Decoder_Read(self, &s, 16) < 0)
        return -1;

    real = _PyFloat_Unpack8((unsigned char *)s, 0);
    if (real == -1.0 && PyErr_Occurred())
        return -1;
    imag = _PyFloat_Unpack8((unsigned char *)(s + 8), 0);
    if (imag == -1.0 && PyErr_Occurred())
        return -1;

    value = PyComplex_FromDoubles(real, imag);
    if (value == NULL)
        return -1;

    STACK_PUSH(self, value);
    return 0;
}

static int
load_timedelta(DecoderObject *self)
{
    PyObject *value;
    int days, seconds, microseconds;
    char *s;

    if (_Decoder_Read(self, &s, 10) < 0)
        return -1;

    days = unpack_int(s, 0, 4);
    seconds = unpack_int(s, 4, 3);
    microseconds = unpack_int(s, 7, 3);

    value = PyDelta_FromDSU(days, seconds, microseconds);
    if (value == NULL)
        return -1;

    STACK_PUSH(self, value);
    return 0;
}

static int
load_date(DecoderObject *self)
{
    PyObject *value;
    int year, month, day;
    char *s;

    if (_Decoder_Read(self, &s, 4) < 0)
        return -1;

    year = unpack_int(s, 0, 2);
    month = unpack_int(s, 2, 1);
    day = unpack_int(s, 3, 1);

    value = PyDate_FromDate(year, month, day);
    if (value == NULL)
        return -1;

    STACK_PUSH(self, value);
    return 0;
}

static int
load_time(DecoderObject *self, int has_tzinfo)
{
    PyObject *tzinfo = Py_None, *value;
    int hour, minute, second, micro, fold;
    char *s;

    if (has_tzinfo) {
        STACK_POP(self, tzinfo);
        if (tzinfo == NULL)
            return -1;
    }

    if (_Decoder_Read(self, &s, 6) < 0)
        return -1;

    hour = unpack_int(s, 0, 1);
    minute = unpack_int(s, 1, 1);
    second = unpack_int(s, 2, 1);
    micro = unpack_int(s, 3, 3);
    fold = (hour & 128) ? 1: 0;
    hour = hour & 127;
    value = PyDateTimeAPI->Time_FromTimeAndFold(
        hour, minute, second, micro, tzinfo, fold, PyDateTimeAPI->TimeType
    );
    if (value == NULL)
        return -1;

    STACK_PUSH(self, value);
    return 0;
}

static int
load_datetime(DecoderObject *self, int has_tzinfo)
{
    PyObject *tzinfo = Py_None, *value;
    int year, month, day, hour, minute, second, micro, fold;
    char *s;

    if (has_tzinfo) {
        STACK_POP(self, tzinfo);
        if (tzinfo == NULL)
            return -1;
    }

    if (_Decoder_Read(self, &s, 10) < 0)
        return -1;

    year = unpack_int(s, 0, 2);
    month = unpack_int(s, 2, 1);
    day = unpack_int(s, 3, 1);
    hour = unpack_int(s, 4, 1);
    minute = unpack_int(s, 5, 1);
    second = unpack_int(s, 6, 1);
    micro = unpack_int(s, 7, 3);
    fold = (hour & 128) ? 1: 0;
    hour = hour & 127;
    value = PyDateTimeAPI->DateTime_FromDateAndTimeAndFold(
        year, month, day, hour, minute, second, micro,
        tzinfo, fold, PyDateTimeAPI->DateTimeType
    );
    if (value == NULL)
        return -1;

    STACK_PUSH(self, value);
    return 0;
}

static int
load_timezone_utc(DecoderObject *self)
{
    STACK_INCREF_PUSH(self, PyDateTime_TimeZone_UTC);
    return 0;
}

static int
load_timezone(DecoderObject *self)
{
    PyObject *value, *offset;
    int days, seconds, microseconds;
    char *s;

    if (_Decoder_Read(self, &s, 6) < 0)
        return -1;

    seconds = unpack_int(s, 0, 3);
    microseconds = unpack_int(s, 3, 3);
    days = (seconds & 8388608) ? -1: 0;
    seconds &= 8388607;

    offset = PyDelta_FromDSU(days, seconds, microseconds);
    if (offset == NULL)
        return -1;

    value = PyTimeZone_FromOffset(offset);
    if (value == NULL) {
        Py_DECREF(offset);
        return -1;
    }
    STACK_PUSH(self, value);
    return 0;
}

static int
load_zoneinfo(DecoderObject *self)
{
    QuickleState *st;
    PyObject *value, *key;

    STACK_POP(self, key);
    if (key == NULL)
        return -1;

    st = quickle_get_global_state();
    if (st->ZoneInfoType == NULL) {
        PyErr_SetString(st->DecodingError, "No module named 'zoneinfo'");
        return -1;
    }
    value = CALL_ONE_ARG((PyObject *)(st->ZoneInfoType), key);
    Py_DECREF(key);
    if (value == NULL)
        return -1;
    STACK_PUSH(self, value);
    return 0;
}

static int
load_counted_binbytes(DecoderObject *self, int nbytes)
{
    PyObject *bytes;
    Py_ssize_t size;
    char *s;

    if (_Decoder_Read(self, &s, nbytes) < 0)
        return -1;

    size = calc_binsize(s, nbytes);
    if (size < 0) {
        PyErr_Format(PyExc_OverflowError,
                     "BINBYTES exceeds system's maximum size of %zd bytes",
                     PY_SSIZE_T_MAX);
        return -1;
    }

    bytes = PyBytes_FromStringAndSize(NULL, size);
    if (bytes == NULL)
        return -1;
    if (_Decoder_ReadInto(self, PyBytes_AS_STRING(bytes), size) < 0) {
        Py_DECREF(bytes);
        return -1;
    }

    STACK_PUSH(self, bytes);
    return 0;
}

static int
load_counted_bytearray(DecoderObject *self)
{
    PyObject *bytearray;
    Py_ssize_t size;
    char *s;

    if (_Decoder_Read(self, &s, 8) < 0) {
        return -1;
    }

    size = calc_binsize(s, 8);
    if (size < 0) {
        PyErr_Format(PyExc_OverflowError,
                     "BYTEARRAY8 exceeds system's maximum size of %zd bytes",
                     PY_SSIZE_T_MAX);
        return -1;
    }

    bytearray = PyByteArray_FromStringAndSize(NULL, size);
    if (bytearray == NULL) {
        return -1;
    }
    if (_Decoder_ReadInto(self, PyByteArray_AS_STRING(bytearray), size) < 0) {
        Py_DECREF(bytearray);
        return -1;
    }

    STACK_PUSH(self, bytearray);
    return 0;
}

static int
load_next_buffer(DecoderObject *self)
{
    if (self->buffers == NULL) {
        QuickleState *st = quickle_get_global_state();
        PyErr_SetString(st->DecodingError,
                        "quickle stream refers to out-of-band data "
                        "but no *buffers* argument was given");
        return -1;
    }
    PyObject *buf = PyIter_Next(self->buffers);
    if (buf == NULL) {
        if (!PyErr_Occurred()) {
            QuickleState *st = quickle_get_global_state();
            PyErr_SetString(st->DecodingError,
                            "not enough out-of-band buffers");
        }
        return -1;
    }

    STACK_PUSH(self, buf);
    return 0;
}

static int
load_readonly_buffer(DecoderObject *self)
{
    Py_ssize_t len = self->stack_len;
    if (len <= self->fence) {
        return _Decoder_stack_underflow(self);
    }

    PyObject *obj = self->stack[len - 1];
    PyObject *view = PyMemoryView_FromObject(obj);
    if (view == NULL) {
        return -1;
    }
    if (!PyMemoryView_GET_BUFFER(view)->readonly) {
        /* Original object is writable */
        PyMemoryView_GET_BUFFER(view)->readonly = 1;
        self->stack[len - 1] = view;
        Py_DECREF(obj);
    }
    else {
        /* Original object is read-only, no need to replace it */
        Py_DECREF(view);
    }
    return 0;
}

static int
load_counted_binunicode(DecoderObject *self, int nbytes)
{
    PyObject *str;
    Py_ssize_t size;
    char *s;

    if (_Decoder_Read(self, &s, nbytes) < 0)
        return -1;

    size = calc_binsize(s, nbytes);
    if (size < 0) {
        PyErr_Format(PyExc_OverflowError,
                     "BINUNICODE exceeds system's maximum size of %zd bytes",
                     PY_SSIZE_T_MAX);
        return -1;
    }

    if (_Decoder_Read(self, &s, size) < 0)
        return -1;

    str = PyUnicode_DecodeUTF8(s, size, "surrogatepass");
    if (str == NULL)
        return -1;

    STACK_PUSH(self, str);
    return 0;
}

static int
load_counted_tuple(DecoderObject *self, Py_ssize_t len)
{
    PyObject *tuple;

    if (self->stack_len < len)
        return _Decoder_stack_underflow(self);

    tuple = _Decoder_stack_poptuple(self, self->stack_len - len);
    if (tuple == NULL)
        return -1;
    STACK_PUSH(self, tuple);
    return 0;
}

static int
load_tuple(DecoderObject *self)
{
    Py_ssize_t i;

    if ((i = marker(self)) < 0)
        return -1;

    return load_counted_tuple(self, self->stack_len - i);
}

static int
load_empty_list(DecoderObject *self)
{
    PyObject *list;

    if ((list = PyList_New(0)) == NULL)
        return -1;
    STACK_PUSH(self, list);
    return 0;
}

static int
load_empty_dict(DecoderObject *self)
{
    PyObject *dict;

    if ((dict = PyDict_New()) == NULL)
        return -1;
    STACK_PUSH(self, dict);
    return 0;
}

static int
load_empty_set(DecoderObject *self)
{
    PyObject *set;

    if ((set = PySet_New(NULL)) == NULL)
        return -1;
    STACK_PUSH(self, set);
    return 0;
}

static int
load_frozenset(DecoderObject *self)
{
    PyObject *items;
    PyObject *frozenset;
    Py_ssize_t i;

    if ((i = marker(self)) < 0)
        return -1;

    items = _Decoder_stack_poptuple(self, i);
    if (items == NULL)
        return -1;

    frozenset = PyFrozenSet_New(items);
    Py_DECREF(items);
    if (frozenset == NULL)
        return -1;

    STACK_PUSH(self, frozenset);
    return 0;
}

static int
load_pop(DecoderObject *self)
{
    Py_ssize_t len = self->stack_len;

    /* Note that we split the stack into two stacks, an object stack and a mark
     * stack. We have to be clever and pop the right one. We do this by looking
     * at the top of the mark stack first, and only signalling a stack
     * underflow if the object stack is empty and the mark stack doesn't match
     * our expectations.
     */
    if (self->marks_len > 0 && self->marks[self->marks_len - 1] == len) {
        self->marks_len--;
        self->fence = self->marks_len ? self->marks[self->marks_len - 1] : 0;
    } else if (len <= self->fence)
        return _Decoder_stack_underflow(self);
    else {
        len--;
        Py_DECREF(self->stack[len]);
        self->stack_len = len;
    }
    return 0;
}

static int
load_pop_mark(DecoderObject *self)
{
    Py_ssize_t i;

    if ((i = marker(self)) < 0)
        return -1;

    _Decoder_stack_clear(self, i);

    return 0;
}

static int
load_binget(DecoderObject *self)
{
    PyObject *value;
    Py_ssize_t idx;
    char *s;

    if (_Decoder_Read(self, &s, 1) < 0)
        return -1;

    idx = Py_CHARMASK(s[0]);

    value = _Decoder_memo_get(self, idx);
    if (value == NULL) {
        PyObject *key = PyLong_FromSsize_t(idx);
        if (key != NULL) {
            PyErr_SetObject(PyExc_KeyError, key);
            Py_DECREF(key);
        }
        return -1;
    }

    STACK_INCREF_PUSH(self, value);
    return 0;
}

static int
load_long_binget(DecoderObject *self)
{
    PyObject *value;
    Py_ssize_t idx;
    char *s;

    if (_Decoder_Read(self, &s, 4) < 0)
        return -1;

    idx = calc_binsize(s, 4);

    value = _Decoder_memo_get(self, idx);
    if (value == NULL) {
        PyObject *key = PyLong_FromSsize_t(idx);
        if (key != NULL) {
            PyErr_SetObject(PyExc_KeyError, key);
            Py_DECREF(key);
        }
        return -1;
    }

    STACK_INCREF_PUSH(self, value);
    return 0;
}

static int
load_memoize(DecoderObject *self)
{
    PyObject *value;

    if (self->stack_len <= self->fence)
        return _Decoder_stack_underflow(self);
    value = self->stack[self->stack_len - 1];

    return _Decoder_memo_put(self, self->memo_len, value);
}

#define raise_decoding_error(fmt, ...) \
    PyErr_Format(quickle_get_global_state()->DecodingError, (fmt), __VA_ARGS__)

static int
do_append(DecoderObject *self, Py_ssize_t x)
{
    PyObject *slice;
    PyObject *list;
    Py_ssize_t len;

    len = self->stack_len;
    if (x > len || x <= self->fence)
        return _Decoder_stack_underflow(self);
    if (len == x)  /* nothing to do */
        return 0;

    list = self->stack[x - 1];

    if (PyList_CheckExact(list)) {
        Py_ssize_t list_len;
        int ret;

        slice = _Decoder_stack_poplist(self, x);
        if (!slice)
            return -1;
        list_len = PyList_GET_SIZE(list);
        ret = PyList_SetSlice(list, list_len, list_len, slice);
        Py_DECREF(slice);
        return ret;
    }
    raise_decoding_error("Invalid APPEND(S) opcode on object of type %.200s", Py_TYPE(list)->tp_name);
    return -1;
}

static int
load_append(DecoderObject *self)
{
    if (self->stack_len - 1 <= self->fence)
        return _Decoder_stack_underflow(self);
    return do_append(self, self->stack_len - 1);
}

static int
load_appends(DecoderObject *self)
{
    Py_ssize_t i = marker(self);
    if (i < 0)
        return -1;
    return do_append(self, i);
}

static int
do_setitems(DecoderObject *self, Py_ssize_t x)
{
    PyObject *value, *key;
    PyObject *dict;
    Py_ssize_t len, i;
    int status = 0;

    len = self->stack_len;
    if (x > len || x <= self->fence)
        return _Decoder_stack_underflow(self);
    if (len == x)  /* nothing to do */
        return 0;
    if ((len - x) % 2 != 0) {
        QuickleState *st = quickle_get_global_state();
        /* Currupt or hostile message -- we never write one like this. */
        PyErr_SetString(st->DecodingError,
                        "odd number of items for SETITEMS");
        return -1;
    }

    dict = self->stack[x - 1];

    if (PyDict_CheckExact(dict)) {
        for (i = x + 1; i < len; i += 2) {
            key = self->stack[i - 1];
            value = self->stack[i];
            if (PyDict_SetItem(dict, key, value) < 0) {
                status = -1;
                break;
            }
        }
        _Decoder_stack_clear(self, x);
        return status;
    }
    raise_decoding_error("Invalid SETITEM(S) opcode on object of type %.200s", Py_TYPE(dict)->tp_name);
    return -1;
}

static int
load_setitem(DecoderObject *self)
{
    return do_setitems(self, self->stack_len - 2);
}

static int
load_setitems(DecoderObject *self)
{
    Py_ssize_t i = marker(self);
    if (i < 0)
        return -1;
    return do_setitems(self, i);
}

static int
load_additems(DecoderObject *self)
{
    PyObject *set;
    Py_ssize_t mark, len;

    mark =  marker(self);
    if (mark < 0)
        return -1;
    len = self->stack_len;
    if (mark > len || mark <= self->fence)
        return _Decoder_stack_underflow(self);
    if (len == mark)  /* nothing to do */
        return 0;

    set = self->stack[mark - 1];

    if (PySet_Check(set)) {
        PyObject *items;
        int status;

        items = _Decoder_stack_poptuple(self, mark);
        if (items == NULL)
            return -1;

        status = _PySet_Update(set, items);
        Py_DECREF(items);
        return status;
    }
    raise_decoding_error("Invalid ADDITEMS opcode on object of type %.200s", Py_TYPE(set)->tp_name);
    return 0;
}

static int
load_mark(DecoderObject *self)
{
    if (self->marks_len >= self->marks_allocated) {
        size_t alloc = ((size_t)self->marks_len << 1) + 20;
        Py_ssize_t *marks_new = self->marks;
        PyMem_Resize(marks_new, Py_ssize_t, alloc);
        if (marks_new == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        self->marks = marks_new;
        self->marks_allocated = (Py_ssize_t)alloc;
    }

    self->marks[self->marks_len++] = self->fence = self->stack_len;

    return 0;
}

static PyObject *
load_from_registry(DecoderObject *self, int nbytes, Py_ssize_t *out_code) {
    char *s;
    Py_ssize_t code;
    PyObject *typ = NULL;

    if (_Decoder_Read(self, &s, nbytes) < 0)
        return NULL;

    code = calc_binsize(s, nbytes);
    *out_code = code;

    if (self->registry != NULL) {
        if (PyList_CheckExact(self->registry)) {
            typ = PyList_GetItem(self->registry, code);
        }
        else {
            PyObject *py_code = PyLong_FromSsize_t(code);
            if (py_code == NULL)
                return NULL;

            typ = PyDict_GetItem(self->registry, py_code);
            Py_DECREF(py_code);
        }
    }
    if (typ == NULL) {
        PyErr_Format(PyExc_ValueError, "Typecode %zd isn't in type registry", code);
    }
    return typ;
}

static int
load_struct(DecoderObject *self, int nbytes)
{
    Py_ssize_t code;
    PyObject *typ, *obj;

    typ = load_from_registry(self, nbytes, &code);
    if (typ == NULL)
        return -1;
    if (!PyType_Check(typ) || Py_TYPE(typ) != &StructMetaType) {
        PyErr_Format(PyExc_TypeError,
                     "Value for typecode %zd isn't a Struct type",
                     code);
        return -1;
    }

    obj = ((PyTypeObject *)(typ))->tp_alloc((PyTypeObject *)typ, 0);
    if (obj == NULL)
        return -1;
    STACK_PUSH(self, obj);
    return 0;
}


static int
load_buildstruct(DecoderObject *self)
{
    PyObject *fields, *defaults, *field, *val, *obj;
    Py_ssize_t start, nargs, nfields, npos, i;
    int should_untrack;

    start = marker(self);
    if (start < 0)
        return -1;
    if (start > self->stack_len || start <= self->fence)
        return _Decoder_stack_underflow(self);

    obj = self->stack[start - 1];

    if (!(Py_TYPE(Py_TYPE(obj)) == &StructMetaType)) {
        raise_decoding_error("Invalid BUILDSTRUCT opcode on object of type %.200s",
                             Py_TYPE(obj)->tp_name);
        return -1;
    }

    fields = StructMeta_GET_FIELDS(Py_TYPE(obj));
    defaults = StructMeta_GET_DEFAULTS(Py_TYPE(obj));
    nfields = PyTuple_GET_SIZE(fields);
    npos = nfields - PyTuple_GET_SIZE(defaults);
    nargs = self->stack_len - start;

    should_untrack = PyObject_IS_GC(obj);

    /* Drop extra trailing args, if any */
    for (i = 0; i < (nargs - nfields); i++) {
        Py_DECREF(self->stack[--self->stack_len]);
    }

    /* Apply remaining args */
    for (i = nfields - 1; i >= 0; i--) {
        field = PyTuple_GET_ITEM(fields, i);
        if (i < nargs) {
            val = self->stack[--self->stack_len];
        }
        else if (i < npos) {
            PyErr_Format(
                PyExc_TypeError,
                "Missing required argument '%U'",
                field
            );
            return -1;
        }
        else {
            val = maybe_deepcopy_default(PyTuple_GET_ITEM(defaults, i - npos), NULL);
            if (val == NULL)
                return -1;
        }
        Struct_set_index(obj, i, val);
        if (should_untrack) {
            should_untrack = !OBJ_IS_GC(val);
        }
    }
    if (should_untrack)
        PyObject_GC_UnTrack(obj);
    return 0;
}

static int
load_enum(DecoderObject *self, int nbytes)
{
    Py_ssize_t code;
    QuickleState *st;
    PyObject *val = NULL, *obj = NULL, *typ, *member_table;

    typ = load_from_registry(self, nbytes, &code);
    if (typ == NULL)
        return -1;
    st = quickle_get_global_state();
    if (!(PyType_Check(typ) && PyType_IsSubtype((PyTypeObject *)typ, st->EnumType))) {
        PyErr_Format(PyExc_TypeError,
                     "Value for typecode %zd isn't an Enum type",
                     code);
        return -1;
    }

    STACK_POP(self, val);
    if (val == NULL)
        return -1;

    /* IntEnums are serialized by value, all other enums are serialized by name */
    if (PyLong_CheckExact(val)) {
        /* Fast path for common case. This accesses a non-public member of the
         * enum class to speedup lookups. If this fails, we clear errors and
         * use the slower-but-more-public method instead. */
        member_table = PyObject_GetAttr(typ, st->value2member_map_str);
        if (member_table != NULL) {
            obj = PyDict_GetItem(member_table, val);
            Py_DECREF(member_table);
            Py_XINCREF(obj);
        }
        if (obj == NULL) {
            PyErr_Clear();
            obj = CALL_ONE_ARG(typ, val);
        }
    }
    else {
        obj = PyObject_GetAttr(typ, val);
    }
    Py_DECREF(val);
    if (obj == NULL)
        return -1;
    STACK_PUSH(self, obj);
    return 0;
}

/* No-op, unsupported opcodes will be detected elsewhere */
static int
load_proto(DecoderObject *self)
{
    char *s;
    if (_Decoder_Read(self, &s, 1) < 0)
        return -1;
    return 0;
}

/* No-op, we ignore frame markers. Buffer is already in memory */
static int
load_frame(DecoderObject *self)
{
    char *s;
    if (_Decoder_Read(self, &s, 8) < 0)
        return -1;
    return 0;
}

static PyObject *
load(DecoderObject *self)
{
    PyObject *value = NULL;
    char *s = NULL;

    /* Convenient macros for the dispatch while-switch loop just below. */
#define OP(opcode, load_func) \
    case opcode: if (load_func(self) < 0) break; continue;

#define OP_ARG(opcode, load_func, arg) \
    case opcode: if (load_func(self, (arg)) < 0) break; continue;

    while (1) {
        if (_Decoder_Read(self, &s, 1) < 0) {
            return NULL;
        }

        switch ((enum opcode)s[0]) {
        OP(NONE, load_none)
        OP(BININT, load_binint)
        OP(BININT1, load_binint1)
        OP(BININT2, load_binint2)
        OP_ARG(LONG1, load_counted_long, 1)
        OP_ARG(LONG4, load_counted_long, 4)
        OP(BINFLOAT, load_binfloat)
        OP_ARG(SHORT_BINBYTES, load_counted_binbytes, 1)
        OP_ARG(BINBYTES, load_counted_binbytes, 4)
        OP_ARG(BINBYTES8, load_counted_binbytes, 8)
        OP(BYTEARRAY8, load_counted_bytearray)
        OP(NEXT_BUFFER, load_next_buffer)
        OP(READONLY_BUFFER, load_readonly_buffer)
        OP_ARG(SHORT_BINUNICODE, load_counted_binunicode, 1)
        OP_ARG(BINUNICODE, load_counted_binunicode, 4)
        OP_ARG(BINUNICODE8, load_counted_binunicode, 8)
        OP_ARG(EMPTY_TUPLE, load_counted_tuple, 0)
        OP_ARG(TUPLE1, load_counted_tuple, 1)
        OP_ARG(TUPLE2, load_counted_tuple, 2)
        OP_ARG(TUPLE3, load_counted_tuple, 3)
        OP(TUPLE, load_tuple)
        OP(EMPTY_LIST, load_empty_list)
        OP(EMPTY_DICT, load_empty_dict)
        OP(EMPTY_SET, load_empty_set)
        OP(ADDITEMS, load_additems)
        OP(FROZENSET, load_frozenset)
        OP(APPEND, load_append)
        OP(APPENDS, load_appends)
        OP(BINGET, load_binget)
        OP(LONG_BINGET, load_long_binget)
        OP(MARK, load_mark)
        OP(MEMOIZE, load_memoize)
        OP(POP, load_pop)
        OP(POP_MARK, load_pop_mark)
        OP(SETITEM, load_setitem)
        OP(SETITEMS, load_setitems)
        OP(BUILDSTRUCT, load_buildstruct)
        OP_ARG(STRUCT1, load_struct, 1)
        OP_ARG(STRUCT2, load_struct, 2)
        OP_ARG(STRUCT4, load_struct, 4)
        OP_ARG(ENUM1, load_enum, 1)
        OP_ARG(ENUM2, load_enum, 2)
        OP_ARG(ENUM4, load_enum, 4)
        OP(COMPLEX, load_complex)
        OP(TIMEDELTA, load_timedelta)
        OP(DATE, load_date)
        OP_ARG(TIME, load_time, 0)
        OP_ARG(TIME_TZ, load_time, 1)
        OP_ARG(DATETIME, load_datetime, 0)
        OP_ARG(DATETIME_TZ, load_datetime, 1)
        OP(TIMEZONE_UTC, load_timezone_utc)
        OP(TIMEZONE, load_timezone)
        OP(ZONEINFO, load_zoneinfo)
        OP(PROTO, load_proto)
        OP(FRAME, load_frame)
        OP_ARG(NEWTRUE, load_bool, Py_True)
        OP_ARG(NEWFALSE, load_bool, Py_False)

        case STOP:
            break;

        default:
            {
                QuickleState *st = quickle_get_global_state();
                unsigned char c = (unsigned char) *s;
                if (0x20 <= c && c <= 0x7e && c != '\'' && c != '\\') {
                    PyErr_Format(st->DecodingError,
                                 "invalid load key, '%c'.", c);
                }
                else {
                    PyErr_Format(st->DecodingError,
                                 "invalid load key, '\\x%02x'.", c);
                }
                return NULL;
            }
        }

        break;                  /* and we are done! */
    }

    if (PyErr_Occurred()) {
        return NULL;
    }

    STACK_POP(self, value);
    return value;
}

static PyObject*
Decoder_sizeof(DecoderObject *self)
{
    Py_ssize_t res;

    res = sizeof(DecoderObject);
    if (self->stack != NULL)
        res += self->stack_allocated * sizeof(PyObject *);
    if (self->memo != NULL)
        res += self->memo_allocated * sizeof(PyObject *);
    if (self->marks != NULL)
        res += self->marks_allocated * sizeof(Py_ssize_t);
    return PyLong_FromSsize_t(res);
}

static PyObject*
Decoder_loads_internal(DecoderObject *self, PyObject *data, PyObject *buffers) {
    PyObject *res = NULL;

    if (PyObject_GetBuffer(data, &self->buffer, PyBUF_CONTIG_RO) < 0) {
        goto cleanup;
    }
    self->input_buffer = self->buffer.buf;
    self->input_len = self->buffer.len;
    self->next_read_idx = 0;

    if (buffers == NULL || buffers == Py_None) {
        self->buffers = NULL;
    }
    else {
        self->buffers = PyObject_GetIter(buffers);
        if (self->buffers == NULL) {
            goto cleanup;
        }
    }

    if (self->stack == NULL) {
        self->stack_allocated = 8;
        self->stack = PyMem_Malloc(self->stack_allocated * sizeof(PyObject *));
        if (self->stack == NULL) {
            PyErr_NoMemory();
            goto cleanup;
        }
    }
    if (self->memo == NULL) {
        self->memo_allocated = 32;
        self->memo = PyMem_Calloc(self->memo_allocated, sizeof(PyObject *));
        if (self->memo == NULL) {
            PyErr_NoMemory();
            goto cleanup;
        }
    }

    res = load(self);

cleanup:
    if (self->buffer.buf != NULL) {
        PyBuffer_Release(&self->buffer);
        self->buffer.buf = NULL;
        self->input_buffer = NULL;
    }
    Py_CLEAR(self->buffers);
    /* Reset stack, deallocates if allocation exceeded limit */
    _Decoder_stack_clear(self, 0);
    if (self->stack_allocated > self->reset_stack_size) {
        PyMem_Free(self->stack);
        self->stack = NULL;
    }
    /* Reset memo, deallocates if allocation exceeded limit */
    _Decoder_memo_clear(self);
    if (self->memo_allocated > self->reset_memo_size) {
        PyMem_Free(self->memo);
        self->memo = NULL;
    }
    /* Reset marks, deallocates if allocation exceeded limit */
    self->marks_len = 0;
    if (self->marks_allocated > self->reset_marks_size) {
        PyMem_Free(self->marks);
        self->marks = NULL;
    }
    return res;
}

static char *Decoder_loads_kws[] = {"buffers", NULL};

PyDoc_STRVAR(Decoder_loads__doc__,
"loads(self, data, *, buffers=None)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"data : bytes\n"
"    The serialized data\n"
"buffers : iterable of bytes, optional\n"
"    An iterable of out-of-band buffers generated by passing\n"
"    ``collect_buffers=True`` to the corresponding `Encoder.dumps` call.\n"
"\n"
"Returns\n"
"-------\n"
"obj : object\n"
"    The deserialized object"
);
static PyObject*
Decoder_loads(DecoderObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *data = NULL;
    PyObject *buffers = NULL;
    QuickleState *st = quickle_get_global_state();

    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }
    data = args[0];
    if (kwnames != NULL) {
        if (!parse_keywords(kwnames, args + nargs, st->decoder_loads_kws, &buffers)) {
            return NULL;
        }
    }
    return Decoder_loads_internal(self, data, buffers);
}

static struct PyMethodDef Decoder_methods[] = {
    {
        "loads", (PyCFunction) Decoder_loads, METH_FASTCALL | METH_KEYWORDS,
        Decoder_loads__doc__,
    },
    {
        "__sizeof__", (PyCFunction) Decoder_sizeof, METH_NOARGS,
        PyDoc_STR("Size in bytes")
    },
    {NULL, NULL}                /* sentinel */
};

static PyTypeObject Decoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "quickle.Decoder",
    .tp_doc = Decoder__doc__,
    .tp_basicsize = sizeof(DecoderObject),
    .tp_dealloc = (destructor)Decoder_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)Decoder_traverse,
    .tp_clear = (inquiry)Decoder_clear,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) Decoder_init,
    .tp_methods = Decoder_methods,
};


/*************************************************************************
 * Module-level definitions                                              *
 *************************************************************************/

PyDoc_STRVAR(quickle_dumps__doc__,
"dumps(obj, *, memoize=True, collect_buffers=False, registry=None)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"memoize : bool, optional\n"
"    Whether to enable memoization. If True (default), any duplicate objects will\n"
"    only be serialized once. Disabling memoization can be more efficient for\n"
"    objects unlikely to contain duplicate values, but self-referential objects\n"
"    will then fail to serialize.\n"
"collect_buffers : bool, optional\n"
"    Whether to collect out-of-band buffers. If True, the return value will be\n"
"    the serialized bytes, and a list of any `PickleBuffer` objects found (or\n"
"    `None` if none are found).\n"
"registry : list or dict, optional\n"
"    A registry of user-defined types to support. Can be either a list of types\n"
"    (recommended), or a dict mapping the type to a unique positive integer.\n"
"    Note that for deserialization to be successful, the registry should match\n"
"    that of the corresponding `loads`.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object\n"
"buffers : list of `PickleBuffer` or `None`, optional\n"
"    If ``collect_buffers`` is `True`, a list of out-of-band buffers will\n"
"    also be returned (or None if no buffers are found). Not returned if\n"
"    ``collect_buffers`` is `False`\n"
"\n"
"See Also\n"
"--------\n"
"Encoder.dumps"
);
static PyObject*
quickle_dumps(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"obj", "memoize", "collect_buffers", "registry", NULL};

    PyObject *obj = NULL;
    int memoize = 1;
    int collect_buffers = 0;
    PyObject *registry = NULL;
    EncoderObject *encoder;
    PyObject *res = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|$ppO", kwlist,
                                     &obj,
                                     &memoize,
                                     &collect_buffers,
                                     &registry)) {
        return NULL;
    }

    encoder = PyObject_GC_New(EncoderObject, &Encoder_Type);
    if (encoder == NULL) {
        return NULL;
    }
    if (Encoder_init_internal(encoder, memoize, collect_buffers, registry, 32) == 0) {
        res = Encoder_dumps_internal(encoder, obj);
    }

    Py_DECREF(encoder);
    return res;
}

PyDoc_STRVAR(quickle_loads__doc__,
"loads(data, *, buffers=None, registry=None)\n"
"--\n"
"\n"
"Deserialize an object from bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"buffers : iterable of bytes, optional\n"
"    An iterable of out-of-band buffers generated by passing\n"
"    ``collect_buffers=True`` to the corresponding `dumps` call.\n"
"registry : list or dict, optional\n"
"    A registry of user-defined types to support. Can be either a list of types\n"
"    (recommended), or a dict mapping positive integers to each type. Note that\n"
"    for deserialization to be successful, the registry should match that of the\n"
"    corresponding `dumps`.\n"
"\n"
"Returns\n"
"-------\n"
"obj : object\n"
"    The deserialized object.\n"
"\n"
"See Also\n"
"--------\n"
"Decoder.loads"
);
static PyObject*
quickle_loads(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"data", "buffers", "registry", NULL};

    PyObject *data = NULL;
    PyObject *buffers = NULL;
    PyObject *res = NULL;
    PyObject *registry = NULL;
    DecoderObject *decoder;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|$OO", kwlist,
                                     &data, &buffers, &registry)) {
        return NULL;
    }

    decoder = PyObject_GC_New(DecoderObject, &Decoder_Type);
    if (decoder == NULL) {
        return NULL;
    }
    if (Decoder_init_internal(decoder, registry) == 0) {
        res = Decoder_loads_internal(decoder, data, buffers);
    }

    Py_DECREF(decoder);
    return res;
}

static struct PyMethodDef quickle_methods[] = {
    {
        "dumps", (PyCFunction) quickle_dumps, METH_VARARGS | METH_KEYWORDS,
        quickle_dumps__doc__,
    },
    {
        "loads", (PyCFunction) quickle_loads, METH_VARARGS | METH_KEYWORDS,
        quickle_loads__doc__,
    },
    {NULL, NULL} /* sentinel */
};

static int
quickle_clear(PyObject *m)
{
    QuickleState *st = quickle_get_state(m);
    Py_CLEAR(st->QuickleError);
    Py_CLEAR(st->EncodingError);
    Py_CLEAR(st->DecodingError);
    Py_CLEAR(st->StructType);
    Py_CLEAR(st->EnumType);
    Py_CLEAR(st->TimeZoneType);
    Py_CLEAR(st->ZoneInfoType);
    Py_CLEAR(st->encoder_dumps_kws);
    Py_CLEAR(st->decoder_loads_kws);
    Py_CLEAR(st->value2member_map_str);
    Py_CLEAR(st->name_str);
    return 0;
}

static void
quickle_free(PyObject *m)
{
    quickle_clear(m);
}

static int
quickle_traverse(PyObject *m, visitproc visit, void *arg)
{
    QuickleState *st = quickle_get_state(m);
    Py_VISIT(st->QuickleError);
    Py_VISIT(st->EncodingError);
    Py_VISIT(st->DecodingError);
    Py_VISIT(st->StructType);
    Py_VISIT(st->EnumType);
    Py_VISIT(st->TimeZoneType);
    Py_VISIT(st->ZoneInfoType);
    return 0;
}

static struct PyModuleDef quicklemodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "quickle",
    .m_doc = quickle__doc__,
    .m_size = sizeof(QuickleState),
    .m_methods = quickle_methods,
    .m_traverse = quickle_traverse,
    .m_clear = quickle_clear,
    .m_free =(freefunc)quickle_free
};

PyMODINIT_FUNC
PyInit_quickle(void)
{
    PyObject *m, *temp_module, *temp_type;
    QuickleState *st;

    PyDateTime_IMPORT;

    m = PyState_FindModule(&quicklemodule);
    if (m) {
        Py_INCREF(m);
        return m;
    }

    if (PyType_Ready(&Decoder_Type) < 0)
        return NULL;
    if (PyType_Ready(&Encoder_Type) < 0)
        return NULL;
    StructMetaType.tp_base = &PyType_Type;
    if (PyType_Ready(&StructMetaType) < 0)
        return NULL;
    if (PyType_Ready(&StructMixinType) < 0)
        return NULL;

    /* Create the module and add the functions. */
    m = PyModule_Create(&quicklemodule);
    if (m == NULL)
        return NULL;

    /* Add constants */
    if (PyModule_AddStringConstant(m, "__version__", QUICKLE_VERSION) < 0)
        return NULL;

    /* Add types */
    Py_INCREF(&Encoder_Type);
    if (PyModule_AddObject(m, "Encoder", (PyObject *)&Encoder_Type) < 0)
        return NULL;
    Py_INCREF(&Decoder_Type);
    if (PyModule_AddObject(m, "Decoder", (PyObject *)&Decoder_Type) < 0)
        return NULL;
    Py_INCREF(&PyPickleBuffer_Type);
    if (PyModule_AddObject(m, "PickleBuffer", (PyObject *)&PyPickleBuffer_Type) < 0)
        return NULL;

    st = quickle_get_state(m);

    /* Initialize the Struct Type */
    st->StructType = PyObject_CallFunction(
        (PyObject *)&StructMetaType, "s(O){ssss}", "Struct", &StructMixinType,
        "__module__", "quickle", "__doc__", Struct__doc__
    );
    if (st->StructType == NULL)
        return NULL;
    Py_INCREF(st->StructType);
    if (PyModule_AddObject(m, "Struct", st->StructType) < 0)
        return NULL;

    /* Get the EnumType */
    temp_module = PyImport_ImportModule("enum");
    if (temp_module == NULL)
        return NULL;
    temp_type = PyObject_GetAttrString(temp_module, "Enum");
    Py_DECREF(temp_module);
    if (temp_type == NULL)
        return NULL;
    if (!PyType_Check(temp_type)) {
        Py_DECREF(temp_type);
        PyErr_SetString(PyExc_TypeError, "enum.Enum should be a type");
        return NULL;
    }
    st->EnumType = (PyTypeObject *)temp_type;

    /* Get the TimeZoneType */
    temp_module = PyImport_ImportModule("datetime");
    if (temp_module == NULL)
        return NULL;
    temp_type = PyObject_GetAttrString(temp_module, "timezone");
    Py_DECREF(temp_module);
    if (temp_type == NULL)
        return NULL;
    if (!PyType_Check(temp_type)) {
        Py_DECREF(temp_type);
        PyErr_SetString(PyExc_TypeError, "datetime.timezone should be a type");
        return NULL;
    }
    st->TimeZoneType = (PyTypeObject *)temp_type;

    /* Get the ZoneInfoType if present */
#if PY_VERSION_HEX >= 0x03090000
    temp_module = PyImport_ImportModule("zoneinfo");
    if (temp_module == NULL)
        return NULL;
    temp_type = PyObject_GetAttrString(temp_module, "ZoneInfo");
    Py_DECREF(temp_module);
    if (temp_type == NULL)
        return NULL;
    if (!PyType_Check(temp_type)) {
        Py_DECREF(temp_type);
        PyErr_SetString(PyExc_TypeError, "zoneinfo.ZoneInfo should be a type");
        return NULL;
    }
    st->ZoneInfoType = (PyTypeObject *)temp_type;
#else
    st->ZoneInfoType = NULL;
#endif

    /* Initialize the exceptions. */
    st->QuickleError = PyErr_NewExceptionWithDoc(
        "quickle.QuickleError",
        "Base class for all Quickle protocol exceptions",
        NULL, NULL
    );
    if (st->QuickleError == NULL)
        return NULL;
    st->EncodingError = \
        PyErr_NewExceptionWithDoc(
            "quickle.EncodingError",
            "A protocol error occurred while encoding an object",
            st->QuickleError, NULL
        );
    if (st->EncodingError == NULL)
        return NULL;
    st->DecodingError = \
        PyErr_NewExceptionWithDoc(
            "quickle.DecodingError",
            "A protocol error occurred while decoding an object",
            st->QuickleError, NULL
        );
    if (st->DecodingError == NULL)
        return NULL;

    Py_INCREF(st->QuickleError);
    if (PyModule_AddObject(m, "QuickleError", st->QuickleError) < 0)
        return NULL;
    Py_INCREF(st->EncodingError);
    if (PyModule_AddObject(m, "EncodingError", st->EncodingError) < 0)
        return NULL;
    Py_INCREF(st->DecodingError);
    if (PyModule_AddObject(m, "DecodingError", st->DecodingError) < 0)
        return NULL;

    /* Initialize cached constant strings and tuples */
    st->encoder_dumps_kws = make_keyword_tuple(Encoder_dumps_kws);
    if (st->encoder_dumps_kws == NULL)
        return NULL;
    st->decoder_loads_kws = make_keyword_tuple(Decoder_loads_kws);
    if (st->decoder_loads_kws == NULL)
        return NULL;
    st->value2member_map_str = PyUnicode_InternFromString("_value2member_map_");
    if (st->value2member_map_str == NULL)
        return NULL;
    st->name_str = PyUnicode_InternFromString("name");
    if (st->name_str == NULL)
        return NULL;

    return m;
}
