from distutils.core import setup, Extension

module = Extension('pydrone',
                    libraries = ['mozjs','pthread'],
                    include_dirs = ['/usr/local/include/mozjs', '/usr/include/mozjs'],
                    sources = ['pydrone.c'])

setup (name = 'pydrone',
       author = 'Unbit',
       version = '0.2',
       description = 'pydrone',
       ext_modules = [module])
