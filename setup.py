# setup.py
from setuptools import setup, Extension

setup(
    name="py_hackrf",
    ext_modules=[
        Extension(
            "py_hackrf",
            ["py_hackrf.c", "queue.c"],
            define_macros=[("DEBUG", "0")],
            extra_compile_args=["-O3"],
            # extra_link_args=['-fsanitize=address'],
            libraries=["hackrf", "pthread"],
        )
    ],
)
