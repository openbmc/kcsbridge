project(
  'kcsbridge',
  'cpp',
  version: '0.1',
  meson_version: '>=0.57.0',
  default_options: [
    'warning_level=3',
    'cpp_std=c++20',
  ])

subdir('src')

systemd = dependency('systemd')
if systemd.found()
  install_data(
    'phosphor-ipmi-kcs@.service',
    install_dir: systemd.get_variable(pkgconfig: 'systemdsystemunitdir'))
endif
