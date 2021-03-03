project('xdg-screensaver-shim', 'c',
        version: '0.0.1')

conf_data = configuration_data()
conf_data.set('VERSION', '"' + meson.project_version() + '"')
configure_file(output: 'project-config.h',
               configuration: conf_data)
conf_inc = include_directories('.')

deps = [dependency('dbus-1'), dependency('x11')]

executable('xdg-screensaver', 'xdg-screensaver-shim.c',
           dependencies: deps,
           include_directories: conf_inc,
           install: true)
