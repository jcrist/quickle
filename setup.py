import os

from setuptools import setup, find_packages
from setuptools.extension import Extension

ext_modules = [Extension("quickle", ["quickle.c"])]

setup(
    name="quickle",
    version="0.0.4",
    maintainer="Jim Crist-Harif",
    maintainer_email="jcristharif@gmail.com",
    url="https://jcristharif.com/quickle/",
    project_urls={
        "Documentation": "https://jcristharif.com/quickle/",
        "Source": "https://github.com/jcrist/quickle/",
        "Issue Tracker": "https://github.com/jcrist/quickle/issues",
    },
    description="A quicker pickle",
    classifiers=[
        "License :: OSI Approved :: BSD License",
        "Programming Language :: Python :: 3.8",
    ],
    license="BSD",
    packages=find_packages(),
    ext_modules=ext_modules,
    long_description=(
        open("README.rst").read() if os.path.exists("README.rst") else ""
    ),
    python_requires=">=3.8",
    zip_safe=False,
)
