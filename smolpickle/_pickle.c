#include "Python.h"
#include "structmember.h"

PyDoc_STRVAR(pickle__doc__,
"smolpickle - like 'pickle', but smol.\n\n");

enum {
    LOWEST_PROTOCOL = 5,
    HIGHEST_PROTOCOL = 5,
    DEFAULT_PROTOCOL = 5
};

enum opcode {
    MARK            = '(',
    STOP            = '.',
    POP             = '0',
    POP_MARK        = '1',
    BININT          = 'J',
    BININT1         = 'K',
    BININT2         = 'M',
    NONE            = 'N',
    BINUNICODE      = 'X',
    APPEND          = 'a',
    EMPTY_DICT      = '}',
    APPENDS         = 'e',
    BINGET          = 'h',
    LONG_BINGET     = 'j',
    EMPTY_LIST      = ']',
    BINPUT          = 'q',
    LONG_BINPUT     = 'r',
    SETITEM         = 's',
    TUPLE           = 't',
    EMPTY_TUPLE     = ')',
    SETITEMS        = 'u',
    BINFLOAT        = 'G',

    /* Protocol 2. */
    PROTO       = '\x80',
    TUPLE1      = '\x85',
    TUPLE2      = '\x86',
    TUPLE3      = '\x87',
    NEWTRUE     = '\x88',
    NEWFALSE    = '\x89',
    LONG1       = '\x8a',
    LONG4       = '\x8b',

    /* Protocol 3 (Python 3.x) */
    BINBYTES       = 'B',
    SHORT_BINBYTES = 'C',

    /* Protocol 4 */
    SHORT_BINUNICODE = '\x8c',
    BINUNICODE8      = '\x8d',
    BINBYTES8        = '\x8e',
    EMPTY_SET        = '\x8f',
    ADDITEMS         = '\x90',
    FROZENSET        = '\x91',
    MEMOIZE          = '\x94',
    FRAME            = '\x95',

    /* Protocol 5 */
    BYTEARRAY8       = '\x96',
    NEXT_BUFFER      = '\x97',
    READONLY_BUFFER  = '\x98',

    /* Unsupported */
    DUP             = '2',
    FLOAT           = 'F',
    INT             = 'I',
    LONG            = 'L',
    REDUCE          = 'R',
    STRING          = 'S',
    BINSTRING       = 'T',
    SHORT_BINSTRING = 'U',
    UNICODE         = 'V',
    PERSID          = 'P',
    BINPERSID       = 'Q',
    GLOBAL          = 'c',
    BUILD           = 'b',
    DICT            = 'd',
    LIST            = 'l',
    OBJ             = 'o',
    INST            = 'i',
    PUT             = 'p',
    GET             = 'g',
    NEWOBJ      = '\x81',
    EXT1        = '\x82',
    EXT2        = '\x83',
    EXT4        = '\x84',
    NEWOBJ_EX        = '\x92',
    STACK_GLOBAL     = '\x93',
};

enum {
   /* Number of elements save_list/dict/set writes out before
    * doing APPENDS/SETITEMS/ADDITEMS. */
    BATCHSIZE = 1000,
};

/*************************************************************************
 * Module level state                                                    *
 *************************************************************************/

/* State of the pickle module, per PEP 3121. */
typedef struct {
    /* Exception classes for pickle. */
    PyObject *PickleError;
    PyObject *PicklingError;
    PyObject *UnpicklingError;
} PickleState;

/* Forward declaration of the _pickle module definition. */
static struct PyModuleDef _picklemodule;

/* Given a module object, get its per-module state. */
static PickleState *
_Pickle_GetState(PyObject *module)
{
    return (PickleState *)PyModule_GetState(module);
}

/* Find the module instance imported in the currently running sub-interpreter
   and get its state. */
static PickleState *
_Pickle_GetGlobalState()
{
    return _Pickle_GetState(PyState_FindModule(&_picklemodule));
}

/* Clear the given pickle module state. */
static void
_Pickle_ClearState(PickleState *st)
{
    Py_CLEAR(st->PickleError);
    Py_CLEAR(st->PicklingError);
    Py_CLEAR(st->UnpicklingError);
}


/*************************************************************************
 * MemoTable object                                                      *
 *************************************************************************/

/* A custom hashtable mapping void* to Python ints. This is used by the pickler
 for memoization. Using a custom hashtable rather than PyDict allows us to skip
 a bunch of unnecessary object creation. This makes a huge performance
 difference. */
typedef struct {
    PyObject *key;
    Py_ssize_t value;
} MemoEntry;

typedef struct {
    size_t mask;
    size_t used;
    size_t allocated;
    size_t buffered_size;
    MemoEntry *table;
} MemoTable;

#define MT_MINSIZE 8
#define PERTURB_SHIFT 5

static MemoTable *
MemoTable_New(Py_ssize_t buffered_size)
{
    MemoTable *memo = PyMem_Malloc(sizeof(MemoTable));
    if (memo == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    memo->buffered_size = MT_MINSIZE;
    if (buffered_size > 0) {
        /* Find the smallest valid table size >= buffered_size. */
        while (memo->buffered_size < (size_t)buffered_size) {
            memo->buffered_size <<= 1;
        }
    }
    memo->used = 0;
    memo->allocated = MT_MINSIZE;
    memo->mask = MT_MINSIZE - 1;
    memo->table = PyMem_Malloc(MT_MINSIZE * sizeof(MemoEntry));
    if (memo->table == NULL) {
        PyMem_Free(memo);
        PyErr_NoMemory();
        return NULL;
    }
    memset(memo->table, 0, MT_MINSIZE * sizeof(MemoEntry));

    return memo;
}

static Py_ssize_t
MemoTable_Size(MemoTable *self)
{
    return self->used;
}

static int
MemoTable_Clear(MemoTable *self)
{
    Py_ssize_t i = self->allocated;

    while (--i >= 0) {
        Py_XDECREF(self->table[i].key);
    }
    self->used = 0;
    memset(self->table, 0, self->allocated * sizeof(MemoEntry));
    return 0;
}

static void
MemoTable_Del(MemoTable *self)
{
    MemoTable_Clear(self);
    PyMem_Free(self->table);
    PyMem_Free(self);
}

/* Since entries cannot be deleted from this hashtable, _MemoTable_Lookup()
   can be considerably simpler than dictobject.c's lookdict(). */
static MemoEntry *
_MemoTable_Lookup(MemoTable *self, PyObject *key)
{
    size_t i;
    size_t perturb;
    size_t mask = self->mask;
    MemoEntry *table = self->table;
    MemoEntry *entry;
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
_MemoTable_Resize(MemoTable *self, size_t min_size)
{
    MemoEntry *oldtable = NULL;
    MemoEntry *oldentry, *newentry;
    size_t new_size = MT_MINSIZE;
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
    self->table = PyMem_NEW(MemoEntry, new_size);
    if (self->table == NULL) {
        self->table = oldtable;
        PyErr_NoMemory();
        return -1;
    }
    self->allocated = new_size;
    self->mask = new_size - 1;
    memset(self->table, 0, sizeof(MemoEntry) * new_size);

    /* Copy entries from the old table. */
    to_process = self->used;
    for (oldentry = oldtable; to_process > 0; oldentry++) {
        if (oldentry->key != NULL) {
            to_process--;
            /* newentry is a pointer to a chunk of the new
               table, so we're setting the key:value pair
               in-place. */
            newentry = _MemoTable_Lookup(self, oldentry->key);
            newentry->key = oldentry->key;
            newentry->value = oldentry->value;
        }
    }

    /* Deallocate the old table. */
    PyMem_Free(oldtable);
    return 0;
}

static int
MemoTable_Reset(MemoTable *self)
{
    MemoTable_Clear(self);
    if (self->allocated > self->buffered_size) {
        return _MemoTable_Resize(self, self->buffered_size);
    }
    return 0;
}

/* Returns -1 on failure, a value otherwise. */
static Py_ssize_t
MemoTable_Get(MemoTable *self, PyObject *key)
{
    MemoEntry *entry = _MemoTable_Lookup(self, key);
    if (entry->key == NULL)
        return -1;
    return entry->value;
}
#define MemoTable_GET_SAFE(self, obj) \
    (((self) == NULL) ? -1 : MemoTable_Get((self), (obj)))

/* Returns -1 on failure, 0 on success. */
static int
MemoTable_Set(MemoTable *self, PyObject *key, Py_ssize_t value)
{
    MemoEntry *entry;

    assert(key != NULL);

    entry = _MemoTable_Lookup(self, key);
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
    return _MemoTable_Resize(self, desired_size);
}

#undef MT_MINSIZE
#undef PERTURB_SHIFT

/*************************************************************************
 * Pickler object                                                        *
 *************************************************************************/
typedef struct PicklerObject {
    PyObject_HEAD
    /* Configuration */
    Py_ssize_t buffer_size;
    int proto;
    PyObject *buffer_callback;  /* Callback for out-of-band buffers, or NULL */

    /* Per-dumps state */
    PyObject *active_buffer_callback;
    MemoTable *memo;            /* Memo table, keep track of the seen
                                   objects to support self-referential objects
                                   pickling. */
    PyObject *output_buffer;    /* Write into a local bytearray buffer before
                                   flushing to the stream. */
    Py_ssize_t output_len;      /* Length of output_buffer. */
    Py_ssize_t max_output_len;  /* Allocation size of output_buffer. */
} PicklerObject;

static int save(PicklerObject *, PyObject *);

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
_Pickler_Write(PicklerObject *self, const char *s, Py_ssize_t data_len)
{
    Py_ssize_t i, n, required;
    char *buffer;

    assert(s != NULL);

    n = data_len;

    required = self->output_len + n;
    if (required > self->max_output_len) {
        /* Make place in buffer for the pickle chunk */
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
memo_get(PicklerObject *self, PyObject *key, Py_ssize_t memo_index)
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
        PickleState *st = _Pickle_GetGlobalState();
        PyErr_SetString(st->PicklingError,
                        "memo id too large for LONG_BINGET");
        return -1;
    }

    if (_Pickler_Write(self, pdata, len) < 0)
        return -1;

    return 0;
}

/* Store an object in the memo, assign it a new unique ID based on the number
   of objects currently stored in the memo and generate a PUT opcode. */
static int
memo_put(PicklerObject *self, PyObject *obj)
{
    Py_ssize_t idx;

    const char memoize_op = MEMOIZE;

    idx = MemoTable_Size(self->memo);
    if (MemoTable_Set(self->memo, obj, idx) < 0)
        return -1;

    if (_Pickler_Write(self, &memoize_op, 1) < 0)
        return -1;
    return 0;
}

#define MEMO_PUT_SAFE(self, obj) \
    (((self)->memo == NULL) ? 0 : memo_put((self), (obj)))

static int
save_none(PicklerObject *self, PyObject *obj)
{
    const char none_op = NONE;
    if (_Pickler_Write(self, &none_op, 1) < 0)
        return -1;

    return 0;
}

static int
save_bool(PicklerObject *self, PyObject *obj)
{
    const char bool_op = (obj == Py_True) ? NEWTRUE : NEWFALSE;
    if (_Pickler_Write(self, &bool_op, 1) < 0)
        return -1;
    return 0;
}

static int
save_long(PicklerObject *self, PyObject *obj)
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
        if (_Pickler_Write(self, pdata, len) < 0)
            return -1;

        return 0;
    }
    assert(!PyErr_Occurred());

    /* Linear-time pickling. */
    size_t nbits;
    size_t nbytes;
    unsigned char *pdata;
    char header[5];
    int i;
    int sign = _PyLong_Sign(obj);

    if (sign == 0) {
        header[0] = LONG1;
        header[1] = 0;      /* It's 0 -- an empty bytestring. */
        if (_Pickler_Write(self, header, 2) < 0)
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
                        "int too large to pickle");
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
    if (_Pickler_Write(self, header, size) < 0 ||
        _Pickler_Write(self, (char *)pdata, (int)nbytes) < 0)
        goto error;

    if (0) {
  error:
      status = -1;
    }
    Py_XDECREF(repr);

    return status;
}

static int
save_float(PicklerObject *self, PyObject *obj)
{
    double x = PyFloat_AS_DOUBLE((PyFloatObject *)obj);

    char pdata[9];
    pdata[0] = BINFLOAT;
    if (_PyFloat_Pack8(x, (unsigned char *)&pdata[1], 0) < 0)
        return -1;
    if (_Pickler_Write(self, pdata, 9) < 0)
        return -1;
    return 0;
}

static int
_write_bytes(PicklerObject *self,
             const char *header, Py_ssize_t header_size,
             const char *data, Py_ssize_t data_size,
             PyObject *payload)
{
    if (_Pickler_Write(self, header, header_size) < 0 ||
        _Pickler_Write(self, data, data_size) < 0) {
        return -1;
    }
    return 0;
}

static int
_save_bytes_data(PicklerObject *self, PyObject *obj, const char *data,
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

    if (MEMO_PUT_SAFE(self, obj) < 0) {
        return -1;
    }

    return 0;
}

static int
save_bytes(PicklerObject *self, PyObject *obj)
{
    return _save_bytes_data(self, obj, PyBytes_AS_STRING(obj),
                            PyBytes_GET_SIZE(obj));
}

static int
_save_bytearray_data(PicklerObject *self, PyObject *obj, const char *data,
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

    if (MEMO_PUT_SAFE(self, obj) < 0) {
        return -1;
    }

    return 0;
}

static int
save_bytearray(PicklerObject *self, PyObject *obj)
{
    return _save_bytearray_data(self, obj, PyByteArray_AS_STRING(obj),
                                PyByteArray_GET_SIZE(obj));
}

static int
save_picklebuffer(PicklerObject *self, PyObject *obj)
{
    const Py_buffer* view = PyPickleBuffer_GetBuffer(obj);
    if (view == NULL) {
        return -1;
    }
    if (view->suboffsets != NULL || !PyBuffer_IsContiguous(view, 'A')) {
        PickleState *st = _Pickle_GetGlobalState();
        PyErr_SetString(st->PicklingError,
                        "PickleBuffer can not be pickled when "
                        "pointing to a non-contiguous buffer");
        return -1;
    }
    int in_band = 1;
    if (self->active_buffer_callback != NULL) {
        PyObject *ret = PyObject_CallFunctionObjArgs(self->active_buffer_callback,
                                                     obj, NULL);
        if (ret == NULL) {
            return -1;
        }
        in_band = PyObject_IsTrue(ret);
        Py_DECREF(ret);
        if (in_band == -1) {
            return -1;
        }
    }
    if (in_band) {
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
    else {
        /* Write data out-of-band */
        const char next_buffer_op = NEXT_BUFFER;
        if (_Pickler_Write(self, &next_buffer_op, 1) < 0) {
            return -1;
        }
        if (view->readonly) {
            const char readonly_buffer_op = READONLY_BUFFER;
            if (_Pickler_Write(self, &readonly_buffer_op, 1) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int
save_unicode(PicklerObject *self, PyObject *obj)
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
    return 0;
}

/* A helper for save_tuple.  Push the len elements in tuple t on the stack. */
static int
store_tuple_elements(PicklerObject *self, PyObject *t, Py_ssize_t len)
{
    Py_ssize_t i;

    assert(PyTuple_Size(t) == len);

    for (i = 0; i < len; i++) {
        PyObject *element = PyTuple_GET_ITEM(t, i);

        if (element == NULL)
            return -1;
        if (save(self, element) < 0)
            return -1;
    }

    return 0;
}

/* Tuples are ubiquitous in the pickle protocols, so many techniques are
 * used across protocols to minimize the space needed to pickle them.
 * Tuples are also the only builtin immutable type that can be recursive
 * (a tuple can be reached from itself), and that requires some subtle
 * magic so that it works in all cases.  IOW, this is a long routine.
 */
static int
save_tuple(PicklerObject *self, PyObject *obj)
{
    Py_ssize_t len, i, memo_index;

    const char mark_op = MARK;
    const char tuple_op = TUPLE;
    const char pop_op = POP;
    const char pop_mark_op = POP_MARK;
    const char len2opcode[] = {EMPTY_TUPLE, TUPLE1, TUPLE2, TUPLE3};

    if ((len = PyTuple_Size(obj)) < 0)
        return -1;

    if (len == 0) {
        char pdata[2];

        if (self->proto) {
            pdata[0] = EMPTY_TUPLE;
            len = 1;
        }
        else {
            pdata[0] = MARK;
            pdata[1] = TUPLE;
            len = 2;
        }
        if (_Pickler_Write(self, pdata, len) < 0)
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
        if (store_tuple_elements(self, obj, len) < 0)
            return -1;

        memo_index = MemoTable_GET_SAFE(self->memo, obj);
        if (memo_index >= 0) {
            /* pop the len elements */
            for (i = 0; i < len; i++)
                if (_Pickler_Write(self, &pop_op, 1) < 0)
                    return -1;
            /* fetch from memo */
            if (memo_get(self, obj, memo_index) < 0)
                return -1;

            return 0;
        }
        else { /* Not recursive. */
            if (_Pickler_Write(self, len2opcode + len, 1) < 0)
                return -1;
        }
        goto memoize;
    }

    /* Generate MARK e1 e2 ... TUPLE */
    if (_Pickler_Write(self, &mark_op, 1) < 0)
        return -1;

    if (store_tuple_elements(self, obj, len) < 0)
        return -1;

    memo_index = MemoTable_GET_SAFE(self->memo, obj);
    if (memo_index >= 0) {
        /* pop the stack stuff we pushed */
        if (_Pickler_Write(self, &pop_mark_op, 1) < 0)
            return -1;
        /* fetch from memo */
        if (memo_get(self, obj, memo_index) < 0)
            return -1;

        return 0;
    }
    else { /* Not recursive. */
        if (_Pickler_Write(self, &tuple_op, 1) < 0)
            return -1;
    }

  memoize:
    if (MEMO_PUT_SAFE(self, obj) < 0)
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
batch_list(PicklerObject *self, PyObject *obj)
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
        if (save(self, item) < 0)
            return -1;
        if (_Pickler_Write(self, &append_op, 1) < 0)
            return -1;
        return 0;
    }

    /* Write in batches of BATCHSIZE. */
    total = 0;
    do {
        this_batch = 0;
        if (_Pickler_Write(self, &mark_op, 1) < 0)
            return -1;
        while (total < PyList_GET_SIZE(obj)) {
            item = PyList_GET_ITEM(obj, total);
            if (save(self, item) < 0)
                return -1;
            total++;
            if (++this_batch == BATCHSIZE)
                break;
        }
        if (_Pickler_Write(self, &appends_op, 1) < 0)
            return -1;

    } while (total < PyList_GET_SIZE(obj));

    return 0;
}

static int
save_list(PicklerObject *self, PyObject *obj)
{
    char header[3];
    Py_ssize_t len;
    int status = 0;

    /* Create an empty list. */
    header[0] = EMPTY_LIST;
    len = 1;

    if (_Pickler_Write(self, header, len) < 0)
        goto error;

    if (MEMO_PUT_SAFE(self, obj) < 0)
        goto error;

    if (PyList_GET_SIZE(obj)) {
        /* Materialize the list elements. */
        if (Py_EnterRecursiveCall(" while pickling an object"))
            goto error;
        status = batch_list(self, obj);
        Py_LeaveRecursiveCall();
    }
    if (0) {
  error:
        status = -1;
    }

    return status;
}

/* 
 * Batch up chunks of `MARK key value ... key value SETITEMS`
 * opcode sequences.  Calling code should have arranged to first create an
 * empty dict, or dict-like object, for the SETITEMS to operate on.
 * Returns 0 on success, -1 on error.
 */
static int
batch_dict(PicklerObject *self, PyObject *obj)
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
        if (save(self, key) < 0)
            return -1;
        if (save(self, value) < 0)
            return -1;
        if (_Pickler_Write(self, &setitem_op, 1) < 0)
            return -1;
        return 0;
    }

    /* Write in batches of BATCHSIZE. */
    do {
        i = 0;
        if (_Pickler_Write(self, &mark_op, 1) < 0)
            return -1;
        while (PyDict_Next(obj, &ppos, &key, &value)) {
            if (save(self, key) < 0)
                return -1;
            if (save(self, value) < 0)
                return -1;
            if (++i == BATCHSIZE)
                break;
        }
        if (_Pickler_Write(self, &setitems_op, 1) < 0)
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
save_dict(PicklerObject *self, PyObject *obj)
{
    char header[3];
    Py_ssize_t len;
    int status = 0;
    assert(PyDict_Check(obj));

    /* Create an empty dict. */
    header[0] = EMPTY_DICT;
    len = 1;

    if (_Pickler_Write(self, header, len) < 0)
        goto error;

    if (MEMO_PUT_SAFE(self, obj) < 0)
        goto error;

    if (PyDict_GET_SIZE(obj)) {
        /* Save the dict items. */
        if (Py_EnterRecursiveCall(" while pickling an object"))
            goto error;
        status = batch_dict(self, obj);
        Py_LeaveRecursiveCall();
    }

    if (0) {
  error:
        status = -1;
    }

    return status;
}

static int
save_set(PicklerObject *self, PyObject *obj)
{
    PyObject *item;
    int i;
    Py_ssize_t set_size, ppos = 0;
    Py_hash_t hash;

    const char empty_set_op = EMPTY_SET;
    const char mark_op = MARK;
    const char additems_op = ADDITEMS;

    if (_Pickler_Write(self, &empty_set_op, 1) < 0)
        return -1;

    if (MEMO_PUT_SAFE(self, obj) < 0)
        return -1;

    set_size = PySet_GET_SIZE(obj);
    if (set_size == 0)
        return 0;  /* nothing to do */

    /* Write in batches of BATCHSIZE. */
    do {
        i = 0;
        if (_Pickler_Write(self, &mark_op, 1) < 0)
            return -1;
        while (_PySet_NextEntry(obj, &ppos, &item, &hash)) {
            if (save(self, item) < 0)
                return -1;
            if (++i == BATCHSIZE)
                break;
        }
        if (_Pickler_Write(self, &additems_op, 1) < 0)
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
save_frozenset(PicklerObject *self, PyObject *obj)
{
    Py_ssize_t memo_index;
    PyObject *iter;

    const char mark_op = MARK;
    const char frozenset_op = FROZENSET;

    if (_Pickler_Write(self, &mark_op, 1) < 0)
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
        if (save(self, item) < 0) {
            Py_DECREF(item);
            Py_DECREF(iter);
            return -1;
        }
        Py_DECREF(item);
    }
    Py_DECREF(iter);

    /* If the object is already in the memo, this means it is
       recursive. In this case, throw away everything we put on the
       stack, and fetch the object back from the memo. */
    memo_index = MemoTable_GET_SAFE(self->memo, obj);
    if (memo_index >= 0) {
        const char pop_mark_op = POP_MARK;

        if (_Pickler_Write(self, &pop_mark_op, 1) < 0)
            return -1;
        if (memo_get(self, obj, memo_index) < 0)
            return -1;
        return 0;
    }

    if (_Pickler_Write(self, &frozenset_op, 1) < 0)
        return -1;
    if (MEMO_PUT_SAFE(self, obj) < 0)
        return -1;

    return 0;
}

static int
save(PicklerObject *self, PyObject *obj)
{
    PyTypeObject *type;
    PyObject *reduce_func = NULL;
    PyObject *reduce_value = NULL;
    Py_ssize_t memo_index;
    int status = 0;

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

    /* Check the memo to see if it has the object. If so, generate
       a GET (or BINGET) opcode, instead of pickling the object
       once again. */
    memo_index = MemoTable_GET_SAFE(self->memo, obj);
    if (memo_index >= 0) {
        return memo_get(self, obj, memo_index);
    }

    if (type == &PyBytes_Type) {
        return save_bytes(self, obj);
    }
    else if (type == &PyUnicode_Type) {
        return save_unicode(self, obj);
    }

    /* We're only calling Py_EnterRecursiveCall here so that atomic
       types above are pickled faster. */
    if (Py_EnterRecursiveCall(" while pickling an object")) {
        return -1;
    }

    if (type == &PyDict_Type) {
        status = save_dict(self, obj);
        goto done;
    }
    else if (type == &PySet_Type) {
        status = save_set(self, obj);
        goto done;
    }
    else if (type == &PyFrozenSet_Type) {
        status = save_frozenset(self, obj);
        goto done;
    }
    else if (type == &PyList_Type) {
        status = save_list(self, obj);
        goto done;
    }
    else if (type == &PyTuple_Type) {
        status = save_tuple(self, obj);
        goto done;
    }
    else if (type == &PyByteArray_Type) {
        status = save_bytearray(self, obj);
        goto done;
    }
    else if (type == &PyPickleBuffer_Type) {
        status = save_picklebuffer(self, obj);
        goto done;
    } else {
        PyErr_Format(PyExc_TypeError, "smolpickle doesn't support objects of type %.200s", type->tp_name);
        status = -1;
        goto done;
    }

  done:

    Py_LeaveRecursiveCall();
    Py_XDECREF(reduce_func);
    Py_XDECREF(reduce_value);

    return status;
}

static int
dump(PicklerObject *self, PyObject *obj)
{
    const char stop_op = STOP;
    char header[2];

    header[0] = PROTO;
    assert(self->proto >= 0 && self->proto < 256);
    header[1] = (unsigned char)self->proto;
    if (_Pickler_Write(self, header, 2) < 0 ||
        save(self, obj) < 0 ||
        _Pickler_Write(self, &stop_op, 1) < 0)
        return -1;
    return 0;
}

static PyObject*
Pickler_dumps_internal(PicklerObject *self, PyObject *obj, PyObject *buffer_callback)
{
    int status;
    PyObject *res = NULL;

    /* reset buffers */
    self->output_len = 0;
    if (self->output_buffer == NULL) {
        self->max_output_len = self->buffer_size;
        self->output_buffer = PyBytes_FromStringAndSize(NULL, self->max_output_len);
        if (self->output_buffer == NULL)
            return NULL;
    }

    if (buffer_callback != NULL) {
        self->active_buffer_callback = buffer_callback;
    } else {
        self->active_buffer_callback = self->buffer_callback;
    }

    status = dump(self, obj);

    /* Reset temporary state */
    self->active_buffer_callback = NULL;
    if (self->memo != NULL) {
        if (MemoTable_Reset(self->memo) < 0)
            status = -1;
    }

    if (status == 0) {
        if (self->max_output_len > self->buffer_size) {
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
    } else {
        /* Error in dumps, drop buffer if necessary */
        if (self->max_output_len > self->buffer_size) {
            Py_DECREF(self->output_buffer);
            self->output_buffer = NULL;
        }
    }
    return res;
}

PyDoc_STRVAR(Pickler_dumps__doc__,
"dumps(obj, *, buffer_callback=None)\n"
"--\n"
"\n"
"Return the pickled representation of the object as a bytes object.\n"
"\n"
"Only supports Python core types, other types will fail to serialize.\n"
"\n"
"If *buffer_callback* is None (the default), buffer views are serialized\n"
"into *file* as part of the pickle stream.");
static PyObject*
Pickler_dumps(PicklerObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"obj", "buffer_callback", NULL};
    PyObject *obj = NULL;
    PyObject *buffer_callback = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|$O", kwlist, &obj, &buffer_callback)) {
        return NULL;
    }
    return Pickler_dumps_internal(self, obj, buffer_callback);
}

static PyObject*
Pickler_sizeof(PicklerObject *self)
{
    Py_ssize_t res;

    res = sizeof(PicklerObject);
    if (self->memo != NULL) {
        res += sizeof(MemoTable);
        res += self->memo->allocated * sizeof(MemoEntry);
    }
    if (self->output_buffer != NULL) {
        res += self->max_output_len;
    }
    return PyLong_FromSsize_t(res);
}

static struct PyMethodDef Pickler_methods[] = {
    {
        "dumps", (PyCFunction) Pickler_dumps, METH_VARARGS | METH_KEYWORDS,
        Pickler_dumps__doc__,
    },
    {
        "__sizeof__", (PyCFunction) Pickler_sizeof, METH_NOARGS,
        PyDoc_STR("Size in bytes")
    },
    {NULL, NULL}                /* sentinel */
};

static int
Pickler_clear(PicklerObject *self)
{
    Py_CLEAR(self->output_buffer);
    Py_CLEAR(self->buffer_callback);

    if (self->memo != NULL) {
        MemoTable_Del(self->memo);
        self->memo = NULL;
    }
    return 0;
}

static void
Pickler_dealloc(PicklerObject *self)
{
    PyObject_GC_UnTrack(self);
    Pickler_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Pickler_traverse(PicklerObject *self, visitproc visit, void *arg)
{
    Py_ssize_t i;
    Py_VISIT(self->buffer_callback);

    if (self->memo != NULL) {
        i = self->memo->allocated;
        while (--i >= 0) {
            Py_VISIT(self->memo->table[i].key);
        }
    }
    return 0;
}

static int
Pickler_init_internal(
    PicklerObject *self, int proto, int memoize,
    Py_ssize_t buffer_size, PyObject *buffer_callback) {

    self->proto = proto;
    self->buffer_size = Py_MAX(buffer_size, 32);
    self->buffer_callback = buffer_callback;

    if (self->proto < 0) {
        self->proto = HIGHEST_PROTOCOL;
    }
    else if (self->proto > HIGHEST_PROTOCOL) {
        PyErr_Format(PyExc_ValueError, "pickle protocol must be <= %d", HIGHEST_PROTOCOL);
        return -1;
    }
    else if (self->proto < LOWEST_PROTOCOL) {
        PyErr_Format(PyExc_ValueError, "pickle protocol must be >= %d", LOWEST_PROTOCOL);
        return -1;
    }

    if (self->buffer_callback == Py_None) {
        self->buffer_callback = NULL;
    }
    Py_XINCREF(self->buffer_callback);

    if (memoize) {
        self->memo = MemoTable_New(64);
        if (self->memo == NULL)
            return -1;
    }

    self->output_len = 0;
    self->max_output_len = self->buffer_size;
    self->output_buffer = PyBytes_FromStringAndSize(NULL, self->max_output_len);
    if (self->output_buffer == NULL)
        return -1;
    return 0;
}

PyDoc_STRVAR(Pickler__doc__,
"Pickler(*, protocol=5, memoize=True, buffer_size=4096, buffer_callback=None)\n"
"--\n"
"\n"
"Efficiently handles pickling multiple objects\n"
"\n"
"Only supports Python core types, other types will fail to serialize.\n"
"\n"
"Creating an *Pickler* and calling the dumps() method multiple\n"
"times is more efficient than calling `smolpickle.dumps` multiple\n"
"times.\n"
"\n"
"The optional *protocol* argument tells the pickler to use the given\n"
"protocol. The default is 5, and is the only protocol currently \n"
"supported. Specifying a negative protocol selects the highest protocol\n"
"version supported.\n"
"\n"
"If *memoize* is False, pickle will avoid generating memoize instructions\n."
"This can be more efficient for some objects, but will fail to handle\n"
"self-referential objects.\n"
"\n"
"The optional *buffer_size* argument indicates the size of the internal\n"
"write buffer.\n"
"\n"
"If *buffer_callback* is None (the default), buffer views are serialized\n"
"into *file* as part of the pickle stream.");
static int
Pickler_init(PicklerObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"protocol", "memoize", "buffer_size", "buffer_callback", NULL};

    int proto = DEFAULT_PROTOCOL;
    int memoize = 1;
    Py_ssize_t buffer_size = 4096;
    PyObject *buffer_callback = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$ipnO", kwlist,
                                     &proto,
                                     &memoize,
                                     &buffer_size,
                                     &buffer_callback)) {
        return -1;
    }
    return Pickler_init_internal(self, proto, memoize, buffer_size, buffer_callback);
}

static PyTypeObject Pickler_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_pickle.Pickler",
    .tp_doc = Pickler__doc__,
    .tp_basicsize = sizeof(PicklerObject),
    .tp_dealloc = (destructor)Pickler_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)Pickler_traverse,
    .tp_clear = (inquiry)Pickler_clear,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Pickler_init,
    .tp_methods = Pickler_methods,
};

/*************************************************************************
 * Unpickler object                                                      *
 *************************************************************************/
typedef struct UnpicklerObject {
    PyObject_HEAD
    /* Static configuration */
    Py_ssize_t reset_stack_size;
    size_t reset_memo_size;
    Py_ssize_t reset_marks_size;

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
    Py_ssize_t *marks;          /* Mark stack, used for unpickling container
                                   objects. */
    Py_ssize_t marks_allocated; /* Current allocated size of the mark stack. */
    Py_ssize_t marks_len;       /* Number of marks in the mark stack. */
} UnpicklerObject;

static int
Unpickler_init_internal(UnpicklerObject *self)
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

    return 0;
}

PyDoc_STRVAR(Unpickler__doc__,
"Unpickler()\n"
"--\n"
"\n"
"Efficiently handles unpickling multiple objects\n"
"\n"
"Only supports Python core types, other types will fail to deserialize.\n"
"\n"
"Creating an *Unpickler* and calling the loads() method multiple\n"
"times is more efficient than calling `smolpickle.loads` multiple\n"
"times.");
static int
Unpickler_init(UnpicklerObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "", kwlist)) {
        return -1;
    }
    return Unpickler_init_internal(self);
}

static void _Unpickler_memo_clear(UnpicklerObject *self);
static void _Unpickler_stack_clear(UnpicklerObject *self, Py_ssize_t clearto);

static int
Unpickler_clear(UnpicklerObject *self)
{
    _Unpickler_stack_clear(self, 0);
    PyMem_Free(self->stack);
    self->stack = NULL;

    _Unpickler_memo_clear(self);
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
Unpickler_dealloc(UnpicklerObject *self)
{
    PyObject_GC_UnTrack((PyObject *)self);

    Unpickler_clear(self);

    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Unpickler_traverse(UnpicklerObject *self, visitproc visit, void *arg)
{
    Py_ssize_t i = self->stack_len;
    while (--i >= 0) {
        Py_VISIT(self->stack[i]);
    }
    Py_VISIT(self->buffers);
    return 0;
}

static Py_ssize_t
bad_readline()
{
    PickleState *st = _Pickle_GetGlobalState();
    PyErr_SetString(st->UnpicklingError, "pickle data was truncated");
    return -1;
}

static Py_ssize_t
_Unpickler_Read(UnpicklerObject *self, char **s, Py_ssize_t n)
{
    if (n <= self->input_len - self->next_read_idx) {
        *s = self->input_buffer + self->next_read_idx;
        self->next_read_idx += n;
        return n;
    }
    return bad_readline();
}

static Py_ssize_t
_Unpickler_ReadInto(UnpicklerObject *self, char *buf, Py_ssize_t n)
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
_Unpickler_stack_clear(UnpicklerObject *self, Py_ssize_t clearto)
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
_Unpickler_stack_grow(UnpicklerObject *self)
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
_Unpickler_stack_underflow(UnpicklerObject *self)
{
    PickleState *st = _Pickle_GetGlobalState();
    PyErr_SetString(st->UnpicklingError,
                    self->marks_len ?
                    "unexpected MARK found" :
                    "unpickling stack underflow");
    return -1;
}

static PyObject *
_Unpickler_stack_pop(UnpicklerObject *self)
{
    if (self->stack_len <= self->fence) {
        _Unpickler_stack_underflow(self);
        return NULL;
    }
    return self->stack[--self->stack_len];
}
/* Pop the top element and store it into V. On stack underflow,
 * UnpicklingError is raised and V is set to NULL.
 */
#define STACK_POP(U, V) do { (V) = _Unpickler_stack_pop((U)); } while (0)

static int
_Unpickler_stack_push(UnpicklerObject *self, PyObject *obj)
{
    if (self->stack_len == self->stack_allocated && _Unpickler_stack_grow(self) < 0) {
        return -1;
    }
    self->stack[self->stack_len++] = obj;
    return 0;
}

/* Push an object on stack, transferring its ownership to the stack. */
#define STACK_PUSH(U, O) do {                               \
        if (_Unpickler_stack_push((U), (O)) < 0) return -1; } while(0)

/* Push an object on stack, adding a new reference to the object. */
#define STACK_INCREF_PUSH(U, O) do {                             \
        Py_INCREF((O));                                         \
        if (_Unpickler_stack_push((U), (O)) < 0) return -1; } while(0)

static PyObject *
_Unpickler_stack_poptuple(UnpicklerObject *self, Py_ssize_t start)
{
    PyObject *tuple;
    Py_ssize_t len, i, j;

    if (start < self->fence) {
        _Unpickler_stack_underflow(self);
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
_Unpickler_stack_poplist(UnpicklerObject *self, Py_ssize_t start)
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
_Unpickler_memo_resize(UnpicklerObject *self, size_t new_size)
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
#define _Unpickler_memo_get(self, idx) \
    (((size_t)(idx) >= (self)->memo_len) ? NULL : (self)->memo[(size_t)(idx)]);

/* Returns -1 (with an exception set) on failure, 0 on success.
   This takes its own reference to `value`. */
static int
_Unpickler_memo_put(UnpicklerObject *self, size_t idx, PyObject *value)
{
    PyObject *old_item;

    if (idx >= self->memo_allocated) {
        if (_Unpickler_memo_resize(self, idx * 2) < 0)
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
_Unpickler_memo_clear(UnpicklerObject *self)
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
marker(UnpicklerObject *self)
{
    Py_ssize_t mark;

    if (self->marks_len < 1) {
        PickleState *st = _Pickle_GetGlobalState();
        PyErr_SetString(st->UnpicklingError, "could not find MARK");
        return -1;
    }

    mark = self->marks[--self->marks_len];
    self->fence = self->marks_len ?
            self->marks[self->marks_len - 1] : 0;
    return mark;
}

static int
load_none(UnpicklerObject *self)
{
    STACK_INCREF_PUSH(self, Py_None);
    return 0;
}

static int
load_bool(UnpicklerObject *self, PyObject *boolean)
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
load_binintx(UnpicklerObject *self, char *s, int size)
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
load_binint(UnpicklerObject *self)
{
    char *s;

    if (_Unpickler_Read(self, &s, 4) < 0)
        return -1;

    return load_binintx(self, s, 4);
}

static int
load_binint1(UnpicklerObject *self)
{
    char *s;

    if (_Unpickler_Read(self, &s, 1) < 0)
        return -1;

    return load_binintx(self, s, 1);
}

static int
load_binint2(UnpicklerObject *self)
{
    char *s;

    if (_Unpickler_Read(self, &s, 2) < 0)
        return -1;

    return load_binintx(self, s, 2);
}

/* 'size' bytes contain the # of bytes of little-endian 256's-complement
 * data following.
 */
static int
load_counted_long(UnpicklerObject *self, int size)
{
    PyObject *value;
    char *nbytes;
    char *pdata;

    assert(size == 1 || size == 4);
    if (_Unpickler_Read(self, &nbytes, size) < 0)
        return -1;

    size = calc_binint(nbytes, size);
    if (size < 0) {
        PickleState *st = _Pickle_GetGlobalState();
        /* Corrupt or hostile pickle -- we never write one like this */
        PyErr_SetString(st->UnpicklingError,
                        "LONG pickle has negative byte count");
        return -1;
    }

    if (size == 0)
        value = PyLong_FromLong(0L);
    else {
        /* Read the raw little-endian bytes and convert. */
        if (_Unpickler_Read(self, &pdata, size) < 0)
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
load_binfloat(UnpicklerObject *self)
{
    PyObject *value;
    double x;
    char *s;

    if (_Unpickler_Read(self, &s, 8) < 0)
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
load_counted_binbytes(UnpicklerObject *self, int nbytes)
{
    PyObject *bytes;
    Py_ssize_t size;
    char *s;

    if (_Unpickler_Read(self, &s, nbytes) < 0)
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
    if (_Unpickler_ReadInto(self, PyBytes_AS_STRING(bytes), size) < 0) {
        Py_DECREF(bytes);
        return -1;
    }

    STACK_PUSH(self, bytes);
    return 0;
}

static int
load_counted_bytearray(UnpicklerObject *self)
{
    PyObject *bytearray;
    Py_ssize_t size;
    char *s;

    if (_Unpickler_Read(self, &s, 8) < 0) {
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
    if (_Unpickler_ReadInto(self, PyByteArray_AS_STRING(bytearray), size) < 0) {
        Py_DECREF(bytearray);
        return -1;
    }

    STACK_PUSH(self, bytearray);
    return 0;
}

static int
load_next_buffer(UnpicklerObject *self)
{
    if (self->buffers == NULL) {
        PickleState *st = _Pickle_GetGlobalState();
        PyErr_SetString(st->UnpicklingError,
                        "pickle stream refers to out-of-band data "
                        "but no *buffers* argument was given");
        return -1;
    }
    PyObject *buf = PyIter_Next(self->buffers);
    if (buf == NULL) {
        if (!PyErr_Occurred()) {
            PickleState *st = _Pickle_GetGlobalState();
            PyErr_SetString(st->UnpicklingError,
                            "not enough out-of-band buffers");
        }
        return -1;
    }

    STACK_PUSH(self, buf);
    return 0;
}

static int
load_readonly_buffer(UnpicklerObject *self)
{
    Py_ssize_t len = self->stack_len;
    if (len <= self->fence) {
        return _Unpickler_stack_underflow(self);
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
load_counted_binunicode(UnpicklerObject *self, int nbytes)
{
    PyObject *str;
    Py_ssize_t size;
    char *s;

    if (_Unpickler_Read(self, &s, nbytes) < 0)
        return -1;

    size = calc_binsize(s, nbytes);
    if (size < 0) {
        PyErr_Format(PyExc_OverflowError,
                     "BINUNICODE exceeds system's maximum size of %zd bytes",
                     PY_SSIZE_T_MAX);
        return -1;
    }

    if (_Unpickler_Read(self, &s, size) < 0)
        return -1;

    str = PyUnicode_DecodeUTF8(s, size, "surrogatepass");
    if (str == NULL)
        return -1;

    STACK_PUSH(self, str);
    return 0;
}

static int
load_counted_tuple(UnpicklerObject *self, Py_ssize_t len)
{
    PyObject *tuple;

    if (self->stack_len < len)
        return _Unpickler_stack_underflow(self);

    tuple = _Unpickler_stack_poptuple(self, self->stack_len - len);
    if (tuple == NULL)
        return -1;
    STACK_PUSH(self, tuple);
    return 0;
}

static int
load_tuple(UnpicklerObject *self)
{
    Py_ssize_t i;

    if ((i = marker(self)) < 0)
        return -1;

    return load_counted_tuple(self, self->stack_len - i);
}

static int
load_empty_list(UnpicklerObject *self)
{
    PyObject *list;

    if ((list = PyList_New(0)) == NULL)
        return -1;
    STACK_PUSH(self, list);
    return 0;
}

static int
load_empty_dict(UnpicklerObject *self)
{
    PyObject *dict;

    if ((dict = PyDict_New()) == NULL)
        return -1;
    STACK_PUSH(self, dict);
    return 0;
}

static int
load_empty_set(UnpicklerObject *self)
{
    PyObject *set;

    if ((set = PySet_New(NULL)) == NULL)
        return -1;
    STACK_PUSH(self, set);
    return 0;
}

static int
load_frozenset(UnpicklerObject *self)
{
    PyObject *items;
    PyObject *frozenset;
    Py_ssize_t i;

    if ((i = marker(self)) < 0)
        return -1;

    items = _Unpickler_stack_poptuple(self, i);
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
load_pop(UnpicklerObject *self)
{
    Py_ssize_t len = self->stack_len;

    /* Note that we split the (pickle.py) stack into two stacks,
     * an object stack and a mark stack. We have to be clever and
     * pop the right one. We do this by looking at the top of the
     * mark stack first, and only signalling a stack underflow if
     * the object stack is empty and the mark stack doesn't match
     * our expectations.
     */
    if (self->marks_len > 0 && self->marks[self->marks_len - 1] == len) {
        self->marks_len--;
        self->fence = self->marks_len ? self->marks[self->marks_len - 1] : 0;
    } else if (len <= self->fence)
        return _Unpickler_stack_underflow(self);
    else {
        len--;
        Py_DECREF(self->stack[len]);
        self->stack_len = len;
    }
    return 0;
}

static int
load_pop_mark(UnpicklerObject *self)
{
    Py_ssize_t i;

    if ((i = marker(self)) < 0)
        return -1;

    _Unpickler_stack_clear(self, i);

    return 0;
}

static int
load_binget(UnpicklerObject *self)
{
    PyObject *value;
    Py_ssize_t idx;
    char *s;

    if (_Unpickler_Read(self, &s, 1) < 0)
        return -1;

    idx = Py_CHARMASK(s[0]);

    value = _Unpickler_memo_get(self, idx);
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
load_long_binget(UnpicklerObject *self)
{
    PyObject *value;
    Py_ssize_t idx;
    char *s;

    if (_Unpickler_Read(self, &s, 4) < 0)
        return -1;

    idx = calc_binsize(s, 4);

    value = _Unpickler_memo_get(self, idx);
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
load_binput(UnpicklerObject *self)
{
    PyObject *value;
    Py_ssize_t idx;
    char *s;

    if (_Unpickler_Read(self, &s, 1) < 0)
        return -1;

    if (self->stack_len <= self->fence)
        return _Unpickler_stack_underflow(self);
    value = self->stack[self->stack_len - 1];

    idx = Py_CHARMASK(s[0]);

    return _Unpickler_memo_put(self, idx, value);
}

static int
load_long_binput(UnpicklerObject *self)
{
    PyObject *value;
    Py_ssize_t idx;
    char *s;

    if (_Unpickler_Read(self, &s, 4) < 0)
        return -1;

    if (self->stack_len <= self->fence)
        return _Unpickler_stack_underflow(self);
    value = self->stack[self->stack_len - 1];

    idx = calc_binsize(s, 4);
    if (idx < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "negative LONG_BINPUT argument");
        return -1;
    }

    return _Unpickler_memo_put(self, idx, value);
}

static int
load_memoize(UnpicklerObject *self)
{
    PyObject *value;

    if (self->stack_len <= self->fence)
        return _Unpickler_stack_underflow(self);
    value = self->stack[self->stack_len - 1];

    return _Unpickler_memo_put(self, self->memo_len, value);
}

#define raise_unpickling_error(fmt, ...) \
    PyErr_Format(_Pickle_GetGlobalState()->UnpicklingError, (fmt), __VA_ARGS__)

static int
do_append(UnpicklerObject *self, Py_ssize_t x)
{
    PyObject *slice;
    PyObject *list;
    Py_ssize_t len;

    len = self->stack_len;
    if (x > len || x <= self->fence)
        return _Unpickler_stack_underflow(self);
    if (len == x)  /* nothing to do */
        return 0;

    list = self->stack[x - 1];

    if (PyList_CheckExact(list)) {
        Py_ssize_t list_len;
        int ret;

        slice = _Unpickler_stack_poplist(self, x);
        if (!slice)
            return -1;
        list_len = PyList_GET_SIZE(list);
        ret = PyList_SetSlice(list, list_len, list_len, slice);
        Py_DECREF(slice);
        return ret;
    }
    raise_unpickling_error("Invalid APPEND(S) opcode on object of type %.200s", Py_TYPE(list)->tp_name);
    return -1;
}

static int
load_append(UnpicklerObject *self)
{
    if (self->stack_len - 1 <= self->fence)
        return _Unpickler_stack_underflow(self);
    return do_append(self, self->stack_len - 1);
}

static int
load_appends(UnpicklerObject *self)
{
    Py_ssize_t i = marker(self);
    if (i < 0)
        return -1;
    return do_append(self, i);
}

static int
do_setitems(UnpicklerObject *self, Py_ssize_t x)
{
    PyObject *value, *key;
    PyObject *dict;
    Py_ssize_t len, i;
    int status = 0;

    len = self->stack_len;
    if (x > len || x <= self->fence)
        return _Unpickler_stack_underflow(self);
    if (len == x)  /* nothing to do */
        return 0;
    if ((len - x) % 2 != 0) {
        PickleState *st = _Pickle_GetGlobalState();
        /* Currupt or hostile pickle -- we never write one like this. */
        PyErr_SetString(st->UnpicklingError,
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
        _Unpickler_stack_clear(self, x);
        return status;
    }
    raise_unpickling_error("Invalid SETITEM(S) opcode on object of type %.200s", Py_TYPE(dict)->tp_name);
    return -1;
}

static int
load_setitem(UnpicklerObject *self)
{
    return do_setitems(self, self->stack_len - 2);
}

static int
load_setitems(UnpicklerObject *self)
{
    Py_ssize_t i = marker(self);
    if (i < 0)
        return -1;
    return do_setitems(self, i);
}

static int
load_additems(UnpicklerObject *self)
{
    PyObject *set;
    Py_ssize_t mark, len;

    mark =  marker(self);
    if (mark < 0)
        return -1;
    len = self->stack_len;
    if (mark > len || mark <= self->fence)
        return _Unpickler_stack_underflow(self);
    if (len == mark)  /* nothing to do */
        return 0;

    set = self->stack[mark - 1];

    if (PySet_Check(set)) {
        PyObject *items;
        int status;

        items = _Unpickler_stack_poptuple(self, mark);
        if (items == NULL)
            return -1;

        status = _PySet_Update(set, items);
        Py_DECREF(items);
        return status;
    }
    raise_unpickling_error("Invalid ADDITEMS opcode on object of type %.200s", Py_TYPE(set)->tp_name);
    return 0;
}

static int
load_mark(UnpicklerObject *self)
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

/* Just raises an error if we don't know the protocol specified.  PROTO
 * is the first opcode for protocols >= 2.
 */
static int
load_proto(UnpicklerObject *self)
{
    char *s;
    int i;

    if (_Unpickler_Read(self, &s, 1) < 0)
        return -1;

    i = (unsigned char)s[0];
    if (i <= HIGHEST_PROTOCOL) {
        return 0;
    }

    PyErr_Format(PyExc_ValueError, "unsupported pickle protocol: %d", i);
    return -1;
}

static int
load_frame(UnpicklerObject *self)
{
    char *s;
    Py_ssize_t frame_len;

    if (_Unpickler_Read(self, &s, 8) < 0)
        return -1;

    frame_len = calc_binsize(s, 8);
    if (frame_len < 0) {
        PyErr_Format(PyExc_OverflowError,
                     "FRAME length exceeds system's maximum of %zd bytes",
                     PY_SSIZE_T_MAX);
        return -1;
    }

    if (_Unpickler_Read(self, &s, frame_len) < 0)
        return -1;

    /* Rewind to start of frame */
    self->next_read_idx -= frame_len;
    return 0;
}

static PyObject *
load(UnpicklerObject *self)
{
    PyObject *value = NULL;
    char *s = NULL;

    /* Convenient macros for the dispatch while-switch loop just below. */
#define OP(opcode, load_func) \
    case opcode: if (load_func(self) < 0) break; continue;

#define OP_ARG(opcode, load_func, arg) \
    case opcode: if (load_func(self, (arg)) < 0) break; continue;

    while (1) {
        if (_Unpickler_Read(self, &s, 1) < 0) {
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
        OP(BINPUT, load_binput)
        OP(LONG_BINPUT, load_long_binput)
        OP(MEMOIZE, load_memoize)
        OP(POP, load_pop)
        OP(POP_MARK, load_pop_mark)
        OP(SETITEM, load_setitem)
        OP(SETITEMS, load_setitems)
        OP(PROTO, load_proto)
        OP(FRAME, load_frame)
        OP_ARG(NEWTRUE, load_bool, Py_True)
        OP_ARG(NEWFALSE, load_bool, Py_False)

        case STOP:
            break;

        default:
            {
                PickleState *st = _Pickle_GetGlobalState();
                unsigned char c = (unsigned char) *s;
                if (0x20 <= c && c <= 0x7e && c != '\'' && c != '\\') {
                    PyErr_Format(st->UnpicklingError,
                                 "invalid load key, '%c'.", c);
                }
                else {
                    PyErr_Format(st->UnpicklingError,
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
Unpickler_sizeof(UnpicklerObject *self)
{
    Py_ssize_t res;

    res = sizeof(UnpicklerObject);
    if (self->stack != NULL)
        res += self->stack_allocated * sizeof(PyObject *);
    if (self->memo != NULL)
        res += self->memo_allocated * sizeof(PyObject *);
    if (self->marks != NULL)
        res += self->marks_allocated * sizeof(Py_ssize_t);
    return PyLong_FromSsize_t(res);
}

static PyObject*
Unpickler_loads_internal(UnpicklerObject *self, PyObject *data, PyObject *buffers) {
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
        self->input_buffer = NULL;
    }
    Py_CLEAR(self->buffers);
    /* Reset stack, deallocates if allocation exceeded limit */
    _Unpickler_stack_clear(self, 0);
    if (self->stack_allocated > self->reset_stack_size) {
        PyMem_Free(self->stack);
        self->stack = NULL;
    }
    /* Reset memo, deallocates if allocation exceeded limit */
    _Unpickler_memo_clear(self);
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

PyDoc_STRVAR(Unpickler_loads__doc__,
"loads(self, data, *, buffers=())\n"
"--\n"
"\n"
"Deserialize an object from the given pickle data.\n"
"\n"
"Only supports Python core types, other types will fail to deserialize.\n"
"\n"
"The optional *buffers* argument takes an iterable of out-of-band buffers\n"
"generated by passing a *buffer_callback* to the corresponding *dumps* call.");
static PyObject*
Unpickler_loads(UnpicklerObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *data = NULL;
    PyObject *buffers = NULL;
    static char *kwlist[] = {"data", "buffers", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|$O", kwlist,
                                     &data, &buffers)) {
        return NULL;
    }

    return Unpickler_loads_internal(self, data, buffers);
}

static struct PyMethodDef Unpickler_methods[] = {
    {
        "loads", (PyCFunction) Unpickler_loads, METH_VARARGS | METH_KEYWORDS,
        Unpickler_loads__doc__,
    },
    {
        "__sizeof__", (PyCFunction) Unpickler_sizeof, METH_NOARGS,
        PyDoc_STR("Size in bytes")
    },
    {NULL, NULL}                /* sentinel */
};

static PyTypeObject Unpickler_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_pickle.Unpickler",
    .tp_doc = Unpickler__doc__,
    .tp_basicsize = sizeof(UnpicklerObject),
    .tp_dealloc = (destructor)Unpickler_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)Unpickler_traverse,
    .tp_clear = (inquiry)Unpickler_clear,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) Unpickler_init,
    .tp_methods = Unpickler_methods,
};


/*************************************************************************
 * Module-level definitions                                              *
 *************************************************************************/

PyDoc_STRVAR(pickle_dumps__doc__,
"dumps(obj, *, protocol=5, memoize=True, buffer_callback=None)\n"
"--\n"
"\n"
"Return the pickled representation of the object as a bytes object.\n"
"\n"
"Only supports Python core types, other types will fail to serialize.\n"
"\n"
"The optional *protocol* argument tells the pickler to use the given\n"
"protocol. The default is 5, and is the only protocol currently \n"
"supported. Specifying a negative protocol selects the highest protocol\n"
"version supported.\n"
"\n"
"If *memoize* is False, pickle will avoid generating memoize instructions\n."
"This can be more efficient for some objects, but will fail to handle\n"
"self-referential objects.\n"
"\n"
"If *buffer_callback* is None (the default), buffer views are serialized\n"
"into *file* as part of the pickle stream.");
static PyObject*
pickle_dumps(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"obj", "protocol", "memoize", "buffer_callback", NULL};

    PyObject *obj = NULL;
    int proto = DEFAULT_PROTOCOL;
    int memoize = 1;
    PyObject *buffer_callback = NULL;
    PicklerObject *pickler;
    PyObject *res = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|$ipO", kwlist,
                                     &obj,
                                     &proto,
                                     &memoize,
                                     &buffer_callback)) {
        return NULL;
    }

    pickler = PyObject_GC_New(PicklerObject, &Pickler_Type);
    if (pickler == NULL) {
        return NULL;
    }
    if (Pickler_init_internal(pickler, proto, memoize, 32, buffer_callback) == 0) {
        res = Pickler_dumps_internal(pickler, obj, NULL);
    }

    Py_DECREF(pickler);
    return res;
}

PyDoc_STRVAR(pickle_loads__doc__,
"loads(data, *, buffers=())\n"
"--\n"
"\n"
"Deserialize an object from the given pickle data.\n"
"\n"
"Only supports Python core types, other types will fail to deserialize.\n"
"\n"
"The optional *buffers* argument takes an iterable of out-of-band buffers\n"
"generated by passing a *buffer_callback* to the corresponding *dumps* call.");
static PyObject*
pickle_loads(PyObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"data", "buffers", NULL};

    PyObject *data = NULL;
    PyObject *buffers = NULL;
    PyObject *res = NULL;
    UnpicklerObject *unpickler;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|$O", kwlist,
                                     &data, &buffers)) {
        return NULL;
    }

    unpickler = PyObject_GC_New(UnpicklerObject, &Unpickler_Type);
    if (unpickler == NULL) {
        return NULL;
    }
    if (Unpickler_init_internal(unpickler) == 0) {
        res = Unpickler_loads_internal(unpickler, data, buffers);
    }

    Py_DECREF(unpickler);
    return res;
}

static struct PyMethodDef pickle_methods[] = {
    {
        "dumps", (PyCFunction) pickle_dumps, METH_VARARGS | METH_KEYWORDS,
        pickle_dumps__doc__,
    },
    {
        "loads", (PyCFunction) pickle_loads, METH_VARARGS | METH_KEYWORDS,
        pickle_loads__doc__,
    },
    {NULL, NULL} /* sentinel */
};

static int
pickle_clear(PyObject *m)
{
    _Pickle_ClearState(_Pickle_GetState(m));
    return 0;
}

static void
pickle_free(PyObject *m)
{
    _Pickle_ClearState(_Pickle_GetState(m));
}

static int
pickle_traverse(PyObject *m, visitproc visit, void *arg)
{
    PickleState *st = _Pickle_GetState(m);
    Py_VISIT(st->PickleError);
    Py_VISIT(st->PicklingError);
    Py_VISIT(st->UnpicklingError);
    return 0;
}

static struct PyModuleDef _picklemodule = {
    PyModuleDef_HEAD_INIT,
    "_pickle",            /* m_name */
    pickle__doc__,        /* m_doc */
    sizeof(PickleState),  /* m_size */
    pickle_methods,       /* m_methods */
    NULL,                 /* m_reload */
    pickle_traverse,      /* m_traverse */
    pickle_clear,         /* m_clear */
    (freefunc)pickle_free /* m_free */
};

PyMODINIT_FUNC
PyInit__pickle(void)
{
    PyObject *m;
    PickleState *st;

    m = PyState_FindModule(&_picklemodule);
    if (m) {
        Py_INCREF(m);
        return m;
    }

    if (PyType_Ready(&Unpickler_Type) < 0)
        return NULL;
    if (PyType_Ready(&Pickler_Type) < 0)
        return NULL;

    /* Create the module and add the functions. */
    m = PyModule_Create(&_picklemodule);
    if (m == NULL)
        return NULL;

    /* Add types */
    Py_INCREF(&Pickler_Type);
    if (PyModule_AddObject(m, "Pickler", (PyObject *)&Pickler_Type) < 0)
        return NULL;
    Py_INCREF(&Unpickler_Type);
    if (PyModule_AddObject(m, "Unpickler", (PyObject *)&Unpickler_Type) < 0)
        return NULL;
    Py_INCREF(&PyPickleBuffer_Type);
    if (PyModule_AddObject(m, "PickleBuffer",
                           (PyObject *)&PyPickleBuffer_Type) < 0)
        return NULL;

    st = _Pickle_GetState(m);

    /* Initialize the exceptions. */
    st->PickleError = PyErr_NewException("_pickle.PickleError", NULL, NULL);
    if (st->PickleError == NULL)
        return NULL;
    st->PicklingError = \
        PyErr_NewException("_pickle.PicklingError", st->PickleError, NULL);
    if (st->PicklingError == NULL)
        return NULL;
    st->UnpicklingError = \
        PyErr_NewException("_pickle.UnpicklingError", st->PickleError, NULL);
    if (st->UnpicklingError == NULL)
        return NULL;

    Py_INCREF(st->PickleError);
    if (PyModule_AddObject(m, "PickleError", st->PickleError) < 0)
        return NULL;
    Py_INCREF(st->PicklingError);
    if (PyModule_AddObject(m, "PicklingError", st->PicklingError) < 0)
        return NULL;
    Py_INCREF(st->UnpicklingError);
    if (PyModule_AddObject(m, "UnpicklingError", st->UnpicklingError) < 0)
        return NULL;

    return m;
}
