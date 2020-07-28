#include "Python.h"
#include "structmember.h"

PyDoc_STRVAR(pickle_module_doc,
"Optimized C implementation for the Python pickle module.");

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
   /* Keep in synch with pickle.Pickler._BATCHSIZE.  This is how many elements
      batch_list/dict() pumps out before doing APPENDS/SETITEMS. */
    BATCHSIZE = 1000,

    /* Initial size of the write buffer of Pickler. */
    WRITE_BUF_SIZE = 4096,

    /* Prefetch size when unpickling (disabled on unpeekable streams) */
    PREFETCH = 8192 * 16,

    FRAME_SIZE_TARGET = 64 * 1024,
};

/*************************************************************************/

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
_Pickle_GetGlobalState(void)
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

/* Helper for calling a function with a single argument quickly.

   This function steals the reference of the given argument. */
static PyObject *
_Pickle_FastCall(PyObject *func, PyObject *obj)
{
    PyObject *result;

    result = PyObject_CallFunctionObjArgs(func, obj, NULL);
    Py_DECREF(obj);
    return result;
}

/*************************************************************************/

/* Internal data type used as the unpickling stack. */
typedef struct {
    PyObject_VAR_HEAD
    PyObject **data;
    int mark_set;          /* is MARK set? */
    Py_ssize_t fence;      /* position of top MARK or 0 */
    Py_ssize_t allocated;  /* number of slots in data allocated */
} Pdata;

static void
Pdata_dealloc(Pdata *self)
{
    Py_ssize_t i = Py_SIZE(self);
    while (--i >= 0) {
        Py_DECREF(self->data[i]);
    }
    PyMem_FREE(self->data);
    PyObject_Del(self);
}

static PyTypeObject Pdata_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_pickle.Pdata",              /*tp_name*/
    sizeof(Pdata),                /*tp_basicsize*/
    sizeof(PyObject *),           /*tp_itemsize*/
    (destructor)Pdata_dealloc,    /*tp_dealloc*/
};

static PyObject *
Pdata_New(void)
{
    Pdata *self;

    if (!(self = PyObject_New(Pdata, &Pdata_Type)))
        return NULL;
    Py_SIZE(self) = 0;
    self->mark_set = 0;
    self->fence = 0;
    self->allocated = 8;
    self->data = PyMem_MALLOC(self->allocated * sizeof(PyObject *));
    if (self->data)
        return (PyObject *)self;
    Py_DECREF(self);
    return PyErr_NoMemory();
}


/* Retain only the initial clearto items.  If clearto >= the current
 * number of items, this is a (non-erroneous) NOP.
 */
static int
Pdata_clear(Pdata *self, Py_ssize_t clearto)
{
    Py_ssize_t i = Py_SIZE(self);

    assert(clearto >= self->fence);
    if (clearto >= i)
        return 0;

    while (--i >= clearto) {
        Py_CLEAR(self->data[i]);
    }
    Py_SIZE(self) = clearto;
    return 0;
}

static int
Pdata_grow(Pdata *self)
{
    PyObject **data = self->data;
    size_t allocated = (size_t)self->allocated;
    size_t new_allocated;

    new_allocated = (allocated >> 3) + 6;
    /* check for integer overflow */
    if (new_allocated > (size_t)PY_SSIZE_T_MAX - allocated)
        goto nomemory;
    new_allocated += allocated;
    PyMem_RESIZE(data, PyObject *, new_allocated);
    if (data == NULL)
        goto nomemory;

    self->data = data;
    self->allocated = (Py_ssize_t)new_allocated;
    return 0;

  nomemory:
    PyErr_NoMemory();
    return -1;
}

static int
Pdata_stack_underflow(Pdata *self)
{
    PickleState *st = _Pickle_GetGlobalState();
    PyErr_SetString(st->UnpicklingError,
                    self->mark_set ?
                    "unexpected MARK found" :
                    "unpickling stack underflow");
    return -1;
}

/* D is a Pdata*.  Pop the topmost element and store it into V, which
 * must be an lvalue holding PyObject*.  On stack underflow, UnpicklingError
 * is raised and V is set to NULL.
 */
static PyObject *
Pdata_pop(Pdata *self)
{
    if (Py_SIZE(self) <= self->fence) {
        Pdata_stack_underflow(self);
        return NULL;
    }
    return self->data[--Py_SIZE(self)];
}
#define PDATA_POP(D, V) do { (V) = Pdata_pop((D)); } while (0)

static int
Pdata_push(Pdata *self, PyObject *obj)
{
    if (Py_SIZE(self) == self->allocated && Pdata_grow(self) < 0) {
        return -1;
    }
    self->data[Py_SIZE(self)++] = obj;
    return 0;
}

/* Push an object on stack, transferring its ownership to the stack. */
#define PDATA_PUSH(D, O, ER) do {                               \
        if (Pdata_push((D), (O)) < 0) return (ER); } while(0)

/* Push an object on stack, adding a new reference to the object. */
#define PDATA_APPEND(D, O, ER) do {                             \
        Py_INCREF((O));                                         \
        if (Pdata_push((D), (O)) < 0) return (ER); } while(0)

static PyObject *
Pdata_poptuple(Pdata *self, Py_ssize_t start)
{
    PyObject *tuple;
    Py_ssize_t len, i, j;

    if (start < self->fence) {
        Pdata_stack_underflow(self);
        return NULL;
    }
    len = Py_SIZE(self) - start;
    tuple = PyTuple_New(len);
    if (tuple == NULL)
        return NULL;
    for (i = start, j = 0; j < len; i++, j++)
        PyTuple_SET_ITEM(tuple, j, self->data[i]);

    Py_SIZE(self) = start;
    return tuple;
}

static PyObject *
Pdata_poplist(Pdata *self, Py_ssize_t start)
{
    PyObject *list;
    Py_ssize_t len, i, j;

    len = Py_SIZE(self) - start;
    list = PyList_New(len);
    if (list == NULL)
        return NULL;
    for (i = start, j = 0; j < len; i++, j++)
        PyList_SET_ITEM(list, j, self->data[i]);

    Py_SIZE(self) = start;
    return list;
}

typedef struct {
    PyObject *me_key;
    Py_ssize_t me_value;
} PyMemoEntry;

typedef struct {
    size_t mt_mask;
    size_t mt_used;
    size_t mt_allocated;
    PyMemoEntry *mt_table;
} PyMemoTable;

typedef struct PicklerObject {
    PyObject_HEAD
    PyMemoTable *memo;          /* Memo table, keep track of the seen
                                   objects to support self-referential objects
                                   pickling. */

    PyObject *write;            /* write() method of the output stream. */
    PyObject *output_buffer;    /* Write into a local bytearray buffer before
                                   flushing to the stream. */
    Py_ssize_t output_len;      /* Length of output_buffer. */
    Py_ssize_t max_output_len;  /* Allocation size of output_buffer. */
    int proto;                  /* Pickle protocol number, >= 0 */

    Py_ssize_t buf_size;        /* Size of the current buffered pickle data */
    int fast;                   /* Enable fast mode if set to a true value.
                                   The fast mode disable the usage of memo,
                                   therefore speeding the pickling process by
                                   not generating superfluous PUT opcodes. It
                                   should not be used if with self-referential
                                   objects. */
    PyObject *buffer_callback;  /* Callback for out-of-band buffers, or NULL */
} PicklerObject;

typedef struct UnpicklerObject {
    PyObject_HEAD
    Pdata *stack;               /* Pickle data stack, store unpickled objects. */

    /* The unpickler memo is just an array of PyObject *s. Using a dict
       is unnecessary, since the keys are contiguous ints. */
    PyObject **memo;
    size_t memo_size;       /* Capacity of the memo array */
    size_t memo_len;        /* Number of objects in the memo */

    Py_buffer buffer;
    char *input_buffer;
    char *input_line;
    Py_ssize_t input_len;
    Py_ssize_t next_read_idx;
    Py_ssize_t prefetched_idx;  /* index of first prefetched byte */

    PyObject *read;             /* read() method of the input stream. */
    PyObject *readinto;         /* readinto() method of the input stream. */
    PyObject *readline;         /* readline() method of the input stream. */
    PyObject *peek;             /* peek() method of the input stream, or NULL */
    PyObject *buffers;          /* iterable of out-of-band buffers, or NULL */

    Py_ssize_t *marks;          /* Mark stack, used for unpickling container
                                   objects. */
    Py_ssize_t num_marks;       /* Number of marks in the mark stack. */
    Py_ssize_t marks_size;      /* Current allocated size of the mark stack. */
    int proto;                  /* Protocol of the pickle loaded. */
} UnpicklerObject;

/* Forward declarations */
static int save(PicklerObject *, PyObject *);
static PyTypeObject Pickler_Type;
static PyTypeObject Unpickler_Type;

#include "_pickle.c.h"

/*************************************************************************
 A custom hashtable mapping void* to Python ints. This is used by the pickler
 for memoization. Using a custom hashtable rather than PyDict allows us to skip
 a bunch of unnecessary object creation. This makes a huge performance
 difference. */

#define MT_MINSIZE 8
#define PERTURB_SHIFT 5


static PyMemoTable *
PyMemoTable_New(void)
{
    PyMemoTable *memo = PyMem_MALLOC(sizeof(PyMemoTable));
    if (memo == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    memo->mt_used = 0;
    memo->mt_allocated = MT_MINSIZE;
    memo->mt_mask = MT_MINSIZE - 1;
    memo->mt_table = PyMem_MALLOC(MT_MINSIZE * sizeof(PyMemoEntry));
    if (memo->mt_table == NULL) {
        PyMem_FREE(memo);
        PyErr_NoMemory();
        return NULL;
    }
    memset(memo->mt_table, 0, MT_MINSIZE * sizeof(PyMemoEntry));

    return memo;
}

static Py_ssize_t
PyMemoTable_Size(PyMemoTable *self)
{
    return self->mt_used;
}

static int
PyMemoTable_Clear(PyMemoTable *self)
{
    Py_ssize_t i = self->mt_allocated;

    while (--i >= 0) {
        Py_XDECREF(self->mt_table[i].me_key);
    }
    self->mt_used = 0;
    memset(self->mt_table, 0, self->mt_allocated * sizeof(PyMemoEntry));
    return 0;
}

static void
PyMemoTable_Del(PyMemoTable *self)
{
    if (self == NULL)
        return;
    PyMemoTable_Clear(self);

    PyMem_FREE(self->mt_table);
    PyMem_FREE(self);
}

/* Since entries cannot be deleted from this hashtable, _PyMemoTable_Lookup()
   can be considerably simpler than dictobject.c's lookdict(). */
static PyMemoEntry *
_PyMemoTable_Lookup(PyMemoTable *self, PyObject *key)
{
    size_t i;
    size_t perturb;
    size_t mask = self->mt_mask;
    PyMemoEntry *table = self->mt_table;
    PyMemoEntry *entry;
    Py_hash_t hash = (Py_hash_t)key >> 3;

    i = hash & mask;
    entry = &table[i];
    if (entry->me_key == NULL || entry->me_key == key)
        return entry;

    for (perturb = hash; ; perturb >>= PERTURB_SHIFT) {
        i = (i << 2) + i + perturb + 1;
        entry = &table[i & mask];
        if (entry->me_key == NULL || entry->me_key == key)
            return entry;
    }
    Py_UNREACHABLE();
}

/* Returns -1 on failure, 0 on success. */
static int
_PyMemoTable_ResizeTable(PyMemoTable *self, size_t min_size)
{
    PyMemoEntry *oldtable = NULL;
    PyMemoEntry *oldentry, *newentry;
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
    oldtable = self->mt_table;
    self->mt_table = PyMem_NEW(PyMemoEntry, new_size);
    if (self->mt_table == NULL) {
        self->mt_table = oldtable;
        PyErr_NoMemory();
        return -1;
    }
    self->mt_allocated = new_size;
    self->mt_mask = new_size - 1;
    memset(self->mt_table, 0, sizeof(PyMemoEntry) * new_size);

    /* Copy entries from the old table. */
    to_process = self->mt_used;
    for (oldentry = oldtable; to_process > 0; oldentry++) {
        if (oldentry->me_key != NULL) {
            to_process--;
            /* newentry is a pointer to a chunk of the new
               mt_table, so we're setting the key:value pair
               in-place. */
            newentry = _PyMemoTable_Lookup(self, oldentry->me_key);
            newentry->me_key = oldentry->me_key;
            newentry->me_value = oldentry->me_value;
        }
    }

    /* Deallocate the old table. */
    PyMem_FREE(oldtable);
    return 0;
}

/* Returns NULL on failure, a pointer to the value otherwise. */
static Py_ssize_t *
PyMemoTable_Get(PyMemoTable *self, PyObject *key)
{
    PyMemoEntry *entry = _PyMemoTable_Lookup(self, key);
    if (entry->me_key == NULL)
        return NULL;
    return &entry->me_value;
}

/* Returns -1 on failure, 0 on success. */
static int
PyMemoTable_Set(PyMemoTable *self, PyObject *key, Py_ssize_t value)
{
    PyMemoEntry *entry;

    assert(key != NULL);

    entry = _PyMemoTable_Lookup(self, key);
    if (entry->me_key != NULL) {
        entry->me_value = value;
        return 0;
    }
    Py_INCREF(key);
    entry->me_key = key;
    entry->me_value = value;
    self->mt_used++;

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
    if (SIZE_MAX / 3 >= self->mt_used && self->mt_used * 3 < self->mt_allocated * 2) {
        return 0;
    }
    // self->mt_used is always < PY_SSIZE_T_MAX, so this can't overflow.
    size_t desired_size = (self->mt_used > 50000 ? 2 : 4) * self->mt_used;
    return _PyMemoTable_ResizeTable(self, desired_size);
}

#undef MT_MINSIZE
#undef PERTURB_SHIFT

/*************************************************************************/


static int
_Pickler_ClearBuffer(PicklerObject *self)
{
    Py_XSETREF(self->output_buffer,
              PyBytes_FromStringAndSize(NULL, self->max_output_len));
    if (self->output_buffer == NULL)
        return -1;
    self->output_len = 0;
    return 0;
}

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

static PyObject *
_Pickler_GetString(PicklerObject *self)
{
    PyObject *output_buffer = self->output_buffer;

    assert(self->output_buffer != NULL);

    self->output_buffer = NULL;
    /* Resize down to exact size */
    if (_PyBytes_Resize(&output_buffer, self->output_len) < 0)
        return NULL;
    return output_buffer;
}

static int
_Pickler_FlushToFile(PicklerObject *self)
{
    PyObject *output, *result;

    assert(self->write != NULL);

    output = _Pickler_GetString(self);
    if (output == NULL)
        return -1;

    result = _Pickle_FastCall(self->write, output);
    Py_XDECREF(result);
    return (result == NULL) ? -1 : 0;
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

static PicklerObject *
_Pickler_New(void)
{
    PicklerObject *self;

    self = PyObject_GC_New(PicklerObject, &Pickler_Type);
    if (self == NULL)
        return NULL;

    self->buffer_callback = NULL;
    self->write = NULL;
    self->proto = 0;
    self->fast = 0;
    self->max_output_len = WRITE_BUF_SIZE;
    self->output_len = 0;

    self->memo = PyMemoTable_New();
    self->output_buffer = PyBytes_FromStringAndSize(NULL,
                                                    self->max_output_len);

    if (self->memo == NULL || self->output_buffer == NULL) {
        Py_DECREF(self);
        return NULL;
    }

    PyObject_GC_Track(self);
    return self;
}

static int
_Pickler_SetProtocol(PicklerObject *self, PyObject *protocol)
{
    long proto;

    if (protocol == Py_None) {
        proto = DEFAULT_PROTOCOL;
    }
    else {
        proto = PyLong_AsLong(protocol);
        if (proto < 0) {
            if (proto == -1 && PyErr_Occurred())
                return -1;
            proto = HIGHEST_PROTOCOL;
        }
        else if (proto > HIGHEST_PROTOCOL) {
            PyErr_Format(PyExc_ValueError, "pickle protocol must be <= %d",
                         HIGHEST_PROTOCOL);
            return -1;
        }
        else if (proto < LOWEST_PROTOCOL) {
            PyErr_Format(PyExc_ValueError, "pickle protocol must be >= %d",
                         LOWEST_PROTOCOL);
            return -1;
        }
    }
    self->proto = (int)proto;
    return 0;
}

/* Returns -1 (with an exception set) on failure, 0 on success. This may
   be called once on a freshly created Pickler. */
static int
_Pickler_SetOutputStream(PicklerObject *self, PyObject *file)
{
    _Py_IDENTIFIER(write);
    assert(file != NULL);
    if (_PyObject_LookupAttrId(file, &PyId_write, &self->write) < 0) {
        return -1;
    }
    if (self->write == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "file must have a 'write' attribute");
        return -1;
    }

    return 0;
}

static int
_Pickler_SetBufferCallback(PicklerObject *self, PyObject *buffer_callback)
{
    if (buffer_callback == Py_None) {
        buffer_callback = NULL;
    }

    Py_XINCREF(buffer_callback);
    self->buffer_callback = buffer_callback;
    return 0;
}

/* Returns the size of the input on success, -1 on failure. This takes its
   own reference to `input`. */
static Py_ssize_t
_Unpickler_SetStringInput(UnpicklerObject *self, PyObject *input)
{
    if (self->buffer.buf != NULL)
        PyBuffer_Release(&self->buffer);
    if (PyObject_GetBuffer(input, &self->buffer, PyBUF_CONTIG_RO) < 0)
        return -1;
    self->input_buffer = self->buffer.buf;
    self->input_len = self->buffer.len;
    self->next_read_idx = 0;
    self->prefetched_idx = self->input_len;
    return self->input_len;
}

static int
bad_readline(void)
{
    PickleState *st = _Pickle_GetGlobalState();
    PyErr_SetString(st->UnpicklingError, "pickle data was truncated");
    return -1;
}

/* Skip any consumed data that was only prefetched using peek() */
static int
_Unpickler_SkipConsumed(UnpicklerObject *self)
{
    Py_ssize_t consumed;
    PyObject *r;

    consumed = self->next_read_idx - self->prefetched_idx;
    if (consumed <= 0)
        return 0;

    assert(self->peek);  /* otherwise we did something wrong */
    /* This makes a useless copy... */
    r = PyObject_CallFunction(self->read, "n", consumed);
    if (r == NULL)
        return -1;
    Py_DECREF(r);

    self->prefetched_idx = self->next_read_idx;
    return 0;
}

static const Py_ssize_t READ_WHOLE_LINE = -1;

/* If reading from a file, we need to only pull the bytes we need, since there
   may be multiple pickle objects arranged contiguously in the same input
   buffer.

   If `n` is READ_WHOLE_LINE, read a whole line. Otherwise, read up to `n`
   bytes from the input stream/buffer.

   Update the unpickler's input buffer with the newly-read data. Returns -1 on
   failure; on success, returns the number of bytes read from the file.

   On success, self->input_len will be 0; this is intentional so that when
   unpickling from a file, the "we've run out of data" code paths will trigger,
   causing the Unpickler to go back to the file for more data. Use the returned
   size to tell you how much data you can process. */
static Py_ssize_t
_Unpickler_ReadFromFile(UnpicklerObject *self, Py_ssize_t n)
{
    PyObject *data;
    Py_ssize_t read_size;

    assert(self->read != NULL);

    if (_Unpickler_SkipConsumed(self) < 0)
        return -1;

    if (n == READ_WHOLE_LINE) {
        data = _PyObject_CallNoArg(self->readline);
    }
    else {
        PyObject *len;
        /* Prefetch some data without advancing the file pointer, if possible */
        if (self->peek && n < PREFETCH) {
            len = PyLong_FromSsize_t(PREFETCH);
            if (len == NULL)
                return -1;
            data = _Pickle_FastCall(self->peek, len);
            if (data == NULL) {
                if (!PyErr_ExceptionMatches(PyExc_NotImplementedError))
                    return -1;
                /* peek() is probably not supported by the given file object */
                PyErr_Clear();
                Py_CLEAR(self->peek);
            }
            else {
                read_size = _Unpickler_SetStringInput(self, data);
                Py_DECREF(data);
                self->prefetched_idx = 0;
                if (n <= read_size)
                    return n;
            }
        }
        len = PyLong_FromSsize_t(n);
        if (len == NULL)
            return -1;
        data = _Pickle_FastCall(self->read, len);
    }
    if (data == NULL)
        return -1;

    read_size = _Unpickler_SetStringInput(self, data);
    Py_DECREF(data);
    return read_size;
}

/* Don't call it directly: use _Unpickler_Read() */
static Py_ssize_t
_Unpickler_ReadImpl(UnpicklerObject *self, char **s, Py_ssize_t n)
{
    Py_ssize_t num_read;

    *s = NULL;
    if (self->next_read_idx > PY_SSIZE_T_MAX - n) {
        PickleState *st = _Pickle_GetGlobalState();
        PyErr_SetString(st->UnpicklingError,
                        "read would overflow (invalid bytecode)");
        return -1;
    }

    /* This case is handled by the _Unpickler_Read() macro for efficiency */
    assert(self->next_read_idx + n > self->input_len);

    if (!self->read)
        return bad_readline();

    /* Extend the buffer to satisfy desired size */
    num_read = _Unpickler_ReadFromFile(self, n);
    if (num_read < 0)
        return -1;
    if (num_read < n)
        return bad_readline();
    *s = self->input_buffer;
    self->next_read_idx = n;
    return n;
}

/* Read `n` bytes from the unpickler's data source, storing the result in `buf`.
 *
 * This should only be used for non-small data reads where potentially
 * avoiding a copy is beneficial.  This method does not try to prefetch
 * more data into the input buffer.
 *
 * _Unpickler_Read() is recommended in most cases.
 */
static Py_ssize_t
_Unpickler_ReadInto(UnpicklerObject *self, char *buf, Py_ssize_t n)
{
    assert(n != READ_WHOLE_LINE);

    /* Read from available buffer data, if any */
    Py_ssize_t in_buffer = self->input_len - self->next_read_idx;
    if (in_buffer > 0) {
        Py_ssize_t to_read = Py_MIN(in_buffer, n);
        memcpy(buf, self->input_buffer + self->next_read_idx, to_read);
        self->next_read_idx += to_read;
        buf += to_read;
        n -= to_read;
        if (n == 0) {
            /* Entire read was satisfied from buffer */
            return n;
        }
    }

    /* Read from file */
    if (!self->read) {
        /* We're unpickling memory, this means the input is truncated */
        return bad_readline();
    }
    if (_Unpickler_SkipConsumed(self) < 0) {
        return -1;
    }

    if (!self->readinto) {
        /* readinto() not supported on file-like object, fall back to read()
         * and copy into destination buffer (bpo-39681) */
        PyObject* len = PyLong_FromSsize_t(n);
        if (len == NULL) {
            return -1;
        }
        PyObject* data = _Pickle_FastCall(self->read, len);
        if (data == NULL) {
            return -1;
        }
        if (!PyBytes_Check(data)) {
            PyErr_Format(PyExc_ValueError,
                         "read() returned non-bytes object (%R)",
                         Py_TYPE(data));
            Py_DECREF(data);
            return -1;
        }
        Py_ssize_t read_size = PyBytes_GET_SIZE(data);
        if (read_size < n) {
            Py_DECREF(data);
            return bad_readline();
        }
        memcpy(buf, PyBytes_AS_STRING(data), n);
        Py_DECREF(data);
        return n;
    }

    /* Call readinto() into user buffer */
    PyObject *buf_obj = PyMemoryView_FromMemory(buf, n, PyBUF_WRITE);
    if (buf_obj == NULL) {
        return -1;
    }
    PyObject *read_size_obj = _Pickle_FastCall(self->readinto, buf_obj);
    if (read_size_obj == NULL) {
        return -1;
    }
    Py_ssize_t read_size = PyLong_AsSsize_t(read_size_obj);
    Py_DECREF(read_size_obj);

    if (read_size < 0) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError,
                            "readinto() returned negative size");
        }
        return -1;
    }
    if (read_size < n) {
        return bad_readline();
    }
    return n;
}

/* Read `n` bytes from the unpickler's data source, storing the result in `*s`.

   This should be used for all data reads, rather than accessing the unpickler's
   input buffer directly. This method deals correctly with reading from input
   streams, which the input buffer doesn't deal with.

   Note that when reading from a file-like object, self->next_read_idx won't
   be updated (it should remain at 0 for the entire unpickling process). You
   should use this function's return value to know how many bytes you can
   consume.

   Returns -1 (with an exception set) on failure. On success, return the
   number of chars read. */
#define _Unpickler_Read(self, s, n) \
    (((n) <= (self)->input_len - (self)->next_read_idx)      \
     ? (*(s) = (self)->input_buffer + (self)->next_read_idx, \
        (self)->next_read_idx += (n),                        \
        (n))                                                 \
     : _Unpickler_ReadImpl(self, (s), (n)))


/* Returns -1 (with an exception set) on failure, 0 on success. The memo array
   will be modified in place. */
static int
_Unpickler_ResizeMemoList(UnpicklerObject *self, size_t new_size)
{
    size_t i;

    assert(new_size > self->memo_size);

    PyObject **memo_new = self->memo;
    PyMem_RESIZE(memo_new, PyObject *, new_size);
    if (memo_new == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    self->memo = memo_new;
    for (i = self->memo_size; i < new_size; i++)
        self->memo[i] = NULL;
    self->memo_size = new_size;
    return 0;
}

/* Returns NULL if idx is out of bounds. */
static PyObject *
_Unpickler_MemoGet(UnpicklerObject *self, size_t idx)
{
    if (idx >= self->memo_size)
        return NULL;

    return self->memo[idx];
}

/* Returns -1 (with an exception set) on failure, 0 on success.
   This takes its own reference to `value`. */
static int
_Unpickler_MemoPut(UnpicklerObject *self, size_t idx, PyObject *value)
{
    PyObject *old_item;

    if (idx >= self->memo_size) {
        if (_Unpickler_ResizeMemoList(self, idx * 2) < 0)
            return -1;
        assert(idx < self->memo_size);
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

static PyObject **
_Unpickler_NewMemo(Py_ssize_t new_size)
{
    PyObject **memo = PyMem_NEW(PyObject *, new_size);
    if (memo == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    memset(memo, 0, new_size * sizeof(PyObject *));
    return memo;
}

/* Free the unpickler's memo, taking care to decref any items left in it. */
static void
_Unpickler_MemoCleanup(UnpicklerObject *self)
{
    Py_ssize_t i;
    PyObject **memo = self->memo;

    if (self->memo == NULL)
        return;
    self->memo = NULL;
    i = self->memo_size;
    while (--i >= 0) {
        Py_XDECREF(memo[i]);
    }
    PyMem_FREE(memo);
}

static UnpicklerObject *
_Unpickler_New(void)
{
    UnpicklerObject *self;

    self = PyObject_GC_New(UnpicklerObject, &Unpickler_Type);
    if (self == NULL)
        return NULL;

    self->input_buffer = NULL;
    self->input_line = NULL;
    self->input_len = 0;
    self->next_read_idx = 0;
    self->prefetched_idx = 0;
    self->read = NULL;
    self->readinto = NULL;
    self->readline = NULL;
    self->peek = NULL;
    self->buffers = NULL;
    self->marks = NULL;
    self->num_marks = 0;
    self->marks_size = 0;
    self->proto = 0;
    memset(&self->buffer, 0, sizeof(Py_buffer));
    self->memo_size = 32;
    self->memo_len = 0;
    self->memo = _Unpickler_NewMemo(self->memo_size);
    self->stack = (Pdata *)Pdata_New();

    if (self->memo == NULL || self->stack == NULL) {
        Py_DECREF(self);
        return NULL;
    }

    PyObject_GC_Track(self);
    return self;
}

/* Returns -1 (with an exception set) on failure, 0 on success. This may
   be called once on a freshly created Unpickler. */
static int
_Unpickler_SetInputStream(UnpicklerObject *self, PyObject *file)
{
    _Py_IDENTIFIER(peek);
    _Py_IDENTIFIER(read);
    _Py_IDENTIFIER(readinto);
    _Py_IDENTIFIER(readline);

    /* Optional file methods */
    if (_PyObject_LookupAttrId(file, &PyId_peek, &self->peek) < 0) {
        return -1;
    }
    if (_PyObject_LookupAttrId(file, &PyId_readinto, &self->readinto) < 0) {
        return -1;
    }
    (void)_PyObject_LookupAttrId(file, &PyId_read, &self->read);
    (void)_PyObject_LookupAttrId(file, &PyId_readline, &self->readline);
    if (!self->readline || !self->read) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError,
                            "file must have 'read' and 'readline' attributes");
        }
        Py_CLEAR(self->read);
        Py_CLEAR(self->readinto);
        Py_CLEAR(self->readline);
        Py_CLEAR(self->peek);
        return -1;
    }
    return 0;
}

/* Returns -1 (with an exception set) on failure, 0 on success. This may
   be called once on a freshly created Unpickler. */
static int
_Unpickler_SetBuffers(UnpicklerObject *self, PyObject *buffers)
{
    if (buffers == NULL || buffers == Py_None) {
        self->buffers = NULL;
    }
    else {
        self->buffers = PyObject_GetIter(buffers);
        if (self->buffers == NULL) {
            return -1;
        }
    }
    return 0;
}

/* Generate a GET opcode for an object stored in the memo. */
static int
memo_get(PicklerObject *self, PyObject *key)
{
    Py_ssize_t *value;
    char pdata[30];
    Py_ssize_t len;

    value = PyMemoTable_Get(self->memo, key);
    if (value == NULL)  {
        PyErr_SetObject(PyExc_KeyError, key);
        return -1;
    }

    if (*value < 256) {
        pdata[0] = BINGET;
        pdata[1] = (unsigned char)(*value & 0xff);
        len = 2;
    }
    else if ((size_t)*value <= 0xffffffffUL) {
        pdata[0] = LONG_BINGET;
        pdata[1] = (unsigned char)(*value & 0xff);
        pdata[2] = (unsigned char)((*value >> 8) & 0xff);
        pdata[3] = (unsigned char)((*value >> 16) & 0xff);
        pdata[4] = (unsigned char)((*value >> 24) & 0xff);
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

    if (self->fast)
        return 0;

    idx = PyMemoTable_Size(self->memo);
    if (PyMemoTable_Set(self->memo, obj, idx) < 0)
        return -1;

    if (_Pickler_Write(self, &memoize_op, 1) < 0)
        return -1;
    return 0;
}

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

/* Perform direct write of the header and payload of the binary object.

   The large contiguous data is written directly into the underlying file
   object, bypassing the output_buffer of the Pickler.  We intentionally
   do not insert a protocol 4 frame opcode to make it possible to optimize
   file.read calls in the loader.
 */
static int
_Pickler_write_bytes(PicklerObject *self,
                     const char *header, Py_ssize_t header_size,
                     const char *data, Py_ssize_t data_size,
                     PyObject *payload)
{
    int bypass_buffer = (data_size >= FRAME_SIZE_TARGET);

    if (_Pickler_Write(self, header, header_size) < 0) {
        return -1;
    }

    if (bypass_buffer && self->write != NULL) {
        /* Bypass the in-memory buffer to directly stream large data
           into the underlying file object. */
        PyObject *result, *mem = NULL;
        /* Dump the output buffer to the file. */
        if (_Pickler_FlushToFile(self) < 0) {
            return -1;
        }

        /* Stream write the payload into the file without going through the
           output buffer. */
        if (payload == NULL) {
            /* TODO: It would be better to use a memoryview with a linked
               original string if this is possible. */
            payload = mem = PyBytes_FromStringAndSize(data, data_size);
            if (payload == NULL) {
                return -1;
            }
        }
        result = PyObject_CallFunctionObjArgs(self->write, payload, NULL);
        Py_XDECREF(mem);
        if (result == NULL) {
            return -1;
        }
        Py_DECREF(result);

        /* Reinitialize the buffer for subsequent calls to _Pickler_Write. */
        if (_Pickler_ClearBuffer(self) < 0) {
            return -1;
        }
    }
    else {
        if (_Pickler_Write(self, data, data_size) < 0) {
            return -1;
        }
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

    if (_Pickler_write_bytes(self, header, len, data, size, obj) < 0) {
        return -1;
    }

    if (memo_put(self, obj) < 0) {
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

    if (_Pickler_write_bytes(self, header, len, data, size, obj) < 0) {
        return -1;
    }

    if (memo_put(self, obj) < 0) {
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
    if (self->buffer_callback != NULL) {
        PyObject *ret = PyObject_CallFunctionObjArgs(self->buffer_callback,
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

    if (_Pickler_write_bytes(self, header, len, data, size, encoded) < 0) {
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
    Py_ssize_t len, i;

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

        if (PyMemoTable_Get(self->memo, obj)) {
            /* pop the len elements */
            for (i = 0; i < len; i++)
                if (_Pickler_Write(self, &pop_op, 1) < 0)
                    return -1;
            /* fetch from memo */
            if (memo_get(self, obj) < 0)
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

    if (PyMemoTable_Get(self->memo, obj)) {
        /* pop the stack stuff we pushed */
        if (_Pickler_Write(self, &pop_mark_op, 1) < 0)
            return -1;
        /* fetch from memo */
        if (memo_get(self, obj) < 0)
            return -1;

        return 0;
    }
    else { /* Not recursive. */
        if (_Pickler_Write(self, &tuple_op, 1) < 0)
            return -1;
    }

  memoize:
    if (memo_put(self, obj) < 0)
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

    if (memo_put(self, obj) < 0)
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

    if (memo_put(self, obj) < 0)
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

    if (memo_put(self, obj) < 0)
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
    if (PyMemoTable_Get(self->memo, obj)) {
        const char pop_mark_op = POP_MARK;

        if (_Pickler_Write(self, &pop_mark_op, 1) < 0)
            return -1;
        if (memo_get(self, obj) < 0)
            return -1;
        return 0;
    }

    if (_Pickler_Write(self, &frozenset_op, 1) < 0)
        return -1;
    if (memo_put(self, obj) < 0)
        return -1;

    return 0;
}

static int
save(PicklerObject *self, PyObject *obj)
{
    PyTypeObject *type;
    PyObject *reduce_func = NULL;
    PyObject *reduce_value = NULL;
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
    if (PyMemoTable_Get(self->memo, obj)) {
        return memo_get(self, obj);
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

static PyObject *
_pickle_Pickler_clear_memo_impl(PicklerObject *self)
{
    if (self->memo)
        PyMemoTable_Clear(self->memo);

    Py_RETURN_NONE;
}

static PyObject *
_pickle_Pickler_dump(PicklerObject *self, PyObject *obj)
{
    /* Check whether the Pickler was initialized correctly (issue3664).
       Developers often forget to call __init__() in their subclasses, which
       would trigger a segfault without this check. */
    if (self->write == NULL) {
        PickleState *st = _Pickle_GetGlobalState();
        PyErr_Format(st->PicklingError,
                     "Pickler.__init__() was not called by %s.__init__()",
                     Py_TYPE(self)->tp_name);
        return NULL;
    }

    if (_Pickler_ClearBuffer(self) < 0)
        return NULL;

    if (dump(self, obj) < 0)
        return NULL;

    if (_Pickler_FlushToFile(self) < 0)
        return NULL;

    Py_RETURN_NONE;
}

static Py_ssize_t
_pickle_Pickler___sizeof___impl(PicklerObject *self)
{
    Py_ssize_t res, s;

    res = _PyObject_SIZE(Py_TYPE(self));
    if (self->memo != NULL) {
        res += sizeof(PyMemoTable);
        res += self->memo->mt_allocated * sizeof(PyMemoEntry);
    }
    if (self->output_buffer != NULL) {
        s = _PySys_GetSizeOf(self->output_buffer);
        if (s == -1)
            return -1;
        res += s;
    }
    return res;
}

static struct PyMethodDef Pickler_methods[] = {
    _PICKLE_PICKLER_DUMP_METHODDEF
    _PICKLE_PICKLER_CLEAR_MEMO_METHODDEF
    _PICKLE_PICKLER___SIZEOF___METHODDEF
    {NULL, NULL}                /* sentinel */
};

static void
Pickler_dealloc(PicklerObject *self)
{
    PyObject_GC_UnTrack(self);

    Py_XDECREF(self->output_buffer);
    Py_XDECREF(self->write);
    Py_XDECREF(self->buffer_callback);

    PyMemoTable_Del(self->memo);

    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Pickler_traverse(PicklerObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->write);
    Py_VISIT(self->buffer_callback);
    return 0;
}

static int
Pickler_clear(PicklerObject *self)
{
    Py_CLEAR(self->output_buffer);
    Py_CLEAR(self->write);
    Py_CLEAR(self->buffer_callback);

    if (self->memo != NULL) {
        PyMemoTable *memo = self->memo;
        self->memo = NULL;
        PyMemoTable_Del(memo);
    }
    return 0;
}

static int
_pickle_Pickler___init___impl(PicklerObject *self, PyObject *file,
                              PyObject *protocol, int fix_imports,
                              PyObject *buffer_callback)
{
    /* In case of multiple __init__() calls, clear previous content. */
    if (self->write != NULL)
        (void)Pickler_clear(self);

    if (_Pickler_SetProtocol(self, protocol) < 0)
        return -1;

    if (_Pickler_SetOutputStream(self, file) < 0)
        return -1;

    if (_Pickler_SetBufferCallback(self, buffer_callback) < 0)
        return -1;

    /* memo and output_buffer may have already been created in _Pickler_New */
    if (self->memo == NULL) {
        self->memo = PyMemoTable_New();
        if (self->memo == NULL)
            return -1;
    }
    self->output_len = 0;
    if (self->output_buffer == NULL) {
        self->max_output_len = WRITE_BUF_SIZE;
        self->output_buffer = PyBytes_FromStringAndSize(NULL,
                                                        self->max_output_len);
        if (self->output_buffer == NULL)
            return -1;
    }
    return 0;
}

static PyTypeObject Pickler_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_pickle.Pickler"  ,                /*tp_name*/
    sizeof(PicklerObject),              /*tp_basicsize*/
    0,                                  /*tp_itemsize*/
    (destructor)Pickler_dealloc,        /*tp_dealloc*/
    0,                                  /*tp_vectorcall_offset*/
    0,                                  /*tp_getattr*/
    0,                                  /*tp_setattr*/
    0,                                  /*tp_as_async*/
    0,                                  /*tp_repr*/
    0,                                  /*tp_as_number*/
    0,                                  /*tp_as_sequence*/
    0,                                  /*tp_as_mapping*/
    0,                                  /*tp_hash*/
    0,                                  /*tp_call*/
    0,                                  /*tp_str*/
    0,                                  /*tp_getattro*/
    0,                                  /*tp_setattro*/
    0,                                  /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    _pickle_Pickler___init____doc__,    /*tp_doc*/
    (traverseproc)Pickler_traverse,     /*tp_traverse*/
    (inquiry)Pickler_clear,             /*tp_clear*/
    0,                                  /*tp_richcompare*/
    0,                                  /*tp_weaklistoffset*/
    0,                                  /*tp_iter*/
    0,                                  /*tp_iternext*/
    Pickler_methods,                    /*tp_methods*/
    0,                                  /*tp_members*/
    0,                                  /*tp_getset*/
    0,                                  /*tp_base*/
    0,                                  /*tp_dict*/
    0,                                  /*tp_descr_get*/
    0,                                  /*tp_descr_set*/
    0,                                  /*tp_dictoffset*/
    _pickle_Pickler___init__,           /*tp_init*/
    PyType_GenericAlloc,                /*tp_alloc*/
    PyType_GenericNew,                  /*tp_new*/
    PyObject_GC_Del,                    /*tp_free*/
    0,                                  /*tp_is_gc*/
};

static Py_ssize_t
marker(UnpicklerObject *self)
{
    Py_ssize_t mark;

    if (self->num_marks < 1) {
        PickleState *st = _Pickle_GetGlobalState();
        PyErr_SetString(st->UnpicklingError, "could not find MARK");
        return -1;
    }

    mark = self->marks[--self->num_marks];
    self->stack->mark_set = self->num_marks != 0;
    self->stack->fence = self->num_marks ?
            self->marks[self->num_marks - 1] : 0;
    return mark;
}

static int
load_none(UnpicklerObject *self)
{
    PDATA_APPEND(self->stack, Py_None, -1);
    return 0;
}

static int
load_bool(UnpicklerObject *self, PyObject *boolean)
{
    assert(boolean == Py_True || boolean == Py_False);
    PDATA_APPEND(self->stack, boolean, -1);
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

    PDATA_PUSH(self->stack, value, -1);
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
    PDATA_PUSH(self->stack, value, -1);
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

    PDATA_PUSH(self->stack, value, -1);
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

    PDATA_PUSH(self->stack, bytes, -1);
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

    PDATA_PUSH(self->stack, bytearray, -1);
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

    PDATA_PUSH(self->stack, buf, -1);
    return 0;
}

static int
load_readonly_buffer(UnpicklerObject *self)
{
    Py_ssize_t len = Py_SIZE(self->stack);
    if (len <= self->stack->fence) {
        return Pdata_stack_underflow(self->stack);
    }

    PyObject *obj = self->stack->data[len - 1];
    PyObject *view = PyMemoryView_FromObject(obj);
    if (view == NULL) {
        return -1;
    }
    if (!PyMemoryView_GET_BUFFER(view)->readonly) {
        /* Original object is writable */
        PyMemoryView_GET_BUFFER(view)->readonly = 1;
        self->stack->data[len - 1] = view;
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

    PDATA_PUSH(self->stack, str, -1);
    return 0;
}

static int
load_counted_tuple(UnpicklerObject *self, Py_ssize_t len)
{
    PyObject *tuple;

    if (Py_SIZE(self->stack) < len)
        return Pdata_stack_underflow(self->stack);

    tuple = Pdata_poptuple(self->stack, Py_SIZE(self->stack) - len);
    if (tuple == NULL)
        return -1;
    PDATA_PUSH(self->stack, tuple, -1);
    return 0;
}

static int
load_tuple(UnpicklerObject *self)
{
    Py_ssize_t i;

    if ((i = marker(self)) < 0)
        return -1;

    return load_counted_tuple(self, Py_SIZE(self->stack) - i);
}

static int
load_empty_list(UnpicklerObject *self)
{
    PyObject *list;

    if ((list = PyList_New(0)) == NULL)
        return -1;
    PDATA_PUSH(self->stack, list, -1);
    return 0;
}

static int
load_empty_dict(UnpicklerObject *self)
{
    PyObject *dict;

    if ((dict = PyDict_New()) == NULL)
        return -1;
    PDATA_PUSH(self->stack, dict, -1);
    return 0;
}

static int
load_empty_set(UnpicklerObject *self)
{
    PyObject *set;

    if ((set = PySet_New(NULL)) == NULL)
        return -1;
    PDATA_PUSH(self->stack, set, -1);
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

    items = Pdata_poptuple(self->stack, i);
    if (items == NULL)
        return -1;

    frozenset = PyFrozenSet_New(items);
    Py_DECREF(items);
    if (frozenset == NULL)
        return -1;

    PDATA_PUSH(self->stack, frozenset, -1);
    return 0;
}

static int
load_pop(UnpicklerObject *self)
{
    Py_ssize_t len = Py_SIZE(self->stack);

    /* Note that we split the (pickle.py) stack into two stacks,
     * an object stack and a mark stack. We have to be clever and
     * pop the right one. We do this by looking at the top of the
     * mark stack first, and only signalling a stack underflow if
     * the object stack is empty and the mark stack doesn't match
     * our expectations.
     */
    if (self->num_marks > 0 && self->marks[self->num_marks - 1] == len) {
        self->num_marks--;
        self->stack->mark_set = self->num_marks != 0;
        self->stack->fence = self->num_marks ?
                self->marks[self->num_marks - 1] : 0;
    } else if (len <= self->stack->fence)
        return Pdata_stack_underflow(self->stack);
    else {
        len--;
        Py_DECREF(self->stack->data[len]);
        Py_SIZE(self->stack) = len;
    }
    return 0;
}

static int
load_pop_mark(UnpicklerObject *self)
{
    Py_ssize_t i;

    if ((i = marker(self)) < 0)
        return -1;

    Pdata_clear(self->stack, i);

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

    value = _Unpickler_MemoGet(self, idx);
    if (value == NULL) {
        PyObject *key = PyLong_FromSsize_t(idx);
        if (key != NULL) {
            PyErr_SetObject(PyExc_KeyError, key);
            Py_DECREF(key);
        }
        return -1;
    }

    PDATA_APPEND(self->stack, value, -1);
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

    value = _Unpickler_MemoGet(self, idx);
    if (value == NULL) {
        PyObject *key = PyLong_FromSsize_t(idx);
        if (key != NULL) {
            PyErr_SetObject(PyExc_KeyError, key);
            Py_DECREF(key);
        }
        return -1;
    }

    PDATA_APPEND(self->stack, value, -1);
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

    if (Py_SIZE(self->stack) <= self->stack->fence)
        return Pdata_stack_underflow(self->stack);
    value = self->stack->data[Py_SIZE(self->stack) - 1];

    idx = Py_CHARMASK(s[0]);

    return _Unpickler_MemoPut(self, idx, value);
}

static int
load_long_binput(UnpicklerObject *self)
{
    PyObject *value;
    Py_ssize_t idx;
    char *s;

    if (_Unpickler_Read(self, &s, 4) < 0)
        return -1;

    if (Py_SIZE(self->stack) <= self->stack->fence)
        return Pdata_stack_underflow(self->stack);
    value = self->stack->data[Py_SIZE(self->stack) - 1];

    idx = calc_binsize(s, 4);
    if (idx < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "negative LONG_BINPUT argument");
        return -1;
    }

    return _Unpickler_MemoPut(self, idx, value);
}

static int
load_memoize(UnpicklerObject *self)
{
    PyObject *value;

    if (Py_SIZE(self->stack) <= self->stack->fence)
        return Pdata_stack_underflow(self->stack);
    value = self->stack->data[Py_SIZE(self->stack) - 1];

    return _Unpickler_MemoPut(self, self->memo_len, value);
}

static int
do_append(UnpicklerObject *self, Py_ssize_t x)
{
    PyObject *value;
    PyObject *slice;
    PyObject *list;
    PyObject *result;
    Py_ssize_t len, i;

    len = Py_SIZE(self->stack);
    if (x > len || x <= self->stack->fence)
        return Pdata_stack_underflow(self->stack);
    if (len == x)  /* nothing to do */
        return 0;

    list = self->stack->data[x - 1];

    if (PyList_CheckExact(list)) {
        Py_ssize_t list_len;
        int ret;

        slice = Pdata_poplist(self->stack, x);
        if (!slice)
            return -1;
        list_len = PyList_GET_SIZE(list);
        ret = PyList_SetSlice(list, list_len, list_len, slice);
        Py_DECREF(slice);
        return ret;
    }
    else {
        PyObject *extend_func;
        _Py_IDENTIFIER(extend);

        if (_PyObject_LookupAttrId(list, &PyId_extend, &extend_func) < 0) {
            return -1;
        }
        if (extend_func != NULL) {
            slice = Pdata_poplist(self->stack, x);
            if (!slice) {
                Py_DECREF(extend_func);
                return -1;
            }
            result = _Pickle_FastCall(extend_func, slice);
            Py_DECREF(extend_func);
            if (result == NULL)
                return -1;
            Py_DECREF(result);
        }
        else {
            PyObject *append_func;
            _Py_IDENTIFIER(append);

            /* Even if the PEP 307 requires extend() and append() methods,
               fall back on append() if the object has no extend() method
               for backward compatibility. */
            append_func = _PyObject_GetAttrId(list, &PyId_append);
            if (append_func == NULL)
                return -1;
            for (i = x; i < len; i++) {
                value = self->stack->data[i];
                result = _Pickle_FastCall(append_func, value);
                if (result == NULL) {
                    Pdata_clear(self->stack, i + 1);
                    Py_SIZE(self->stack) = x;
                    Py_DECREF(append_func);
                    return -1;
                }
                Py_DECREF(result);
            }
            Py_SIZE(self->stack) = x;
            Py_DECREF(append_func);
        }
    }

    return 0;
}

static int
load_append(UnpicklerObject *self)
{
    if (Py_SIZE(self->stack) - 1 <= self->stack->fence)
        return Pdata_stack_underflow(self->stack);
    return do_append(self, Py_SIZE(self->stack) - 1);
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

    len = Py_SIZE(self->stack);
    if (x > len || x <= self->stack->fence)
        return Pdata_stack_underflow(self->stack);
    if (len == x)  /* nothing to do */
        return 0;
    if ((len - x) % 2 != 0) {
        PickleState *st = _Pickle_GetGlobalState();
        /* Currupt or hostile pickle -- we never write one like this. */
        PyErr_SetString(st->UnpicklingError,
                        "odd number of items for SETITEMS");
        return -1;
    }

    /* Here, dict does not actually need to be a PyDict; it could be anything
       that supports the __setitem__ attribute. */
    dict = self->stack->data[x - 1];

    for (i = x + 1; i < len; i += 2) {
        key = self->stack->data[i - 1];
        value = self->stack->data[i];
        if (PyObject_SetItem(dict, key, value) < 0) {
            status = -1;
            break;
        }
    }

    Pdata_clear(self->stack, x);
    return status;
}

static int
load_setitem(UnpicklerObject *self)
{
    return do_setitems(self, Py_SIZE(self->stack) - 2);
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
    Py_ssize_t mark, len, i;

    mark =  marker(self);
    if (mark < 0)
        return -1;
    len = Py_SIZE(self->stack);
    if (mark > len || mark <= self->stack->fence)
        return Pdata_stack_underflow(self->stack);
    if (len == mark)  /* nothing to do */
        return 0;

    set = self->stack->data[mark - 1];

    if (PySet_Check(set)) {
        PyObject *items;
        int status;

        items = Pdata_poptuple(self->stack, mark);
        if (items == NULL)
            return -1;

        status = _PySet_Update(set, items);
        Py_DECREF(items);
        return status;
    }
    else {
        PyObject *add_func;
        _Py_IDENTIFIER(add);

        add_func = _PyObject_GetAttrId(set, &PyId_add);
        if (add_func == NULL)
            return -1;
        for (i = mark; i < len; i++) {
            PyObject *result;
            PyObject *item;

            item = self->stack->data[i];
            result = _Pickle_FastCall(add_func, item);
            if (result == NULL) {
                Pdata_clear(self->stack, i + 1);
                Py_SIZE(self->stack) = mark;
                return -1;
            }
            Py_DECREF(result);
        }
        Py_SIZE(self->stack) = mark;
    }

    return 0;
}

static int
load_mark(UnpicklerObject *self)
{

    /* Note that we split the (pickle.py) stack into two stacks, an
     * object stack and a mark stack. Here we push a mark onto the
     * mark stack.
     */

    if (self->num_marks >= self->marks_size) {
        size_t alloc = ((size_t)self->num_marks << 1) + 20;
        Py_ssize_t *marks_new = self->marks;
        PyMem_RESIZE(marks_new, Py_ssize_t, alloc);
        if (marks_new == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        self->marks = marks_new;
        self->marks_size = (Py_ssize_t)alloc;
    }

    self->stack->mark_set = 1;
    self->marks[self->num_marks++] = self->stack->fence = Py_SIZE(self->stack);

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
        self->proto = i;
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

    self->num_marks = 0;
    self->stack->mark_set = 0;
    self->stack->fence = 0;
    self->proto = 0;
    if (Py_SIZE(self->stack))
        Pdata_clear(self->stack, 0);

    /* Convenient macros for the dispatch while-switch loop just below. */
#define OP(opcode, load_func) \
    case opcode: if (load_func(self) < 0) break; continue;

#define OP_ARG(opcode, load_func, arg) \
    case opcode: if (load_func(self, (arg)) < 0) break; continue;

    while (1) {
        if (_Unpickler_Read(self, &s, 1) < 0) {
            PickleState *st = _Pickle_GetGlobalState();
            if (PyErr_ExceptionMatches(st->UnpicklingError)) {
                PyErr_Format(PyExc_EOFError, "Ran out of input");
            }
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

    if (_Unpickler_SkipConsumed(self) < 0)
        return NULL;

    PDATA_POP(self->stack, value);
    return value;
}

static PyObject *
_pickle_Unpickler_load_impl(UnpicklerObject *self)
{
    UnpicklerObject *unpickler = (UnpicklerObject*)self;

    /* Check whether the Unpickler was initialized correctly. This prevents
       segfaulting if a subclass overridden __init__ with a function that does
       not call Unpickler.__init__(). Here, we simply ensure that self->read
       is not NULL. */
    if (unpickler->read == NULL) {
        PickleState *st = _Pickle_GetGlobalState();
        PyErr_Format(st->UnpicklingError,
                     "Unpickler.__init__() was not called by %s.__init__()",
                     Py_TYPE(unpickler)->tp_name);
        return NULL;
    }

    return load(unpickler);
}


static Py_ssize_t
_pickle_Unpickler___sizeof___impl(UnpicklerObject *self)
{
    Py_ssize_t res;

    res = _PyObject_SIZE(Py_TYPE(self));
    if (self->memo != NULL)
        res += self->memo_size * sizeof(PyObject *);
    if (self->marks != NULL)
        res += self->marks_size * sizeof(Py_ssize_t);
    if (self->input_line != NULL)
        res += strlen(self->input_line) + 1;
    return res;
}

static struct PyMethodDef Unpickler_methods[] = {
    _PICKLE_UNPICKLER_LOAD_METHODDEF
    _PICKLE_UNPICKLER___SIZEOF___METHODDEF
    {NULL, NULL}                /* sentinel */
};

static void
Unpickler_dealloc(UnpicklerObject *self)
{
    PyObject_GC_UnTrack((PyObject *)self);
    Py_XDECREF(self->readline);
    Py_XDECREF(self->readinto);
    Py_XDECREF(self->read);
    Py_XDECREF(self->peek);
    Py_XDECREF(self->stack);
    Py_XDECREF(self->buffers);
    if (self->buffer.buf != NULL) {
        PyBuffer_Release(&self->buffer);
        self->buffer.buf = NULL;
    }

    _Unpickler_MemoCleanup(self);
    PyMem_Free(self->marks);
    PyMem_Free(self->input_line);

    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int
Unpickler_traverse(UnpicklerObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->readline);
    Py_VISIT(self->readinto);
    Py_VISIT(self->read);
    Py_VISIT(self->peek);
    Py_VISIT(self->stack);
    Py_VISIT(self->buffers);
    return 0;
}

static int
Unpickler_clear(UnpicklerObject *self)
{
    Py_CLEAR(self->readline);
    Py_CLEAR(self->readinto);
    Py_CLEAR(self->read);
    Py_CLEAR(self->peek);
    Py_CLEAR(self->stack);
    Py_CLEAR(self->buffers);
    if (self->buffer.buf != NULL) {
        PyBuffer_Release(&self->buffer);
        self->buffer.buf = NULL;
    }

    _Unpickler_MemoCleanup(self);
    PyMem_Free(self->marks);
    self->marks = NULL;
    PyMem_Free(self->input_line);
    self->input_line = NULL;

    return 0;
}

static int
_pickle_Unpickler___init___impl(UnpicklerObject *self, PyObject *file,
                                int fix_imports, const char *encoding,
                                const char *errors, PyObject *buffers)
{
    if (self->read != NULL)
        (void)Unpickler_clear(self);

    if (_Unpickler_SetInputStream(self, file) < 0)
        return -1;

    if (_Unpickler_SetBuffers(self, buffers) < 0)
        return -1;

    self->stack = (Pdata *)Pdata_New();
    if (self->stack == NULL)
        return -1;

    self->memo_size = 32;
    self->memo = _Unpickler_NewMemo(self->memo_size);
    if (self->memo == NULL)
        return -1;

    self->proto = 0;

    return 0;
}

static PyTypeObject Unpickler_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_pickle.Unpickler",                /*tp_name*/
    sizeof(UnpicklerObject),            /*tp_basicsize*/
    0,                                  /*tp_itemsize*/
    (destructor)Unpickler_dealloc,      /*tp_dealloc*/
    0,                                  /*tp_vectorcall_offset*/
    0,                                  /*tp_getattr*/
    0,                                  /*tp_setattr*/
    0,                                  /*tp_as_async*/
    0,                                  /*tp_repr*/
    0,                                  /*tp_as_number*/
    0,                                  /*tp_as_sequence*/
    0,                                  /*tp_as_mapping*/
    0,                                  /*tp_hash*/
    0,                                  /*tp_call*/
    0,                                  /*tp_str*/
    0,                                  /*tp_getattro*/
    0,                                  /*tp_setattro*/
    0,                                  /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    _pickle_Unpickler___init____doc__,  /*tp_doc*/
    (traverseproc)Unpickler_traverse,   /*tp_traverse*/
    (inquiry)Unpickler_clear,           /*tp_clear*/
    0,                                  /*tp_richcompare*/
    0,                                  /*tp_weaklistoffset*/
    0,                                  /*tp_iter*/
    0,                                  /*tp_iternext*/
    Unpickler_methods,                  /*tp_methods*/
    0,                                  /*tp_members*/
    0,                                  /*tp_getset*/
    0,                                  /*tp_base*/
    0,                                  /*tp_dict*/
    0,                                  /*tp_descr_get*/
    0,                                  /*tp_descr_set*/
    0,                                  /*tp_dictoffset*/
    _pickle_Unpickler___init__,         /*tp_init*/
    PyType_GenericAlloc,                /*tp_alloc*/
    PyType_GenericNew,                  /*tp_new*/
    PyObject_GC_Del,                    /*tp_free*/
    0,                                  /*tp_is_gc*/
};

static PyObject *
_pickle_dump_impl(PyObject *module, PyObject *obj, PyObject *file,
                  PyObject *protocol, int fix_imports,
                  PyObject *buffer_callback)
{
    PicklerObject *pickler = _Pickler_New();

    if (pickler == NULL)
        return NULL;

    if (_Pickler_SetProtocol(pickler, protocol) < 0)
        goto error;

    if (_Pickler_SetOutputStream(pickler, file) < 0)
        goto error;

    if (_Pickler_SetBufferCallback(pickler, buffer_callback) < 0)
        goto error;

    if (dump(pickler, obj) < 0)
        goto error;

    if (_Pickler_FlushToFile(pickler) < 0)
        goto error;

    Py_DECREF(pickler);
    Py_RETURN_NONE;

  error:
    Py_XDECREF(pickler);
    return NULL;
}

static PyObject *
_pickle_dumps_impl(PyObject *module, PyObject *obj, PyObject *protocol,
                   int fix_imports, PyObject *buffer_callback)
{
    PyObject *result;
    PicklerObject *pickler = _Pickler_New();

    if (pickler == NULL)
        return NULL;

    if (_Pickler_SetProtocol(pickler, protocol) < 0)
        goto error;

    if (_Pickler_SetBufferCallback(pickler, buffer_callback) < 0)
        goto error;

    if (dump(pickler, obj) < 0)
        goto error;

    result = _Pickler_GetString(pickler);
    Py_DECREF(pickler);
    return result;

  error:
    Py_XDECREF(pickler);
    return NULL;
}

static PyObject *
_pickle_load_impl(PyObject *module, PyObject *file, int fix_imports,
                  const char *encoding, const char *errors,
                  PyObject *buffers)
{
    PyObject *result;
    UnpicklerObject *unpickler = _Unpickler_New();

    if (unpickler == NULL)
        return NULL;

    if (_Unpickler_SetInputStream(unpickler, file) < 0)
        goto error;

    if (_Unpickler_SetBuffers(unpickler, buffers) < 0)
        goto error;

    result = load(unpickler);
    Py_DECREF(unpickler);
    return result;

  error:
    Py_XDECREF(unpickler);
    return NULL;
}

static PyObject *
_pickle_loads_impl(PyObject *module, PyObject *data, int fix_imports,
                   const char *encoding, const char *errors,
                   PyObject *buffers)
{
    PyObject *result;
    UnpicklerObject *unpickler = _Unpickler_New();

    if (unpickler == NULL)
        return NULL;

    if (_Unpickler_SetStringInput(unpickler, data) < 0)
        goto error;

    if (_Unpickler_SetBuffers(unpickler, buffers) < 0)
        goto error;

    result = load(unpickler);
    Py_DECREF(unpickler);
    return result;

  error:
    Py_XDECREF(unpickler);
    return NULL;
}

static struct PyMethodDef pickle_methods[] = {
    _PICKLE_DUMP_METHODDEF
    _PICKLE_DUMPS_METHODDEF
    _PICKLE_LOAD_METHODDEF
    _PICKLE_LOADS_METHODDEF
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
    pickle_module_doc,    /* m_doc */
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
    if (PyType_Ready(&Pdata_Type) < 0)
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
