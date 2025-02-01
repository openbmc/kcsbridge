headers = include_directories('.')

deps = [
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
    dependencies: deps,
)

dep = declare_dependency(
    dependencies: deps,
    include_directories: headers,
    link_with: lib,
)

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
    install_dir: get_option('libexecdir'),
)
