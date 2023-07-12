project(
  'kcsbridge',
  'cpp',
  version: '0.1',
  meson_version: '>=1.1.1',
  default_options: [
    'warning_level=3',
    'cpp_std=c++23',
  ])

subdir('src')

systemd = dependency('systemd')
if systemd.found()
  conf_data = configuration_data()
  conf_data.set('BIN', get_option('prefix') / get_option('libexecdir') / kcsbridged.name())
  configure_file(
    input: 'phosphor-ipmi-kcs@.service.in',
    output: 'phosphor-ipmi-kcs@.service',
    configuration: conf_data,
    install: true,
    install_dir: systemd.get_variable('systemdsystemunitdir'))
endif
