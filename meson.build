project('fuse-trace', 'c',
  version : '1.0.0',
  license : 'MIT',
  meson_version : '>=0.55.0',
  default_options : [
    'c_std=gnu11',
    'warning_level=3',
    'buildtype=release',
  ])

cc = meson.get_compiler('c')

# FUSE large-file support: 64-bit off_t.
add_project_arguments('-D_FILE_OFFSET_BITS=64', language : 'c')

# The lseek and copy_file_range callbacks require FUSE 3.8+.
fuse_dep = dependency('fuse3', version : '>=3.8')

executable('fuse-trace',
  sources : 'main.c',
  dependencies : fuse_dep,
  install : true)
