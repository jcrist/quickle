import gc
from itertools import cycle
from timeit import default_timer
from collections import namedtuple

import msgpack
import orjson
import pickle
import quickle


class Person(quickle.Struct):
    first: str
    last: str
    age: int


PersonTuple = namedtuple("PersonTuple", ["first", "last", "age"])


data = [
    {"first": "Pecola", "last": "Breedlove", "age": 11},
    {"first": "Claudia", "last": "MacTeer", "age": 9},
    {"first": "Frieda", "last": "MacTeer", "age": 10},
]


def bench_overhead(data, niters):
    getdata = cycle(data).__next__
    gc.collect()
    gc.disable()
    start = default_timer()
    for _ in range(niters):
        getdata()
    stop = default_timer()
    gc.enable()
    return (stop - start) / niters


def timeit(func, data, niters):
    getdata = cycle(data).__next__
    gc.collect()
    gc.disable()
    start = default_timer()
    for _ in range(niters):
        func(getdata())
    stop = default_timer()
    gc.enable()
    return (stop - start) / niters


def bench(dumps, loads, data, niters):
    dumps_time = timeit(dumps, data, niters)
    loads_time = timeit(loads, [dumps(x) for x in data], niters)
    msg_size = len(dumps(data[0]))
    return dumps_time, loads_time, msg_size


def bench_msgpack(data, niters):
    packer = msgpack.Packer()
    return bench(packer.pack, msgpack.loads, data, niters)


def bench_orjson(data, niters):
    return bench(orjson.dumps, orjson.loads, data, niters)


def bench_pickle(data, niters):
    return bench(pickle.dumps, pickle.loads, data, niters)


def bench_pickle_namedtuple(data, niters):
    data = [PersonTuple(**d) for d in data]
    return bench(pickle.dumps, pickle.loads, data, niters)


def bench_quickle(data, niters):
    enc = quickle.Encoder(memoize=False)
    dec = quickle.Decoder()
    return bench(enc.dumps, dec.loads, data, niters)


def bench_quickle_structs(data, niters):
    enc = quickle.Encoder(registry=[Person], memoize=False)
    dec = quickle.Decoder(registry=[Person])
    data = [Person(**d) for d in data]
    return bench(enc.dumps, dec.loads, data, niters)


benchmarks = [
    ("msgpack", bench_msgpack, 2_000_000),
    ("orjson", bench_orjson, 2_000_000),
    ("pickle", bench_pickle, 1_000_000),
    ("pickle nametuples", bench_pickle_namedtuple, 1_000_000),
    ("quickle", bench_quickle, 2_000_000),
    ("quickle structs", bench_quickle_structs, 2_000_000),
]


def run(verbose=False):
    overhead = bench_overhead(data, 10_000_000)
    results = []
    for name, func, niters in benchmarks:
        if verbose:
            print(f"Bencmarking {name}...")
        dumps_time, loads_time, msg_size = func(data, niters)
        if verbose:
            print(f"  dumps: {dumps_time * 1e6:.2f} us")
            print(f"  loads: {loads_time * 1e6:.2f} us")
            print(f"  size: {msg_size} bytes")
        results.append((name, dumps_time - overhead, loads_time - overhead, msg_size))
    return results


def plot(results, path):
    import pandas as pd
    import altair as alt

    df = pd.DataFrame(results, columns=["benchmark", "dumps", "loads", "size"])
    times = df.melt(
        id_vars=["benchmark"],
        value_vars=["dumps", "loads"],
        var_name="type",
        value_name="time",
    )
    times = times.assign(time=times.time * 1e6)
    times = times.assign(tooltip=times.time.map("{:.2f} us".format))
    sort_order = list(
        df.assign(total=df.loads + df.dumps)
        .sort_values("total", ascending=False)
        .benchmark
    )

    time_plot = (
        alt.Chart(times)
        .mark_bar()
        .encode(
            x=alt.X("type", axis=alt.Axis(labels=False, ticks=False, title=None)),
            y=alt.Y("time:Q", title="time (us)", axis=alt.Axis(grid=False)),
            color=alt.Column("type"),
            column=alt.Column(
                "benchmark",
                sort=sort_order,
                title="",
                header=alt.Header(title=None, labelOrient="bottom"),
            ),
            tooltip=[alt.Tooltip("tooltip", title="time")],
        )
        .properties(width=50, height=200)
    )

    sizes = df.assign(type="size", tooltip=df["size"].map("{:} bytes".format))
    size_plot = (
        alt.Chart(sizes)
        .mark_bar()
        .encode(
            x=alt.X("type", axis=alt.Axis(ticks=False, labels=False, title=None)),
            y=alt.Y("size:Q", title="size (bytes)", axis=alt.Axis(grid=False)),
            column=alt.Column(
                "benchmark:O",
                sort=sort_order,
                title="",
                header=alt.Header(title=None, labelOrient="bottom"),
            ),
            tooltip=[alt.Tooltip("tooltip", title="size")],
            color=alt.value("#a6bcd4"),
        )
        .properties(width=50, height=100)
    )

    plot = (
        (time_plot & size_plot)
        .configure_view(stroke="transparent")
        .configure_title(anchor="middle")
        .properties(title="Small Messages")
    )
    print("Generating plot...")
    plot.save(path)
    print(f"Plot saved to {path}!")


def main():
    import argparse

    parser = argparse.ArgumentParser(description="Benchmark small messages")
    parser.add_argument(
        "--plot",
        metavar="PATH",
        default="",
        type=str,
        help="where to save a plot of the results",
    )
    args = parser.parse_args()
    results = run(verbose=True)
    if args.plot:
        plot(results, args.plot)


if __name__ == "__main__":
    main()
