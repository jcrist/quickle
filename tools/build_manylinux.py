import glob
import os
import subprocess
import sys

SCRIPT = """\
cd /tmp
export HOME=/tmp
for py in {pythons}; do
    "/opt/python/$py/bin/pip" wheel --no-deps --wheel-dir /tmp /dist/*.tar.gz
done
ls *.whl | xargs -n1 --verbose auditwheel repair --wheel-dir /dist
ls -al /dist
"""

PACKAGE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST_DIR = os.path.join(PACKAGE_DIR, "dist")


def fail(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)


def main():
    python_versions = "cp38-cp38"

    pythons = " ".join(python_versions.split(","))

    sdists = glob.glob(os.path.join(DIST_DIR, "*.tar.gz"))
    if not sdists:
        fail("Must build sdist beforehand")
    elif len(sdists) > 1:
        fail("Must have only one sdist built")

    subprocess.check_call(
        [
            "docker",
            "run",
            "-it",
            "--rm",
            "--volume",
            "{}:/dist:rw".format(DIST_DIR),
            "--user",
            "{}:{}".format(os.getuid(), os.getgid()),
            "quay.io/pypa/manylinux2010_x86_64:latest",
            "bash",
            "-o",
            "pipefail",
            "-euxc",
            SCRIPT.format(pythons=pythons),
        ]
    )


if __name__ == "__main__":
    main()
