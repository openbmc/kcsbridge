headers = include_directories('.')

fmt_dep = dependency('fmt', required: false)
if not fmt_dep.found()
  fmt_opts = import('cmake').subproject_options()
  fmt_opts.add_cmake_defines({
    'CMAKE_POSITION_INDEPENDENT_CODE': 'ON',
    'MASTER_PROJECT': 'OFF',
  })
  fmt_proj = import('cmake').subproject(
    'fmt',
    options: fmt_opts,
    required: false)
  assert(fmt_proj.found(), 'fmtlib is required')
  fmt_dep = fmt_proj.dependency('fmt')
endif


deps = [
  fmt_dep,
  dependency('stdplus', fallback: ['stdplus', 'stdplus_dep']),
  dependency('sdbusplus', fallback: ['sdbusplus', 'sdbusplus_dep']),
]

lib = static_library(
  'kcsbridged',
  'args.cpp',
  'cmd.cpp',
  'server.cpp',
  include_directories: headers,
  implicit_include_directories: false,
  dependencies: deps)

dep = declare_dependency(
  dependencies: deps,
  include_directories: headers,
  link_with: lib)

kcsbridged = executable(
  'kcsbridged',
  'main.cpp',
  implicit_include_directories: false,
  dependencies: [
    dep,
    dependency('sdeventplus', fallback: ['sdeventplus', 'sdeventplus_dep']),
    dependency('libsystemd'),
  ],
  install: true,
  install_dir: get_option('libexecdir'))
