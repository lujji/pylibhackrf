# setup.py
from distutils.core import setup, Extension

setup(name='py_hackrf',
      ext_modules=[
          Extension('py_hackrf',
                    ['py_hackrf.c'],
                    include_dirs=['/usr/include'],
                    define_macros=[('DEBUG', '1')],
                    library_dirs=['/usr/local/lib'],
                    libraries=['hackrf'])]
      )
