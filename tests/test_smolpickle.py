import itertools
import pickle
import pickletools
import string
import sys
from distutils.version import StrictVersion

import pytest

import smolpickle


BATCHSIZE = 1000


def test_module_constants():
    assert smolpickle.HIGHEST_PROTOCOL == 5
    assert smolpickle.DEFAULT_PROTOCOL == 5


def test_picklebuffer_is_shared():
    assert pickle.PickleBuffer is smolpickle.PickleBuffer


def test_exceptions_are_unique():
    assert pickle.PickleError is not smolpickle.PickleError
    assert pickle.PicklingError is not smolpickle.PicklingError
    assert pickle.UnpicklingError is not smolpickle.UnpicklingError


def test_module_version():
    StrictVersion(smolpickle.__version__)


def test_pickler_init_errors():
    with pytest.raises(ValueError, match="pickle protocol must be <="):
        smolpickle.Pickler(protocol=6)

    with pytest.raises(ValueError, match="pickle protocol must be >="):
        smolpickle.Pickler(protocol=4)


def check(obj, sol=None):
    if sol is None:
        sol = obj

    smol_res = smolpickle.dumps(obj)
    obj2 = smolpickle.loads(smol_res)
    assert obj2 == sol
    assert type(obj2) is type(sol)

    obj3 = pickle.loads(smol_res)
    assert obj3 == sol
    assert type(obj3) is type(sol)

    pickle_res = pickle.dumps(obj, protocol=5)
    obj4 = smolpickle.loads(pickle_res)
    assert obj4 == sol
    assert type(obj4) is type(sol)


def test_pickle_none():
    check(None)


@pytest.mark.parametrize("value", [True, False])
def test_pickle_bool(value):
    check(value)


@pytest.mark.parametrize("nbytes", [1, 2, 4, 8, 254, 255, 256, 257])
@pytest.mark.parametrize("negative", [False, True])
def test_pickle_int(nbytes, negative):
    value = 2 ** (nbytes * 8 - 6)
    if negative:
        value *= -1

    check(value)


@pytest.mark.parametrize(
    "value",
    [
        0.0,
        4.94e-324,
        1e-310,
        7e-308,
        6.626e-34,
        0.1,
        0.5,
        3.14,
        263.44582062374053,
        6.022e23,
        1e30,
    ],
)
@pytest.mark.parametrize("negative", [False, True])
def test_pickle_float(value, negative):
    if negative:
        value *= -1

    check(value)


@pytest.mark.parametrize("nbytes", [0, 10, 512])
def test_pickle_bytes(nbytes):
    value = b"y" * nbytes
    check(value)


@pytest.mark.parametrize("nbytes", [0, 10, 512])
def test_pickle_bytearray(nbytes):
    value = bytearray(b"y" * nbytes)
    check(value)


@pytest.mark.parametrize("nbytes", [0, 10, 512])
def test_pickle_unicode(nbytes):
    value = "y" * nbytes
    check(value)


@pytest.mark.parametrize(
    "value",
    ["<\\u>", "<\\\u1234>", "<\n>", "<\\>", "\U00012345", "<\\\U00012345>", "<\udc80>"],
)
def test_pickle_unicode_edgecases(value):
    check(value)


@pytest.mark.parametrize("n", [0, 1, 5, 100, BATCHSIZE + 10])
def test_pickle_set(n):
    check(set(range(n)))


@pytest.mark.parametrize("n", [0, 1, 5, 100, BATCHSIZE + 10])
def test_pickle_frozenset(n):
    check(frozenset(range(n)))


@pytest.mark.parametrize("n", [0, 1, 2, 3, 100, BATCHSIZE + 10])
def test_pickle_tuple(n):
    check(tuple(range(n)))


def test_pickle_recursive_tuple():
    obj = ([None],)
    obj[0][0] = obj

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
        assert isinstance(obj2, tuple)
        assert obj2[0][0] is obj2
        # Fix the cycle so `==` works, then test
        obj2[0][0] = None
        assert obj2 == ([None],)


@pytest.mark.parametrize("n", [0, 1, 5, 100, BATCHSIZE + 10])
def test_pickle_list(n):
    check(list(range(n)))


def test_pickle_recursive_list():
    # self referential
    obj = []
    obj.append(obj)

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
        assert isinstance(obj2, list)
        assert obj2[0] is obj2
        assert len(obj2) == 1

    # one level removed
    obj = [[None]]
    obj[0][0] = obj

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
        assert isinstance(obj2, list)
        assert obj2[0][0] is obj2
        # Fix the cycle so `==` works, then test
        obj2[0][0] = None
        assert obj2 == [[None]]


@pytest.mark.parametrize("n", [0, 1, 5, 100, BATCHSIZE + 10])
def test_pickle_dict(n):
    value = dict(
        zip(itertools.product(string.ascii_letters, string.ascii_letters), range(n))
    )
    check(value)


def test_pickle_recursive_dict():
    # self referential
    obj = {}
    obj[0] = obj

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
        assert isinstance(obj2, dict)
        assert obj2[0] is obj2
        assert len(obj2) == 1

    # one level removed
    obj = {0: []}
    obj[0].append(obj)

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
        assert isinstance(obj2, dict)
        assert obj2[0][0] is obj2
        # Fix the cycle so `==` works, then test
        obj2[0].pop()
        assert obj2 == {0: []}


def test_pickle_highly_nested_list():
    obj = []
    for _ in range(66):
        obj = [obj]
    check(obj)


def test_pickle_large_memo():
    obj = [[1, 2, 3] for _ in range(2000)]
    check(obj)


def opcode_in_pickle(code, pickle):
    for op, dummy, dummy in pickletools.genops(pickle):
        if op.code == code.decode("latin-1"):
            return True
    return False


def test_pickle_memoize_false():
    obj = [[1], [2]]
    res = smolpickle.dumps(obj, memoize=False)
    assert not opcode_in_pickle(pickle.BINPUT, res)
    obj2 = smolpickle.loads(res)
    assert obj == obj2

    obj = [[]] * 2
    res = smolpickle.dumps(obj, memoize=False)
    assert not opcode_in_pickle(pickle.BINPUT, res)
    obj2 = smolpickle.loads(res)
    assert obj == obj2
    assert obj2[0] is not obj2[1]


def test_pickle_memoize_false_recursion_error():
    obj = []
    obj.append(obj)
    with pytest.raises(RecursionError):
        res = smolpickle.dumps(obj, memoize=False)


@pytest.mark.parametrize("cls", [bytes, bytearray])
def test_pickle_picklebuffer_no_callback(cls):
    sol = cls(b"hello")
    obj = smolpickle.PickleBuffer(sol)
    check(obj, sol)


@pytest.mark.parametrize("cls", [bytes, bytearray])
def test_dumps_buffer_callback(cls):
    sol = cls(b"hello")
    obj = smolpickle.PickleBuffer(sol)

    buffers = []
    smol_res = smolpickle.dumps(obj, buffer_callback=buffers.append)

    # roundtrip
    obj2 = smolpickle.loads(smol_res, buffers=buffers)
    assert obj2 is obj

    # smolpickle -> pickle
    obj3 = pickle.loads(smol_res, buffers=buffers)
    assert obj3 is obj

    # pickle -> smolpickle
    buffers = []
    pickle_res = pickle.dumps(obj, protocol=5, buffer_callback=buffers.append)
    obj4 = smolpickle.loads(pickle_res, buffers=buffers)
    assert obj4 is obj


def test_dumps_buffer_callback_errors():
    obj = pickle.PickleBuffer(b"hello")

    with pytest.raises(TypeError):
        smolpickle.dumps(obj, buffer_callback=object())

    with pytest.raises(ZeroDivisionError):
        smolpickle.dumps(obj, buffer_callback=lambda x: 1 / 0)


def test_loads_buffers_errors():
    obj = smolpickle.PickleBuffer(b"hello")
    data = smolpickle.dumps(obj, buffer_callback=lambda x: None)

    with pytest.raises(TypeError):
        smolpickle.loads(data, buffers=object())

    with pytest.raises(smolpickle.UnpicklingError):
        smolpickle.loads(data, buffers=[])


def test_getsizeof():
    a = sys.getsizeof(smolpickle.Pickler(buffer_size=64))
    b = sys.getsizeof(smolpickle.Pickler(buffer_size=128))
    assert b > a
    # Smoketest
    sys.getsizeof(smolpickle.Unpickler())
