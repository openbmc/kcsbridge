project(
  'kcsbridge',
  'cpp',
  version: '0.1',
  meson_version: '>=0.57.0',
  default_options: [
    'warning_level=3',
    'cpp_std=c++20',
  ])

# CLI11 might not have a pkg-config. It is header only so just make
# sure we can access the needed symbols from the header.
cli11_dep = dependency('cli11', required: false)
has_cli11 = meson.get_compiler('cpp').has_header_symbol(
  'CLI/CLI.hpp',
  'CLI::App',
  dependencies: cli11_dep,
  required: false)
assert(has_cli11, 'CLI11 is required')

executable(
  'kcsbridged',
  'kcsbridged.cpp',
  implicit_include_directories: false,
  dependencies: [
    dependency(
      'boost',
      modules: [
        'coroutine',
        'context',
      ],
    ),
    cli11_dep,
    dependency('phosphor-logging'),
    dependency('sdbusplus'),
  ],
  cpp_args: [
    '-DBOOST_ASIO_DISABLE_THREADS',
    '-DBOOST_ALL_NO_LIB',
    '-DBOOST_SYSTEM_NO_DEPRECATED',
    '-DBOOST_ERROR_CODE_HEADER_ONLY',
    '-DBOOST_COROUTINES_NO_DEPRECATION_WARNING',
  ],
  install: true,
  install_dir: get_option('bindir'))

systemd = dependency('systemd')
if systemd.found()
  install_data(
    'phosphor-ipmi-kcs@.service',
    install_dir: systemd.get_variable(pkgconfig: 'systemdsystemunitdir'))
endif
